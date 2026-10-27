// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../CycleClock.h"
#include <cassert>
#include <cstdint>

struct Source {
  uint64_t v = 0, p = 500;
  unsigned n = 0;
  bool stopped = false, backwards = false, slow = false;
  uint64_t counter(unsigned which) {
    if (which == 1) return p++;
    ++n;
    if (backwards && n == 2) return --v;
    if (!stopped && (!slow || n == 3)) ++v;
    return v;
  }
};
int main() {
  Source stalled; stalled.stopped = true;
  auto v = armcpu_timer_probe(stalled, 0);
  assert(v.status == 0 && v.first == 0 && v.last == 0 && stalled.n == 10001);
  auto p = armcpu_timer_probe(stalled, 1);
  assert(p.status == 1 && p.first == 500 && p.last == 501);
  assert(armcpu_timer_select(v, p) == 2);
  assert(armcpu_timer_select(p, p) == 1);
  assert(armcpu_timer_select(v, v) == 0);
  Source regression; regression.v = 42; regression.backwards = true;
  auto r = armcpu_timer_probe(regression, 0);
  assert(r.status == 2 && r.first == 43 && r.last == 42);
  assert(armcpu_timer_select(r, p) == 2 && armcpu_timer_select(r, v) == 0);
  Source wrap; wrap.v = UINT64_MAX - 1;
  auto w = armcpu_timer_probe(wrap, 0);
  assert(w.status == 1 && w.first == UINT64_MAX && w.last == 0);
  Source delayed; delayed.slow = true;
  auto d = armcpu_timer_probe(delayed, 0);
  assert(d.status == 1 && d.first == 0 && d.last == 1);
}
