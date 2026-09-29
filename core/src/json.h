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

// Values to write.
Value number(double);
Value string(const std::string&);
Value boolean(bool);
Value array(std::vector<Value> items = {});
Value object();  // add members with add()
void add(Value* object, const std::string& key, Value value);

// JSON text, indented by two spaces; an array of plain values (numbers, strings, booleans)
// stays on one line. Numbers print with up to 15 significant digits.
std::string write(const Value&);

}  // namespace mf::json
