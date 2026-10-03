#include "rag_core/document_store.h"
#include "rag_core/http_service.h"
#include "rag_core/image_store.h"
#include "rag_core/video_store.h"

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic<multimodal::rag::core::HttpServer *> server{nullptr};

void HandleSignal(int) {
  auto *running = server.load(std::memory_order_relaxed);
  if (running != nullptr) running->Stop();
}

struct ListenAddress {
  std::string host{"127.0.0.1"};
  std::uint16_t port{8081};
};

ListenAddress ParseArguments(int argc, char *argv[]) {
  ListenAddress output;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument != "--listen" || index + 1 >= argc) {
      throw std::invalid_argument("usage: nano_core [--listen HOST:PORT]");
    }
    const std::string value = argv[++index];
    const auto separator = value.rfind(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 == value.size()) {
      throw std::invalid_argument("listen address must be HOST:PORT");
    }
    output.host = value.substr(0, separator);
    unsigned int port = 0;
    const auto raw = value.substr(separator + 1);
    const auto [end, error] =
        std::from_chars(raw.data(), raw.data() + raw.size(), port);
    if (error != std::errc{} || end != raw.data() + raw.size() ||
        port > 65'535U) {
      throw std::invalid_argument("listen port must be between 0 and 65535");
    }
    output.port = static_cast<std::uint16_t>(port);
  }
  return output;
}

} // namespace

int main(int argc, char *argv[]) {
  ListenAddress listen;
  try {
    listen = ParseArguments(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }

  multimodal::rag::core::InMemoryDocumentStore documents;
  multimodal::rag::core::InMemoryImageStore images;
  multimodal::rag::core::InMemoryVideoStore videos;
  multimodal::rag::core::HttpCoreService service(&documents, &images, &videos);
  multimodal::rag::core::HttpServer http(listen.host, listen.port, &service);
  server.store(&http, std::memory_order_relaxed);
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  try {
    std::cout << "NANO_CORE_STARTING address=http://" << listen.host << ':'
              << listen.port << '\n';
    std::cout.flush();
    http.Run();
  } catch (const std::exception &error) {
    std::cerr << "nano core failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  server.store(nullptr, std::memory_order_relaxed);
  return EXIT_SUCCESS;
}
