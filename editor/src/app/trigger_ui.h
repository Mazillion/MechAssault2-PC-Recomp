// Small shared pieces of the trigger and action editors (app_triggers.cpp, app_actions.cpp).
#pragma once
#include <string>

#include <vector>

#include "../core/names.h"
#include "../core/scenario.h"

namespace ed {

// The name of an object a trigger or action points at ("(none)" / "unnamed object #n" / "missing #n").
std::string unit_label(const ma2::Wdf& w, uint32_t uid);
// The plain-words kind of a trigger or action class ("After a delay", "Show a message").
std::string trig_kind_label(const std::string& cls);
// The name shown for a trigger: its own name, else its kind.
std::string trig_display_name(const ma2::WObj& o, const std::string& cls);

// Per object of the world: 1 = a trigger (MATrigger*), 2 = an action (MAAction*), 0 = anything else.
std::vector<char> trigger_kinds(const ma2::Wdf& w, const ma2::NameTable& names);

}  // namespace ed
