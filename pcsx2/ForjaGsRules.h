// SPDX-License-Identifier: GPL-3.0+
// Forja SX2 (compilation 55): the draw fingerprint rules ("huellas") the GS matches every draw against (GS/Renderers/HW/
// GSRendererHW.cpp, which has the descriptor's layout) and the menu reads from huellas.ini and offers as patches
// (ps4/vm/main.cpp). A draw matches a rule when its descriptor under the rule's mask is one of the rule's values.
#pragma once

#include <atomic>
#include <cstdint>

struct ForjaGsRule
{
	std::uint64_t mask;
	std::uint64_t values[4];
	std::uint32_t count;
};

extern ForjaGsRule g_forja_gs_rules[16]; // written by the menu's side while g_forja_gs_rule_count is 0
extern std::atomic<std::uint32_t> g_forja_gs_rule_count;
extern std::atomic<std::uint32_t> g_forja_gs_rules_off; // a bit per rule: the player left it out
extern std::atomic<std::uint32_t> g_forja_gs_rule_share[16]; // permille of the GS's draw time in the last ~5 s
