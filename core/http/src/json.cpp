#include "rag_core/json.h"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace multimodal::rag::json {
namespace {

class Parser final {
public:
  explicit Parser(std::string_view input) : input_(input) {}

  Value ParseDocument() {
    SkipWhitespace();
    Value result = ParseValue();
    SkipWhitespace();
    if (position_ != input_.size()) {
      Fail("unexpected trailing JSON content");
    }
    return result;
  }

private:
  [[noreturn]] void Fail(const std::string &message) const {
    throw Error(message + " at byte " + std::to_string(position_));
  }

  void SkipWhitespace() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\n' ||
            input_[position_] == '\r' || input_[position_] == '\t')) {
      ++position_;
    }
  }

  bool Consume(char expected) {
    if (position_ < input_.size() && input_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

  Value ParseValue() {
    if (position_ >= input_.size()) {
      Fail("unexpected end of JSON");
    }
    switch (input_[position_]) {
    case 'n':
      return ParseLiteral("null", Value{});
    case 't':
      return ParseLiteral("true", Value{true});
    case 'f':
      return ParseLiteral("false", Value{false});
    case '"':
      return Value{ParseString()};
    case '[':
      return ParseArray();
    case '{':
      return ParseObject();
    default:
      if (input_[position_] == '-' ||
          (input_[position_] >= '0' && input_[position_] <= '9')) {
        return ParseNumber();
      }
      Fail("unexpected JSON token");
    }
  }

  Value ParseLiteral(std::string_view literal, Value value) {
    if (input_.substr(position_, literal.size()) != literal) {
      Fail("invalid JSON literal");
    }
    position_ += literal.size();
    return value;
  }

  static int Hex(char value) {
    if (value >= '0' && value <= '9') {
      return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
      return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
      return value - 'A' + 10;
    }
    return -1;
  }

  std::uint32_t ParseCodeUnit() {
    if (position_ + 4 > input_.size()) {
      Fail("truncated unicode escape");
    }
    std::uint32_t code = 0;
    for (int index = 0; index < 4; ++index) {
      const int digit = Hex(input_[position_++]);
      if (digit < 0) {
        Fail("invalid unicode escape");
      }
      code = (code << 4U) | static_cast<std::uint32_t>(digit);
    }
    return code;
  }

  static void AppendUtf8(std::string &output, std::uint32_t codepoint) {
    if (codepoint <= 0x7FU) {
      output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
      output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
      output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
      output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
      output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
      output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
      output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
      output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
      output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
      output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
  }

  std::string ParseString() {
    if (!Consume('"')) {
      Fail("expected string");
    }
    std::string output;
    while (position_ < input_.size()) {
      const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
      if (byte == '"') {
        return output;
      }
      if (byte < 0x20U) {
        Fail("unescaped control character in string");
      }
      if (byte != '\\') {
        output.push_back(static_cast<char>(byte));
        continue;
      }
      if (position_ >= input_.size()) {
        Fail("truncated string escape");
      }
      switch (input_[position_++]) {
      case '"':
        output.push_back('"');
        break;
      case '\\':
        output.push_back('\\');
        break;
      case '/':
        output.push_back('/');
        break;
      case 'b':
        output.push_back('\b');
        break;
      case 'f':
        output.push_back('\f');
        break;
      case 'n':
        output.push_back('\n');
        break;
      case 'r':
        output.push_back('\r');
        break;
      case 't':
        output.push_back('\t');
        break;
      case 'u': {
        std::uint32_t codepoint = ParseCodeUnit();
        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
          if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
              input_[position_ + 1] != 'u') {
            Fail("high surrogate is missing its low surrogate");
          }
          position_ += 2;
          const std::uint32_t low = ParseCodeUnit();
          if (low < 0xDC00U || low > 0xDFFFU) {
            Fail("invalid low surrogate");
          }
          codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) +
                      (low - 0xDC00U);
        } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
          Fail("unexpected low surrogate");
        }
        AppendUtf8(output, codepoint);
        break;
      }
      default:
        Fail("invalid string escape");
      }
    }
    Fail("unterminated string");
  }

  Value ParseArray() {
    Consume('[');
    Value::Array output;
    SkipWhitespace();
    if (Consume(']')) {
      return output;
    }
    while (true) {
      SkipWhitespace();
      output.push_back(ParseValue());
      SkipWhitespace();
      if (Consume(']')) {
        return output;
      }
      if (!Consume(',')) {
        Fail("expected comma in array");
      }
    }
  }

  Value ParseObject() {
    Consume('{');
    Value::Object output;
    SkipWhitespace();
    if (Consume('}')) {
      return output;
    }
    while (true) {
      SkipWhitespace();
      if (position_ >= input_.size() || input_[position_] != '"') {
        Fail("expected object key");
      }
      std::string key = ParseString();
      SkipWhitespace();
      if (!Consume(':')) {
        Fail("expected colon after object key");
      }
      SkipWhitespace();
      if (!output.emplace(std::move(key), ParseValue()).second) {
        Fail("duplicate object key");
      }
      SkipWhitespace();
      if (Consume('}')) {
        return output;
      }
      if (!Consume(',')) {
        Fail("expected comma in object");
      }
    }
  }

  Value ParseNumber() {
    const std::size_t start = position_;
    Consume('-');
    if (Consume('0')) {
      if (position_ < input_.size() && input_[position_] >= '0' &&
          input_[position_] <= '9') {
        Fail("number has a leading zero");
      }
    } else {
      const std::size_t digits = position_;
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
      if (digits == position_) {
        Fail("invalid number");
      }
    }
    if (Consume('.')) {
      const std::size_t digits = position_;
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
      if (digits == position_) {
        Fail("fraction has no digits");
      }
    }
    if (position_ < input_.size() &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() &&
          (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      const std::size_t digits = position_;
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
      if (digits == position_) {
        Fail("exponent has no digits");
      }
    }
    double output = 0.0;
    const auto token = input_.substr(start, position_ - start);
    const auto [end, error] =
        std::from_chars(token.data(), token.data() + token.size(), output);
    if (error != std::errc{} || end != token.data() + token.size() ||
        !std::isfinite(output)) {
      Fail("invalid or non-finite number");
    }
    return output;
  }

  std::string_view input_;
  std::size_t position_{0};
};

std::string Escape(std::string_view input) {
  std::ostringstream output;
  output << '"';
  for (const unsigned char value : input) {
    switch (value) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (value < 0x20U) {
        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<int>(value) << std::dec;
      } else {
        output << static_cast<char>(value);
      }
    }
  }
  output << '"';
  return output.str();
}

std::string DumpValue(const Value &value) {
  if (value.IsNull()) {
    return "null";
  }
  if (value.IsBool()) {
    return value.AsBool() ? "true" : "false";
  }
  if (value.IsNumber()) {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << value.AsNumber();
    return output.str();
  }
  if (value.IsString()) {
    return Escape(value.AsString());
  }
  if (value.IsArray()) {
    std::string output = "[";
    bool first = true;
    for (const auto &item : value.AsArray()) {
      if (!first) {
        output.push_back(',');
      }
      first = false;
      output += DumpValue(item);
    }
    output.push_back(']');
    return output;
  }
  std::string output = "{";
  bool first = true;
  for (const auto &[key, item] : value.AsObject()) {
    if (!first) {
      output.push_back(',');
    }
    first = false;
    output += Escape(key);
    output.push_back(':');
    output += DumpValue(item);
  }
  output.push_back('}');
  return output;
}

} // namespace

Value::Value() : value_(nullptr) {}
Value::Value(std::nullptr_t) : value_(nullptr) {}
Value::Value(bool value) : value_(value) {}
Value::Value(double value) : value_(value) {
  if (!std::isfinite(value)) {
    throw Error("JSON number must be finite");
  }
}
Value::Value(std::int64_t value) : value_(static_cast<double>(value)) {}
Value::Value(std::uint64_t value) : value_(static_cast<double>(value)) {
  if (value > 9'007'199'254'740'991ULL) {
    throw Error("integer exceeds JSON's exact numeric range");
  }
}
Value::Value(std::string value) : value_(std::move(value)) {}
Value::Value(const char *value) : value_(std::string(value)) {}
Value::Value(Array value) : value_(std::move(value)) {}
Value::Value(Object value) : value_(std::move(value)) {}

bool Value::IsNull() const { return std::holds_alternative<std::nullptr_t>(value_); }
bool Value::IsBool() const { return std::holds_alternative<bool>(value_); }
bool Value::IsNumber() const { return std::holds_alternative<double>(value_); }
bool Value::IsString() const { return std::holds_alternative<std::string>(value_); }
bool Value::IsArray() const { return std::holds_alternative<Array>(value_); }
bool Value::IsObject() const { return std::holds_alternative<Object>(value_); }

bool Value::AsBool() const {
  if (!IsBool()) throw Error("JSON value is not a boolean");
  return std::get<bool>(value_);
}
double Value::AsNumber() const {
  if (!IsNumber()) throw Error("JSON value is not a number");
  return std::get<double>(value_);
}
std::int64_t Value::AsInt64() const {
  const double value = AsNumber();
  if (std::trunc(value) != value ||
      value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
      value > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    throw Error("JSON number is not an int64");
  }
  return static_cast<std::int64_t>(value);
}
std::uint64_t Value::AsUint64() const {
  const double value = AsNumber();
  if (std::trunc(value) != value || value < 0.0 ||
      value > 9'007'199'254'740'991.0) {
    throw Error("JSON number is not a safe uint64");
  }
  return static_cast<std::uint64_t>(value);
}
const std::string &Value::AsString() const {
  if (!IsString()) throw Error("JSON value is not a string");
  return std::get<std::string>(value_);
}
const Value::Array &Value::AsArray() const {
  if (!IsArray()) throw Error("JSON value is not an array");
  return std::get<Array>(value_);
}
const Value::Object &Value::AsObject() const {
  if (!IsObject()) throw Error("JSON value is not an object");
  return std::get<Object>(value_);
}
Value::Array &Value::AsArray() {
  if (!IsArray()) throw Error("JSON value is not an array");
  return std::get<Array>(value_);
}
Value::Object &Value::AsObject() {
  if (!IsObject()) throw Error("JSON value is not an object");
  return std::get<Object>(value_);
}
bool Value::Contains(std::string_view key) const {
  if (!IsObject()) return false;
  return AsObject().contains(std::string(key));
}
const Value &Value::At(std::string_view key) const {
  const auto &object = AsObject();
  const auto item = object.find(std::string(key));
  if (item == object.end()) {
    throw Error("missing JSON field: " + std::string(key));
  }
  return item->second;
}
Value &Value::operator[](std::string key) {
  if (!IsObject()) {
    value_ = Object{};
  }
  return AsObject()[std::move(key)];
}
std::string Value::Dump() const { return DumpValue(*this); }
Value Value::Parse(std::string_view input) { return Parser(input).ParseDocument(); }

} // namespace multimodal::rag::json
