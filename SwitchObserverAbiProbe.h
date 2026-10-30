// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

#ifndef ARMCPU_SWITCH_ABI_PROBE
#define ARMCPU_SWITCH_ABI_PROBE 0
#endif

#if ARMCPU_SWITCH_ABI_PROBE
// Compile/link evidence only. No production code calls this local function.
// `used` retains it even when the ordinary arw=4 refusal is optimized.
extern "C" bool unixnu_current_thread_switches(uint32_t *) __attribute__((weak_import));
__attribute__((used, noinline)) static bool
armcpu_switch_observer_abi_probe(uint32_t *out)
{
  if (!out || !unixnu_current_thread_switches) return false;
  return unixnu_current_thread_switches(out);
}
#endif
