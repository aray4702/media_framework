// Effect plugins (mf/effect_plugin.h): each .dylib is opened with dlopen, its description checked
// and its type registered with the core (mf/effects.h). Plugins are never unloaded: compositors
// keep instances of them, and scenes refer to their types.

#import "effect_plugins.h"

#import <Foundation/Foundation.h>

#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>

#include <map>
#include <mutex>
#include <set>

#include "mf/effects.h"
#include "mf/macos.h"

namespace mf::macos {
namespace {

struct Plugins {
  std::mutex mu;
  std::map<std::string, const MfEffectPlugin*> byType;
  std::set<void*> handles;
};

Plugins& plugins() {
  static Plugins p;
  return p;
}

// Checks the description a plugin returns; empty when it's usable.
std::string problem(const MfEffectPlugin* p) {
  if (!p) return "mf_effect_plugin() returned NULL";
  if (p->abi != MF_EFFECT_PLUGIN_ABI) return "built for plugin ABI " + std::to_string(p->abi) + ", not " + std::to_string(MF_EFFECT_PLUGIN_ABI);
  if (!p->type || !p->create || !p->destroy || !p->encode) return "needs a type, create, destroy and encode";
  if (p->paramCount && !p->params) return "has parameters but no list of them";
  for (uint32_t k = 0; k < p->paramCount; ++k) {
    if (!p->params[k].name) return "parameter " + std::to_string(k) + " has no name";
  }
  return {};
}

// Loads one file: its type on success, else "" with the reason in `error`.
std::string load(const std::string& path, std::string* error) {
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* reason = dlerror();
    *error = reason ? reason : "dlopen failed";
    return {};
  }
  Plugins& all = plugins();
  std::lock_guard<std::mutex> lock(all.mu);
  if (all.handles.count(handle)) {  // the same file again (e.g. from two folders): already loaded
    dlclose(handle);
    return {};
  }
  auto entry = reinterpret_cast<MfEffectPluginEntry>(dlsym(handle, MF_EFFECT_PLUGIN_ENTRY));
  const MfEffectPlugin* p = entry ? entry() : nullptr;
  std::string bad = entry ? problem(p) : "not an effect plugin (no " MF_EFFECT_PLUGIN_ENTRY "())";
  if (bad.empty()) {
    EffectInfo info;
    info.type = p->type;
    info.displayName = p->displayName ? p->displayName : "";
    for (uint32_t k = 0; k < p->paramCount; ++k) {
      const MfEffectParam& param = p->params[k];
      info.params.push_back({param.name, param.defaultValue, param.min, param.max});
    }
    if (registerEffect(std::move(info), &bad) == Result::Ok) bad.clear();
  }
  if (!bad.empty()) {
    *error = bad;
    dlclose(handle);
    return {};
  }
  all.handles.insert(handle);
  all.byType[p->type] = p;
  return p->type;
}

std::string executableDir() {
  char path[PATH_MAX];
  uint32_t size = sizeof(path);
  char real[PATH_MAX];
  if (_NSGetExecutablePath(path, &size) != 0 || !realpath(path, real)) return {};
  std::string s = real;
  return s.substr(0, s.rfind('/'));
}

}  // namespace

const MfEffectPlugin* findEffectPlugin(const std::string& type) {
  Plugins& all = plugins();
  std::lock_guard<std::mutex> lock(all.mu);
  auto it = all.byType.find(type);
  return it == all.byType.end() ? nullptr : it->second;
}

std::vector<std::string> loadEffectPluginsFrom(const std::string& dir, std::vector<std::string>* errors) {
  std::vector<std::string> types;
  @autoreleasepool {
    NSString* folder = [NSString stringWithUTF8String:dir.c_str()];
    NSArray<NSString*>* names = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:folder error:nil]
        sortedArrayUsingSelector:@selector(compare:)];
    for (NSString* name in names) {
      if (![name.pathExtension isEqualToString:@"dylib"]) continue;
      std::string path = [folder stringByAppendingPathComponent:name].UTF8String;
      std::string error;
      std::string type = load(path, &error);
      if (!type.empty()) types.push_back(type);
      else if (!error.empty() && errors) errors->push_back(path + ": " + error);
    }
  }
  return types;
}

std::vector<std::string> loadEffectPlugins(std::vector<std::string>* errors) {
  std::vector<std::string> dirs;
  if (const char* env = getenv("MF_EFFECT_PLUGIN_PATH")) {
    std::string list = env;
    for (size_t start = 0; start <= list.size();) {
      size_t end = list.find(':', start);
      if (end == std::string::npos) end = list.size();
      if (end > start) dirs.push_back(list.substr(start, end - start));
      start = end + 1;
    }
  }
  std::string exe = executableDir();
  if (!exe.empty()) dirs.push_back(exe + "/plugins");
  @autoreleasepool {
    NSBundle* bundle = NSBundle.mainBundle;
    if ([bundle.bundlePath.pathExtension isEqualToString:@"app"] && bundle.builtInPlugInsPath) {
      dirs.push_back(bundle.builtInPlugInsPath.UTF8String);
    }
    NSArray<NSString*>* support = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    if (support.firstObject) dirs.push_back([support.firstObject stringByAppendingPathComponent:@"Media Framework/Plugins"].UTF8String);
  }
#ifdef MF_BUILD_PLUGIN_DIR
  dirs.push_back(MF_BUILD_PLUGIN_DIR);  // the plugins built with the framework, for running from the build tree
#endif
  std::vector<std::string> types;
  for (const std::string& dir : dirs) {
    for (std::string& t : loadEffectPluginsFrom(dir, errors)) types.push_back(std::move(t));
  }
  return types;
}

}  // namespace mf::macos
