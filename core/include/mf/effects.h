#pragma once

// Effect types beyond the built-in ones (scene_graph_spec.md §4.4), added by plugins. The core
// knows only a type's parameters, so it can parse, validate, animate and write documents that
// use it; drawing it is the platform's (on macOS, mf::macos::loadEffectPlugins registers each
// plugin it loads). Register types before parsing or opening scenes that use them: a type
// that isn't registered is a validation error, like any unknown effect.

#include <string>
#include <vector>

#include "mf/types.h"

namespace mf {

struct EffectParamInfo {
  std::string name;  // the parameter's key in a document
  double defaultValue = 0, min = 0, max = 1;
};

struct EffectInfo {
  std::string type;         // the effect's "type" in a document
  std::string displayName;  // for editors; empty: the type
  std::vector<EffectParamInfo> params;
  int param(const std::string& name) const;  // its index, or -1
};

constexpr size_t kMaxEffectParams = 16;

// Adds a type. InvalidArgument (with `error`) for a built-in or already registered type, a type
// or parameter name that isn't 1 to 64 letters, digits, _ . - (starting with a letter), a
// parameter named "type" or used twice, more than kMaxEffectParams parameters, or a default
// outside [min, max]. Thread-safe. Types are never removed, so the pointers below stay valid.
Result registerEffect(EffectInfo info, std::string* error = nullptr);
const EffectInfo* findEffect(const std::string& type);  // null: not registered
std::vector<const EffectInfo*> registeredEffects();     // in registration order
bool isBuiltinEffect(const std::string& type);          // colorAdjust, blur, crop, chromaKey

}  // namespace mf
