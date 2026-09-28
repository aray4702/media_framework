#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mf::json {

// A parsed JSON value. Objects keep their keys in document order.
struct Value {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Value> array;
  std::vector<std::pair<std::string, Value>> object;

  bool isNull() const { return type == Type::Null; }
  bool isBool() const { return type == Type::Bool; }
  bool isNumber() const { return type == Type::Number; }
  bool isString() const { return type == Type::String; }
  bool isArray() const { return type == Type::Array; }
  bool isObject() const { return type == Type::Object; }
  const Value* find(const std::string& key) const;  // null if absent (or not an object)
};

// Strict RFC 8259 JSON, nesting at most 64 deep. On failure returns false and says where.
bool parse(const std::string& text, Value* out, std::string* error);

}  // namespace mf::json
