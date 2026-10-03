#include "rag_core/http_service.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>

namespace multimodal::rag::core {
namespace {

constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

std::string StatusText(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 411:
    return "Length Required";
  case 412:
    return "Precondition Failed";
  case 413:
    return "Content Too Large";
  case 500:
    return "Internal Server Error";
  case 503:
    return "Service Unavailable";
  default:
    return "Error";
  }
}

void SendAll(int socket, std::string_view payload) {
  std::size_t sent = 0;
  while (sent < payload.size()) {
    const ssize_t result =
        ::send(socket, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return;
    }
    sent += static_cast<std::size_t>(result);
  }
}

void SendResponse(int socket, const HttpResponse &response) {
  const std::string body = response.body.Dump();
  const std::string headers =
      "HTTP/1.1 " + std::to_string(response.status) + " " +
      StatusText(response.status) + "\r\nContent-Type: application/json; "
      "charset=utf-8\r\nContent-Length: " +
      std::to_string(body.size()) +
      "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
  SendAll(socket, headers);
  SendAll(socket, body);
}

std::string Lower(std::string_view value) {
  std::string output(value);
  std::transform(output.begin(), output.end(), output.begin(),
                 [](unsigned char item) {
                   return static_cast<char>(std::tolower(item));
                 });
  return output;
}

std::size_t ContentLength(std::string_view headers, bool require_length) {
  std::size_t offset = headers.find("\r\n") + 2;
  bool found = false;
  std::size_t length = 0;
  while (offset < headers.size()) {
    const std::size_t end = headers.find("\r\n", offset);
    if (end == std::string_view::npos || end == offset) {
      break;
    }
    const std::string_view line = headers.substr(offset, end - offset);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos && Lower(line.substr(0, colon)) ==
                                              "content-length") {
      std::string_view raw = line.substr(colon + 1);
      while (!raw.empty() && raw.front() == ' ') raw.remove_prefix(1);
      const auto [parsed_end, error] =
          std::from_chars(raw.data(), raw.data() + raw.size(), length);
      if (error != std::errc{} || parsed_end != raw.data() + raw.size()) {
        throw std::invalid_argument("invalid Content-Length");
      }
      found = true;
    }
    offset = end + 2;
  }
  if (require_length && !found) {
    throw std::length_error("missing Content-Length");
  }
  return length;
}

HttpResponse RequestError(int status, std::string message) {
  return {status,
          json::Value::Object{{"error", json::Value::Object{
                                           {"code", "HTTP_ERROR"},
                                           {"message", std::move(message)},
                                       }}}};
}

void HandleConnection(int client, HttpCoreService *service,
                      std::size_t max_body_bytes) {
  timeval timeout{.tv_sec = 30, .tv_usec = 0};
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  std::string request;
  std::array<char, 16 * 1024> buffer{};
  std::size_t header_end = std::string::npos;
  while ((header_end = request.find("\r\n\r\n")) == std::string::npos) {
    const ssize_t received = ::recv(client, buffer.data(), buffer.size(), 0);
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) return;
    request.append(buffer.data(), static_cast<std::size_t>(received));
    if (request.size() > kMaxHeaderBytes) {
      SendResponse(client, RequestError(413, "HTTP headers are too large"));
      return;
    }
  }
  header_end += 4;
  const std::size_t first_line_end = request.find("\r\n");
  const std::string_view first_line(request.data(), first_line_end);
  const std::size_t first_space = first_line.find(' ');
  const std::size_t second_space = first_line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos ||
      second_space == std::string_view::npos) {
    SendResponse(client, RequestError(400, "invalid HTTP request line"));
    return;
  }
  const std::string method(first_line.substr(0, first_space));
  const std::string path(first_line.substr(first_space + 1,
                                           second_space - first_space - 1));
  std::size_t content_length = 0;
  try {
    content_length = ContentLength(
        std::string_view(request.data(), header_end), method == "POST");
  } catch (const std::length_error &error) {
    SendResponse(client, RequestError(411, error.what()));
    return;
  } catch (const std::invalid_argument &error) {
    SendResponse(client, RequestError(400, error.what()));
    return;
  }
  if (content_length > max_body_bytes) {
    SendResponse(client, RequestError(413, "request body exceeds server limit"));
    return;
  }
  const std::size_t expected_size = header_end + content_length;
  while (request.size() < expected_size) {
    const std::size_t remaining = expected_size - request.size();
    const ssize_t received =
        ::recv(client, buffer.data(), std::min(buffer.size(), remaining), 0);
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) {
      SendResponse(client, RequestError(400, "request body ended early"));
      return;
    }
    request.append(buffer.data(), static_cast<std::size_t>(received));
  }
  SendResponse(client,
               service->Handle(method, path,
                               std::string_view(request).substr(header_end,
                                                                content_length)));
}

int CreateSocket(const std::string &host, std::uint16_t port,
                 std::uint16_t &bound_port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  addrinfo *addresses = nullptr;
  const std::string port_text = std::to_string(port);
  const int lookup = getaddrinfo(host.c_str(), port_text.c_str(), &hints,
                                 &addresses);
  if (lookup != 0) {
    throw std::runtime_error(std::string("cannot resolve listen host: ") +
                             gai_strerror(lookup));
  }
  int socket = -1;
  for (addrinfo *address = addresses; address != nullptr;
       address = address->ai_next) {
    socket = ::socket(address->ai_family, address->ai_socktype,
                      address->ai_protocol);
    if (socket < 0) continue;
    const int enabled = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (::bind(socket, address->ai_addr, address->ai_addrlen) == 0 &&
        ::listen(socket, 128) == 0) {
      break;
    }
    ::close(socket);
    socket = -1;
  }
  freeaddrinfo(addresses);
  if (socket < 0) {
    throw std::runtime_error("cannot bind HTTP listen address: " +
                             std::string(std::strerror(errno)));
  }
  sockaddr_storage bound{};
  socklen_t bound_size = sizeof(bound);
  if (getsockname(socket, reinterpret_cast<sockaddr *>(&bound), &bound_size) <
      0) {
    ::close(socket);
    throw std::runtime_error("cannot inspect HTTP listen socket");
  }
  bound_port = bound.ss_family == AF_INET
                   ? ntohs(reinterpret_cast<sockaddr_in *>(&bound)->sin_port)
                   : ntohs(reinterpret_cast<sockaddr_in6 *>(&bound)->sin6_port);
  return socket;
}

} // namespace

HttpServer::HttpServer(std::string host, std::uint16_t port,
                       HttpCoreService *service, std::size_t max_body_bytes)
    : host_(std::move(host)), port_(port), service_(service),
      max_body_bytes_(max_body_bytes) {
  if (host_.empty() || service_ == nullptr || max_body_bytes_ == 0) {
    throw std::invalid_argument("invalid HTTP server configuration");
  }
}

HttpServer::~HttpServer() { Stop(); }

void HttpServer::Run() {
  socket_ = CreateSocket(host_, port_, port_);
  stopping_.store(false);
  while (!stopping_.load()) {
    pollfd descriptor{.fd = socket_, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&descriptor, 1, 250);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) throw std::runtime_error("HTTP poll failed");
    if (ready == 0) continue;
    const int client = ::accept(socket_, nullptr, nullptr);
    if (client < 0 && errno == EINTR) continue;
    if (client < 0) {
      if (stopping_.load()) break;
      continue;
    }
    HandleConnection(client, service_, max_body_bytes_);
    ::close(client);
  }
}

void HttpServer::Stop() {
  stopping_.store(true);
  if (socket_ >= 0) {
    ::shutdown(socket_, SHUT_RDWR);
    ::close(socket_);
    socket_ = -1;
  }
}

std::uint16_t HttpServer::port() const noexcept { return port_; }

} // namespace multimodal::rag::core
