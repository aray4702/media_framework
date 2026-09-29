#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mf::json {

const Value* Value::find(const std::string& key) const {
  for (const auto& [k, v] : object) {
    if (k == key) return &v;
  }
  return nullptr;
}

namespace {

constexpr int kMaxDepth = 64;

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}

  bool parseDocument(Value* out, std::string* error) {
    skipSpace();
    if (!parseValue(out, 0)) return fail(error);
    skipSpace();
    if (i_ != s_.size()) {
      message_ = "unexpected text after the document";
      return fail(error);
    }
    return true;
  }

 private:
  bool fail(std::string* error) {
    if (error) {
      int line = 1, column = 1;
      for (size_t k = 0; k < i_ && k < s_.size(); ++k) {
        if (s_[k] == '\n') {
          ++line;
          column = 1;
        } else {
          ++column;
        }
      }
      *error = "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message_;
    }
    return false;
  }

  bool error(const char* message) {
    message_ = message;
    return false;
  }

  void skipSpace() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
  }

  bool literal(const char* word) {
    size_t n = std::char_traits<char>::length(word);
    if (s_.compare(i_, n, word) != 0) return error("invalid literal");
    i_ += n;
    return true;
  }

  bool parseValue(Value* v, int depth) {
    if (depth > kMaxDepth) return error("nested too deeply");
    if (i_ >= s_.size()) return error("unexpected end of the document");
    switch (s_[i_]) {
      case '{': return parseObject(v, depth);
      case '[': return parseArray(v, depth);
      case '"':
        v->type = Value::Type::String;
        return parseString(&v->string);
      case 't':
        v->type = Value::Type::Bool;
        v->boolean = true;
        return literal("true");
      case 'f':
        v->type = Value::Type::Bool;
        v->boolean = false;
        return literal("false");
      case 'n':
        v->type = Value::Type::Null;
        return literal("null");
      default: return parseNumber(v);
    }
  }

  bool parseObject(Value* v, int depth) {
    v->type = Value::Type::Object;
    ++i_;
    skipSpace();
    if (i_ < s_.size() && s_[i_] == '}') {
      ++i_;
      return true;
    }
    for (;;) {
      skipSpace();
      if (i_ >= s_.size() || s_[i_] != '"') return error("expected a key in quotes");
      std::string key;
      if (!parseString(&key)) return false;
      if (v->find(key)) return error("duplicate key");
      skipSpace();
      if (i_ >= s_.size() || s_[i_] != ':') return error("expected ':'");
      ++i_;
      skipSpace();
      v->object.emplace_back(std::move(key), Value{});
      if (!parseValue(&v->object.back().second, depth + 1)) return false;
      skipSpace();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        continue;
      }
      if (i_ < s_.size() && s_[i_] == '}') {
        ++i_;
        return true;
      }
      return error("expected ',' or '}'");
    }
  }

  bool parseArray(Value* v, int depth) {
    v->type = Value::Type::Array;
    ++i_;
    skipSpace();
    if (i_ < s_.size() && s_[i_] == ']') {
      ++i_;
      return true;
    }
    for (;;) {
      skipSpace();
      v->array.emplace_back();
      if (!parseValue(&v->array.back(), depth + 1)) return false;
      skipSpace();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        continue;
      }
      if (i_ < s_.size() && s_[i_] == ']') {
        ++i_;
        return true;
      }
      return error("expected ',' or ']'");
    }
  }

  static void appendUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
      out->push_back(char(cp));
    } else if (cp < 0x800) {
      out->push_back(char(0xC0 | (cp >> 6)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(char(0xE0 | (cp >> 12)));
      out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(char(0xF0 | (cp >> 18)));
      out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(char(0x80 | (cp & 0x3F)));
    }
  }

  bool hex4(uint32_t* out) {
    if (i_ + 4 > s_.size()) return error("truncated \\u escape");
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s_[i_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return error("invalid \\u escape");
    }
    *out = v;
    return true;
  }

  bool parseString(std::string* out) {
    ++i_;  // opening quote
    while (i_ < s_.size()) {
      unsigned char c = static_cast<unsigned char>(s_[i_]);
      if (c == '"') {
        ++i_;
        return true;
      }
      if (c < 0x20) return error("control character in a string");
      if (c != '\\') {
        out->push_back(char(c));
        ++i_;
        continue;
      }
      if (++i_ >= s_.size()) break;
      char e = s_[i_++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          uint32_t cp;
          if (!hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate: a low one must follow
            uint32_t low;
            if (s_.compare(i_, 2, "\\u") != 0) return error("unpaired surrogate");
            i_ += 2;
            if (!hex4(&low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return error("unpaired surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return error("unpaired surrogate");
          }
          appendUtf8(out, cp);
          break;
        }
        default: return error("invalid escape");
      }
    }
    return error("unterminated string");
  }

  bool parseNumber(Value* v) {
    size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    if (i_ >= s_.size() || !isdigit(static_cast<unsigned char>(s_[i_]))) return error("invalid value");
    if (s_[i_] == '0') {
      ++i_;
    } else {
      while (i_ < s_.size() && isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      if (i_ >= s_.size() || !isdigit(static_cast<unsigned char>(s_[i_]))) return error("invalid number");
      while (i_ < s_.size() && isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      if (i_ >= s_.size() || !isdigit(static_cast<unsigned char>(s_[i_]))) return error("invalid number");
      while (i_ < s_.size() && isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    v->type = Value::Type::Number;
    v->number = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
    if (!std::isfinite(v->number)) return error("number out of range");
    return true;
  }

  const std::string& s_;
  size_t i_ = 0;
  std::string message_;
};

}  // namespace

bool parse(const std::string& text, Value* out, std::string* error) {
  *out = Value{};
  return Parser(text).parseDocument(out, error);
}

// --- Writing ----------------------------------------------------------------------------------

Value number(double d) {
  Value v;
  v.type = Value::Type::Number;
  v.number = d;
  return v;
}

Value string(const std::string& s) {
  Value v;
  v.type = Value::Type::String;
  v.string = s;
  return v;
}

Value boolean(bool b) {
  Value v;
  v.type = Value::Type::Bool;
  v.boolean = b;
  return v;
}

Value array(std::vector<Value> items) {
  Value v;
  v.type = Value::Type::Array;
  v.array = std::move(items);
  return v;
}

Value object() {
  Value v;
  v.type = Value::Type::Object;
  return v;
}

void add(Value* object, const std::string& key, Value value) { object->object.emplace_back(key, std::move(value)); }

namespace {

void writeString(const std::string& s, std::string* out) {
  *out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          *out += char(c);  // UTF-8 passes through
        }
    }
  }
  *out += '"';
}

void writeValue(const Value& v, int depth, std::string* out) {
  std::string indent(size_t(depth) * 2, ' '), inner(size_t(depth + 1) * 2, ' ');
  switch (v.type) {
    case Value::Type::Null: *out += "null"; break;
    case Value::Type::Bool: *out += v.boolean ? "true" : "false"; break;
    case Value::Type::Number: {
      char buf[32];
      if (v.number == std::floor(v.number) && std::fabs(v.number) < 1e15) std::snprintf(buf, sizeof(buf), "%.0f", v.number);
      else std::snprintf(buf, sizeof(buf), "%.15g", v.number);
      *out += buf;
      break;
    }
    case Value::Type::String: writeString(v.string, out); break;
    case Value::Type::Array: {
      bool plain = true;
      for (const Value& e : v.array) plain &= !e.isArray() && !e.isObject();
      *out += '[';
      for (size_t k = 0; k < v.array.size(); ++k) {
        if (plain) {
          *out += k ? ", " : "";
        } else {
          *out += k ? ",\n" : "\n";
          *out += inner;
        }
        writeValue(v.array[k], depth + 1, out);
      }
      if (!plain && !v.array.empty()) *out += "\n" + indent;
      *out += ']';
      break;
    }
    case Value::Type::Object: {
      *out += '{';
      for (size_t k = 0; k < v.object.size(); ++k) {
        *out += k ? ",\n" : "\n";
        *out += inner;
        writeString(v.object[k].first, out);
        *out += ": ";
        writeValue(v.object[k].second, depth + 1, out);
      }
      if (!v.object.empty()) *out += "\n" + indent;
      *out += '}';
      break;
    }
  }
}

}  // namespace

std::string write(const Value& v) {
  std::string out;
  writeValue(v, 0, &out);
  return out + "\n";
}

}  // namespace mf::json
