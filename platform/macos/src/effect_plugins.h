#pragma once

// The loaded effect plugins (mf::macos::loadEffectPlugins), by type, for the compositor.

#include <string>

#include "mf/effect_plugin.h"

namespace mf::macos {

const MfEffectPlugin* findEffectPlugin(const std::string& type);  // null: no plugin loaded has it

}  // namespace mf::macos
