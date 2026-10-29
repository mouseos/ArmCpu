// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>

// No system-wide PMU lease or higher-EL access guarantee is available yet.
// A build flag, board identity, or idle register snapshot is not ownership.
static inline bool armcpu_runtime_policy_allows(unsigned cpu)
{
  (void)cpu;
  return false;
}

// The ADT compatible value is a NUL-delimited byte string, not an OSString.
static inline bool armcpu_runtime_is_a04br3(const void *data, size_t size)
{
  static const char name[] = "benesse,a04br3";
  if (!data || size < sizeof(name)) return false;
  const unsigned char *s = static_cast<const unsigned char *>(data);
  for (size_t i = 0; i < sizeof(name); ++i)
    if (s[i] != static_cast<unsigned char>(name[i])) return false;
  return true;
}
