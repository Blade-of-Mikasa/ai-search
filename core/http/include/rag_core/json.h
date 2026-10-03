#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace multimodal::rag::json {

class Error final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class Value final {
public:
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value>;

  Value();
  Value(std::nullptr_t);
  Value(bool value);
  Value(double value);
  Value(std::int64_t value);
  Value(std::uint64_t value);
  Value(std::string value);
  Value(const char *value);
  Value(Array value);
  Value(Object value);

  [[nodiscard]] bool IsNull() const;
  [[nodiscard]] bool IsBool() const;
  [[nodiscard]] bool IsNumber() const;
  [[nodiscard]] bool IsString() const;
  [[nodiscard]] bool IsArray() const;
  [[nodiscard]] bool IsObject() const;

  [[nodiscard]] bool AsBool() const;
  [[nodiscard]] double AsNumber() const;
  [[nodiscard]] std::int64_t AsInt64() const;
  [[nodiscard]] std::uint64_t AsUint64() const;
  [[nodiscard]] const std::string &AsString() const;
  [[nodiscard]] const Array &AsArray() const;
  [[nodiscard]] const Object &AsObject() const;
  [[nodiscard]] Array &AsArray();
  [[nodiscard]] Object &AsObject();

  [[nodiscard]] bool Contains(std::string_view key) const;
  [[nodiscard]] const Value &At(std::string_view key) const;
  Value &operator[](std::string key);

  [[nodiscard]] std::string Dump() const;
  [[nodiscard]] static Value Parse(std::string_view input);

private:
  using Storage =
      std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
  Storage value_;
};

} // namespace multimodal::rag::json
