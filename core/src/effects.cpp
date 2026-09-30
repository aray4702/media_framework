#include "mf/effects.h"

#include <cctype>
#include <cmath>
#include <deque>
#include <mutex>

namespace mf {
namespace {

struct Registry {
  std::mutex mu;
  std::deque<EffectInfo> types;  // a deque: adding never moves the ones already there
};

Registry& registry() {
  static Registry r;
  return r;
}

bool validName(const std::string& s) {
  if (s.empty() || s.size() > 64 || !std::isalpha(static_cast<unsigned char>(s[0]))) return false;
  for (char c : s) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '-') return false;
  }
  return true;
}

Result fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return Result::InvalidArgument;
}

}  // namespace

int EffectInfo::param(const std::string& name) const {
  for (size_t k = 0; k < params.size(); ++k) {
    if (params[k].name == name) return int(k);
  }
  return -1;
}

bool isBuiltinEffect(const std::string& type) {
  return type == "colorAdjust" || type == "blur" || type == "crop" || type == "chromaKey";
}

Result registerEffect(EffectInfo info, std::string* error) {
  const std::string& t = info.type;
  if (!validName(t)) return fail(error, "effect type '" + t + "' must be 1 to 64 letters, digits, _ . - starting with a letter");
  if (isBuiltinEffect(t)) return fail(error, "effect type '" + t + "' is built in");
  if (info.params.size() > kMaxEffectParams) return fail(error, t + ": more than " + std::to_string(kMaxEffectParams) + " parameters");
  for (size_t k = 0; k < info.params.size(); ++k) {
    const EffectParamInfo& p = info.params[k];
    if (!validName(p.name) || p.name == "type") return fail(error, t + ": bad parameter name '" + p.name + "'");
    if (info.param(p.name) != int(k)) return fail(error, t + ": parameter '" + p.name + "' is listed twice");
    if (!std::isfinite(p.min) || !std::isfinite(p.max) || !(p.min <= p.defaultValue && p.defaultValue <= p.max)) {
      return fail(error, t + "." + p.name + ": needs min <= default <= max");
    }
  }
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  for (const EffectInfo& e : r.types) {
    if (e.type == t) return fail(error, "effect type '" + t + "' is already registered");
  }
  r.types.push_back(std::move(info));
  return Result::Ok;
}

const EffectInfo* findEffect(const std::string& type) {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  for (const EffectInfo& e : r.types) {
    if (e.type == type) return &e;
  }
  return nullptr;
}

std::vector<const EffectInfo*> registeredEffects() {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  std::vector<const EffectInfo*> out;
  for (const EffectInfo& e : r.types) out.push_back(&e);
  return out;
}

}  // namespace mf
