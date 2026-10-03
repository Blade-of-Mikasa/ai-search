#include "rag_core/http_service.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace multimodal::rag::core {
namespace {

class ServiceError final : public std::runtime_error {
public:
  ServiceError(int status, std::string message)
      : std::runtime_error(std::move(message)), status_(status) {}
  [[nodiscard]] int status() const noexcept { return status_; }

private:
  int status_;
};

const json::Value &Required(const json::Value &value, std::string_view key) {
  try {
    return value.At(key);
  } catch (const json::Error &error) {
    throw ServiceError(400, error.what());
  }
}

std::string String(const json::Value &value, std::string_view key,
                   std::string fallback = {}) {
  if (!value.Contains(key)) {
    return fallback;
  }
  try {
    return value.At(key).AsString();
  } catch (const json::Error &) {
    throw ServiceError(400, std::string(key) + " must be a string");
  }
}

std::uint64_t Uint64(const json::Value &value, std::string_view key,
                     std::uint64_t fallback = 0) {
  if (!value.Contains(key)) {
    return fallback;
  }
  try {
    return value.At(key).AsUint64();
  } catch (const json::Error &) {
    throw ServiceError(400, std::string(key) + " must be an unsigned integer");
  }
}

std::uint32_t Uint32(const json::Value &value, std::string_view key,
                     std::uint32_t fallback = 0) {
  const auto parsed = Uint64(value, key, fallback);
  if (parsed > std::numeric_limits<std::uint32_t>::max()) {
    throw ServiceError(400, std::string(key) + " exceeds uint32");
  }
  return static_cast<std::uint32_t>(parsed);
}

std::int64_t Int64(const json::Value &value, std::string_view key,
                   std::int64_t fallback = 0) {
  if (!value.Contains(key)) {
    return fallback;
  }
  try {
    return value.At(key).AsInt64();
  } catch (const json::Error &) {
    throw ServiceError(400, std::string(key) + " must be an integer");
  }
}

double Number(const json::Value &value, std::string_view key,
              double fallback = 0.0) {
  if (!value.Contains(key)) {
    return fallback;
  }
  try {
    return value.At(key).AsNumber();
  } catch (const json::Error &) {
    throw ServiceError(400, std::string(key) + " must be a number");
  }
}

bool Boolean(const json::Value &value, std::string_view key,
             bool fallback = false) {
  if (!value.Contains(key)) {
    return fallback;
  }
  try {
    return value.At(key).AsBool();
  } catch (const json::Error &) {
    throw ServiceError(400, std::string(key) + " must be a boolean");
  }
}

const json::Value::Array &Array(const json::Value &value,
                               std::string_view key) {
  try {
    return Required(value, key).AsArray();
  } catch (const json::Error &) {
    throw ServiceError(400, std::string(key) + " must be an array");
  }
}

std::vector<std::string> Strings(const json::Value &value,
                                 std::string_view key) {
  std::vector<std::string> output;
  if (!value.Contains(key)) {
    return output;
  }
  for (const auto &item : Array(value, key)) {
    try {
      output.push_back(item.AsString());
    } catch (const json::Error &) {
      throw ServiceError(400, std::string(key) + " must contain strings");
    }
  }
  return output;
}

std::vector<float> Floats(const json::Value &value, std::string_view key) {
  std::vector<float> output;
  if (!value.Contains(key)) {
    return output;
  }
  for (const auto &item : Array(value, key)) {
    try {
      output.push_back(static_cast<float>(item.AsNumber()));
    } catch (const json::Error &) {
      throw ServiceError(400, std::string(key) + " must contain numbers");
    }
  }
  return output;
}

std::map<std::string, std::string> Metadata(const json::Value &value) {
  std::map<std::string, std::string> output;
  if (!value.Contains("metadata")) {
    return output;
  }
  try {
    for (const auto &[key, item] : value.At("metadata").AsObject()) {
      output.emplace(key, item.AsString());
    }
  } catch (const json::Error &) {
    throw ServiceError(400, "metadata must map strings to strings");
  }
  return output;
}

std::string Meta(const std::map<std::string, std::string> &metadata,
                 const std::string &key) {
  const auto item = metadata.find(key);
  return item == metadata.end() ? std::string{} : item->second;
}

bool ParsePositiveUint32(const std::string &value, std::uint32_t &output) {
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() ||
      parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  output = static_cast<std::uint32_t>(parsed);
  return true;
}

bool ParseUint64(const std::string &value, std::uint64_t &output,
                 bool allow_zero) {
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() ||
      (!allow_zero && parsed == 0)) {
    return false;
  }
  output = parsed;
  return true;
}

Modality ParseModality(const json::Value &value) {
  const auto raw = Uint32(value, "modality");
  switch (raw) {
  case 1:
    return Modality::kDocument;
  case 2:
    return Modality::kImage;
  case 3:
    return Modality::kVideo;
  default:
    return Modality::kUnspecified;
  }
}

SourceScope ParseSourceScope(const json::Value &value) {
  const auto raw = Uint32(value, "source_scope");
  switch (raw) {
  case 1:
    return SourceScope::kLocal;
  case 2:
    return SourceScope::kWeb;
  default:
    return SourceScope::kUnspecified;
  }
}

std::uint32_t ModalityNumber(Modality modality) {
  return static_cast<std::uint32_t>(modality);
}

std::uint32_t SourceScopeNumber(SourceScope scope) {
  return static_cast<std::uint32_t>(scope);
}

json::Value StringArray(const std::vector<std::string> &values) {
  json::Value::Array output;
  output.reserve(values.size());
  for (const auto &value : values) output.emplace_back(value);
  return output;
}

json::Value StringObject(const std::map<std::string, std::string> &values) {
  json::Value::Object output;
  for (const auto &[key, value] : values) output.emplace(key, value);
  return output;
}

json::Value EvidenceJson(const EvidenceItem &item) {
  return json::Value::Object{
      {"evidence_id", item.evidence_id},
      {"content", item.content},
      {"modality", static_cast<std::uint64_t>(ModalityNumber(item.modality))},
      {"source_scope",
       static_cast<std::uint64_t>(SourceScopeNumber(item.source_scope))},
      {"title", item.title},
      {"source", item.source},
      {"url", item.url},
      {"published_at_unix_ms", item.published_at_unix_ms},
      {"retrieved_at_unix_ms", item.retrieved_at_unix_ms},
      {"score", item.score},
      {"metadata", StringObject(item.metadata)},
      {"content_sha256", item.content_sha256},
  };
}

EvidenceItem ParseEvidence(const json::Value &item) {
  return EvidenceItem{
      .evidence_id = String(item, "evidence_id"),
      .content = String(item, "content"),
      .modality = ParseModality(item),
      .source_scope = ParseSourceScope(item),
      .title = String(item, "title"),
      .source = String(item, "source"),
      .url = String(item, "url"),
      .published_at_unix_ms = Int64(item, "published_at_unix_ms"),
      .retrieved_at_unix_ms = Int64(item, "retrieved_at_unix_ms"),
      .score = Number(item, "score"),
      .metadata = Metadata(item),
      .content_sha256 = String(item, "content_sha256"),
  };
}

struct Route {
  std::string route_id;
  std::string query;
  SourceScope source_scope{SourceScope::kUnspecified};
  Modality modality{Modality::kUnspecified};
  std::uint32_t top_k{10};
  std::vector<float> dense_embedding;
  std::string embedding_model_id;
  std::string embedding_model_version;
};

Route ParseRoute(const json::Value &item) {
  return Route{
      .route_id = String(item, "route_id"),
      .query = String(item, "query"),
      .source_scope = ParseSourceScope(item),
      .modality = ParseModality(item),
      .top_k = Uint32(item, "top_k", 10),
      .dense_embedding = Floats(item, "dense_embedding"),
      .embedding_model_id = String(item, "embedding_model_id"),
      .embedding_model_version = String(item, "embedding_model_version"),
  };
}

void AddRouteError(json::Value::Array &errors, const Route &route,
                   std::string code, std::string message, bool retryable) {
  errors.emplace_back(json::Value::Object{
      {"route_id", route.route_id},
      {"code", std::move(code)},
      {"message", std::move(message)},
      {"retryable", retryable},
  });
}

json::Value ErrorBody(std::string code, std::string message) {
  return json::Value::Object{{"error", json::Value::Object{
                                          {"code", std::move(code)},
                                          {"message", std::move(message)},
                                      }}};
}

} // namespace

HttpCoreService::HttpCoreService(DocumentStore *document_store,
                                 ImageStore *image_store,
                                 VideoStore *video_store)
    : document_store_(document_store), image_store_(image_store),
      video_store_(video_store) {}

HttpResponse HttpCoreService::Handle(std::string_view method,
                                     std::string_view path,
                                     std::string_view body) {
  if (method == "GET" && path == "/health") {
    return {200, json::Value::Object{{"service", kCoreServiceName},
                                     {"version", kCoreServiceVersion},
                                     {"ready", true}}};
  }
  if (method != "POST" ||
      (path != "/v1/execute-plan" && path != "/v1/index-asset")) {
    return {404, ErrorBody("NOT_FOUND", "HTTP endpoint not found")};
  }
  try {
    const auto request = json::Value::Parse(body);
    if (!request.IsObject()) {
      throw ServiceError(400, "request body must be a JSON object");
    }
    return {200, path == "/v1/execute-plan" ? ExecutePlan(request)
                                             : IndexAsset(request)};
  } catch (const ServiceError &error) {
    return {error.status(), ErrorBody("INVALID_REQUEST", error.what())};
  } catch (const json::Error &error) {
    return {400, ErrorBody("INVALID_JSON", error.what())};
  } catch (const std::exception &error) {
    return {500, ErrorBody("INTERNAL_ERROR", error.what())};
  }
}

json::Value HttpCoreService::ExecutePlan(const json::Value &request) {
  const std::string request_id = String(request, "request_id");
  const std::string tenant_id = String(request, "tenant_id");
  if (request_id.empty() || tenant_id.empty()) {
    throw ServiceError(400, "request_id and tenant_id must not be empty");
  }
  const auto allowed_acl_ids = Strings(request, "allowed_acl_ids");
  std::vector<Route> routes;
  for (const auto &item : Array(request, "routes")) routes.push_back(ParseRoute(item));
  if (routes.size() > kMaxRouteCount) {
    throw ServiceError(400, "route count must not exceed 6");
  }
  if (std::any_of(routes.begin(), routes.end(), [](const Route &route) {
        return route.source_scope != SourceScope::kLocal;
      })) {
    throw ServiceError(400, "retrieval routes must use LOCAL");
  }
  if (!routes.empty() && allowed_acl_ids.empty()) {
    throw ServiceError(400, "allowed_acl_ids must not be empty for local retrieval");
  }

  std::vector<EvidenceItem> evidence_items;
  for (const auto &item : Array(request, "external_evidence")) {
    auto evidence = ParseEvidence(item);
    if (evidence.source_scope != SourceScope::kWeb) {
      throw ServiceError(400, "external_evidence must use WEB source_scope");
    }
    evidence_items.push_back(std::move(evidence));
  }
  json::Value::Array route_errors;
  for (const auto &route : routes) {
    if (route.modality == Modality::kDocument) {
      if (document_store_ == nullptr) {
        AddRouteError(route_errors, route, "DOCUMENT_STORE_UNAVAILABLE",
                      "document store is not configured", true);
        continue;
      }
      try {
        DocumentQuery query{tenant_id, allowed_acl_ids, route.query,
                            route.dense_embedding, route.embedding_model_id,
                            route.embedding_model_version, route.top_k};
        for (const auto &hit : document_store_->HybridSearch(query)) {
          evidence_items.push_back(EvidenceItem{
              .evidence_id = hit.chunk_id,
              .content = hit.content,
              .modality = Modality::kDocument,
              .source_scope = SourceScope::kLocal,
              .title = hit.title,
              .source = hit.object_key,
              .url = {},
              .score = hit.score,
              .metadata = {{"asset_id", hit.asset_id},
                           {"asset_version_id", hit.asset_version_id},
                           {"ordinal", std::to_string(hit.ordinal)},
                           {"page_number", std::to_string(hit.page_number)},
                           {"route_id", route.route_id}},
              .content_sha256 = hit.content_sha256,
          });
        }
      } catch (const DocumentStoreError &error) {
        AddRouteError(route_errors, route, "DOCUMENT_RETRIEVAL_FAILED",
                      error.what(), error.retryable());
      }
      continue;
    }
    if (route.modality == Modality::kImage) {
      if (image_store_ == nullptr) {
        AddRouteError(route_errors, route, "IMAGE_STORE_UNAVAILABLE",
                      "image store is not configured", true);
        continue;
      }
      try {
        ImageQuery query{tenant_id, allowed_acl_ids, route.query,
                         route.dense_embedding, route.embedding_model_id,
                         route.embedding_model_version, route.top_k};
        for (const auto &hit : image_store_->HybridSearch(query)) {
          evidence_items.push_back(EvidenceItem{
              .evidence_id = hit.image_id,
              .content = hit.content,
              .modality = Modality::kImage,
              .source_scope = SourceScope::kLocal,
              .title = hit.caption,
              .source = hit.object_key,
              .url = {},
              .score = hit.score,
              .metadata = {{"asset_id", hit.asset_id},
                           {"asset_version_id", hit.asset_version_id},
                           {"media_type", hit.media_type},
                           {"width", std::to_string(hit.width)},
                           {"height", std::to_string(hit.height)},
                           {"ocr_text", hit.ocr_text},
                           {"route_id", route.route_id}},
              .content_sha256 = hit.content_sha256,
          });
        }
      } catch (const ImageStoreError &error) {
        AddRouteError(route_errors, route, "IMAGE_RETRIEVAL_FAILED",
                      error.what(), error.retryable());
      }
      continue;
    }
    if (route.modality == Modality::kVideo) {
      if (video_store_ == nullptr) {
        AddRouteError(route_errors, route, "VIDEO_STORE_UNAVAILABLE",
                      "video store is not configured", true);
        continue;
      }
      try {
        VideoQuery query{tenant_id, allowed_acl_ids, route.query,
                         route.dense_embedding, route.embedding_model_id,
                         route.embedding_model_version, route.top_k};
        for (const auto &hit : video_store_->HybridSearch(query)) {
          evidence_items.push_back(EvidenceItem{
              .evidence_id = hit.segment_id,
              .content = hit.content,
              .modality = Modality::kVideo,
              .source_scope = SourceScope::kLocal,
              .title = hit.caption,
              .source = hit.object_key,
              .url = {},
              .score = hit.score,
              .metadata = {{"asset_id", hit.asset_id},
                           {"asset_version_id", hit.asset_version_id},
                           {"ordinal", std::to_string(hit.ordinal)},
                           {"media_type", hit.media_type},
                           {"duration_ms", std::to_string(hit.duration_ms)},
                           {"width", std::to_string(hit.width)},
                           {"height", std::to_string(hit.height)},
                           {"start_ms", std::to_string(hit.start_ms)},
                           {"end_ms", std::to_string(hit.end_ms)},
                           {"keyframe_ms", std::to_string(hit.keyframe_ms)},
                           {"ocr_text", hit.ocr_text},
                           {"transcript", hit.transcript},
                           {"route_id", route.route_id}},
              .content_sha256 = hit.content_sha256,
          });
        }
      } catch (const VideoStoreError &error) {
        AddRouteError(route_errors, route, "VIDEO_RETRIEVAL_FAILED",
                      error.what(), error.retryable());
      }
    }
  }

  EvidenceContextOptions options;
  options.context_token_budget =
      Uint32(request, "context_token_budget", options.context_token_budget);
  options.max_evidence_tokens =
      Uint32(request, "max_evidence_tokens", options.max_evidence_tokens);
  EvidenceContextResult result;
  try {
    result = evidence_processor_.Process(evidence_items, options);
  } catch (const EvidenceProcessorError &error) {
    throw ServiceError(400, error.what());
  }

  json::Value::Array evidence;
  for (const auto &item : result.evidence) evidence.push_back(EvidenceJson(item));
  json::Value::Array conflicts;
  for (const auto &item : result.conflicts) {
    conflicts.emplace_back(json::Value::Object{
        {"evidence_ids", StringArray(item.evidence_ids)},
        {"type", item.type},
        {"reason", item.reason},
    });
  }
  json::Value::Array citations;
  for (const auto &item : result.citations) {
    citations.emplace_back(json::Value::Object{
        {"citation_id", static_cast<std::uint64_t>(item.citation_id)},
        {"evidence_id", item.evidence_id},
        {"source", item.source},
        {"url", item.url},
        {"title", item.title},
        {"modality", static_cast<std::uint64_t>(ModalityNumber(item.modality))},
        {"metadata", StringObject(item.metadata)},
    });
  }
  json::Value::Array decisions;
  for (const auto &item : result.decisions) {
    decisions.emplace_back(json::Value::Object{
        {"evidence_id", item.evidence_id},
        {"disposition", item.disposition},
        {"representative_evidence_id", item.representative_evidence_id},
        {"reason", item.reason},
    });
  }
  return json::Value::Object{
      {"request_id", request_id},
      {"evidence", std::move(evidence)},
      {"conflicts", std::move(conflicts)},
      {"context", result.context},
      {"citations", std::move(citations)},
      {"evidence_decisions", std::move(decisions)},
      {"context_token_count",
       static_cast<std::uint64_t>(result.context_token_count)},
      {"context_truncated", result.context_truncated},
      {"token_count_method", result.token_count_method},
      {"route_errors", route_errors},
      {"partial_failure", !route_errors.empty()},
  };
}

json::Value HttpCoreService::IndexAsset(const json::Value &request) {
  const std::string request_id = String(request, "request_id");
  const std::string tenant_id = String(request, "tenant_id");
  const std::string acl_id = String(request, "acl_id");
  const std::string asset_id = String(request, "asset_id");
  const std::string asset_version_id = String(request, "asset_version_id");
  const std::uint64_t asset_version = Uint64(request, "asset_version");
  const std::string object_key = String(request, "object_key");
  const auto &units = Array(request, "units");
  const bool append = Boolean(request, "append_to_asset_version");
  if (request_id.empty() || tenant_id.empty() || acl_id.empty() ||
      asset_id.empty() || asset_version_id.empty() || asset_version == 0 ||
      object_key.empty() || units.empty()) {
    throw ServiceError(400, "index asset identity and units must not be empty");
  }
  const Modality modality = ParseModality(units.front());
  if (std::any_of(units.begin(), units.end(), [modality](const auto &item) {
        return ParseModality(item) != modality;
      })) {
    throw ServiceError(400, "index units must share one modality");
  }

  std::string collection_alias;
  if (modality == Modality::kImage) {
    if (image_store_ == nullptr) throw ServiceError(412, "image store is not configured");
    if (units.size() != 1 || append) {
      throw ServiceError(400, "one image version requires one replacement unit");
    }
    const auto &unit = units.front();
    const auto metadata = Metadata(unit);
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!ParsePositiveUint32(Meta(metadata, "width"), width) ||
        !ParsePositiveUint32(Meta(metadata, "height"), height)) {
      throw ServiceError(400, "image width and height must be positive integers");
    }
    ImageRecord image{
        .image_id = String(unit, "unit_id"),
        .tenant_id = tenant_id,
        .acl_id = acl_id,
        .asset_id = asset_id,
        .asset_version_id = asset_version_id,
        .asset_version = asset_version,
        .object_key = object_key,
        .media_type = Meta(metadata, "media_type"),
        .width = width,
        .height = height,
        .caption = String(unit, "title"),
        .ocr_text = Meta(metadata, "ocr_text"),
        .content = String(unit, "content"),
        .content_sha256 = String(unit, "content_sha256"),
        .dense_embedding = Floats(unit, "dense_embedding"),
        .embedding_model_id = String(unit, "embedding_model_id"),
        .embedding_model_version = String(unit, "embedding_model_version"),
        .vision_model_id = Meta(metadata, "vision_model_id"),
        .vision_model_version = Meta(metadata, "vision_model_version"),
    };
    try {
      collection_alias = image_store_->ReplaceAssetVersion(image);
    } catch (const ImageStoreError &error) {
      throw ServiceError(error.retryable() ? 503 : 400, error.what());
    }
  } else if (modality == Modality::kVideo) {
    if (video_store_ == nullptr) throw ServiceError(412, "video store is not configured");
    std::vector<VideoSegment> segments;
    for (const auto &unit : units) {
      const auto metadata = Metadata(unit);
      std::uint64_t duration = 0, start = 0, end = 0, keyframe = 0;
      std::uint32_t width = 0, height = 0;
      if (!ParseUint64(Meta(metadata, "duration_ms"), duration, false) ||
          !ParsePositiveUint32(Meta(metadata, "width"), width) ||
          !ParsePositiveUint32(Meta(metadata, "height"), height) ||
          !ParseUint64(Meta(metadata, "start_ms"), start, true) ||
          !ParseUint64(Meta(metadata, "end_ms"), end, false) ||
          !ParseUint64(Meta(metadata, "keyframe_ms"), keyframe, true)) {
        throw ServiceError(400, "video dimensions and timestamps must be integers");
      }
      VideoSegment segment{
          .segment_id = String(unit, "unit_id"), .tenant_id = tenant_id,
          .acl_id = acl_id, .asset_id = asset_id,
          .asset_version_id = asset_version_id, .asset_version = asset_version,
          .object_key = object_key, .ordinal = Uint32(unit, "ordinal"),
          .media_type = Meta(metadata, "media_type"), .duration_ms = duration,
          .width = width, .height = height, .start_ms = start, .end_ms = end,
          .keyframe_ms = keyframe, .caption = Meta(metadata, "caption"),
          .ocr_text = Meta(metadata, "ocr_text"),
          .transcript = Meta(metadata, "transcript"),
          .content = String(unit, "content"),
          .content_sha256 = String(unit, "content_sha256"),
          .dense_embedding = Floats(unit, "dense_embedding"),
          .embedding_model_id = String(unit, "embedding_model_id"),
          .embedding_model_version = String(unit, "embedding_model_version"),
          .vision_model_id = Meta(metadata, "vision_model_id"),
          .vision_model_version = Meta(metadata, "vision_model_version"),
          .speech_model_id = Meta(metadata, "speech_model_id"),
          .speech_model_version = Meta(metadata, "speech_model_version"),
      };
      const auto errors = Validate(segment);
      if (!errors.empty()) throw ServiceError(400, errors.front());
      segments.push_back(std::move(segment));
    }
    try {
      collection_alias = append ? video_store_->AppendAssetVersion(segments)
                                : video_store_->ReplaceAssetVersion(segments);
    } catch (const VideoStoreError &error) {
      throw ServiceError(error.retryable() ? 503 : 400, error.what());
    }
  } else if (modality == Modality::kDocument) {
    if (document_store_ == nullptr) throw ServiceError(412, "document store is not configured");
    std::vector<DocumentChunk> chunks;
    for (const auto &unit : units) {
      DocumentChunk chunk{
          .chunk_id = String(unit, "unit_id"), .tenant_id = tenant_id,
          .acl_id = acl_id, .asset_id = asset_id,
          .asset_version_id = asset_version_id, .asset_version = asset_version,
          .object_key = object_key, .ordinal = Uint32(unit, "ordinal"),
          .page_number = Uint32(unit, "page_number"),
          .title = String(unit, "title"), .content = String(unit, "content"),
          .content_sha256 = String(unit, "content_sha256"),
          .dense_embedding = Floats(unit, "dense_embedding"),
          .embedding_model_id = String(unit, "embedding_model_id"),
          .embedding_model_version = String(unit, "embedding_model_version"),
      };
      const auto errors = Validate(chunk);
      if (!errors.empty()) throw ServiceError(400, errors.front());
      chunks.push_back(std::move(chunk));
    }
    try {
      collection_alias = append ? document_store_->AppendAssetVersion(chunks)
                                : document_store_->ReplaceAssetVersion(chunks);
    } catch (const DocumentStoreError &error) {
      throw ServiceError(error.retryable() ? 503 : 400, error.what());
    }
  } else {
    throw ServiceError(400, "units must be document, image, or video");
  }

  return json::Value::Object{
      {"request_id", request_id},
      {"asset_id", asset_id},
      {"asset_version", asset_version},
      {"indexed_unit_count", static_cast<std::uint64_t>(units.size())},
      {"collection_alias", collection_alias},
  };
}

} // namespace multimodal::rag::core
