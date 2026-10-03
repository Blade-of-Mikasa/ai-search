#pragma once

#include "rag_core/document_store.h"
#include "rag_core/evidence.h"
#include "rag_core/image_store.h"
#include "rag_core/json.h"
#include "rag_core/video_store.h"

#include <atomic>
#include <cstddef>
#include <string>

namespace multimodal::rag::core {

inline constexpr char kCoreServiceName[] = "nano-ai-search-core";
inline constexpr char kCoreServiceVersion[] = "0.2.0";

struct HttpResponse {
  int status{200};
  json::Value body{json::Value::Object{}};
};

class HttpCoreService final {
public:
  explicit HttpCoreService(DocumentStore *document_store,
                           ImageStore *image_store,
                           VideoStore *video_store);

  [[nodiscard]] HttpResponse Handle(std::string_view method,
                                    std::string_view path,
                                    std::string_view body);

private:
  [[nodiscard]] json::Value ExecutePlan(const json::Value &request);
  [[nodiscard]] json::Value IndexAsset(const json::Value &request);

  DocumentStore *document_store_;
  ImageStore *image_store_;
  VideoStore *video_store_;
  EvidenceProcessor evidence_processor_;
};

class HttpServer final {
public:
  HttpServer(std::string host, std::uint16_t port, HttpCoreService *service,
             std::size_t max_body_bytes = 8'000'000);
  ~HttpServer();

  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;

  void Run();
  void Stop();
  [[nodiscard]] std::uint16_t port() const noexcept;

private:
  std::string host_;
  std::uint16_t port_;
  HttpCoreService *service_;
  std::size_t max_body_bytes_;
  int socket_{-1};
  std::atomic<bool> stopping_{false};
};

} // namespace multimodal::rag::core
