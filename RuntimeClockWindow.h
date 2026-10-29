// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "CycleBounds.h"

// This transaction requires an independently established, per-CPU PMU lease
// for its entire lifetime. The template does not create or validate a lease.
// The caller must hold the watchdog gate before ANY possible PMU access, and
// release it only after affirmative evidence of full restoration. A trap or
// non-return cannot be handled by a returned status. The caller must prevent
// migration/preemption around both register operation sequences.
// It must run begin and end on the same CPU; callers retain the watchdog gate
// on status 11 and stop all further probes. No busy loop occurs here.
struct ArmCpuRuntimeWindow {
  uint64_t generation, originalPmcr, originalCycles, filter, overflow, frequency;
  uint64_t idleEpoch, before, after, cycles;
  unsigned cpu, status;
  bool started;
};
struct ArmCpuRuntimeWindowResult {
  ArmCpuCycleBounds bounds;
  uint64_t cycles, frequency, beginBefore, beginAfter, endBefore, endAfter;
  unsigned status;
};
template<class R, class Lease>
static ArmCpuRuntimeWindow armcpu_runtime_window_begin(R &r, Lease &lease, unsigned cpu, uint64_t generation)
{
  ArmCpuRuntimeWindow w = {}; w.cpu = cpu; w.generation = generation;
  if (!generation || !lease.watchdogHeld() || !lease.valid(cpu, generation) || r.cpu() != cpu) { w.status=12; return w; }
  const uint64_t idle = r.idleEpoch();
  if (idle == UINT64_MAX || (idle & 1)) { w.status=13; return w; }
  const uint64_t version = (r.id() >> 8) & 15;
  if (version == 0 || version == 15) { w.status=1; return w; }
  w.frequency=r.frequency(); if(w.frequency<1000 || w.frequency>1000000000) {w.status=2;return w;}
  w.originalPmcr=r.pmcr(); const uint64_t enabled=r.enabled(); w.filter=r.filter();
  const uint64_t interrupts=r.interrupts(); w.overflow=r.overflow();
  if ((w.originalPmcr & 1) || enabled || (w.filter & 0xfc000000ULL) || interrupts || w.overflow) {w.status=3;return w;}
  w.originalCycles=r.cycle(); w.originalPmcr &= ~6ULL;
  w.started=true; // recovery gate is already held before the first write
  r.set_cycle(0); r.set_pmcr((w.originalPmcr & ~(8ULL|64ULL))|1); r.enable_cycle(); r.barrier();
  w.idleEpoch=idle;
  w.before=r.time(); w.cycles=r.cycle(); w.after=r.time();
  // An epoch change while setting up rejects this window at end, but cleanup
  // must still happen with the lease held on this CPU.
  return w;
}
template<class R, class Lease>
static ArmCpuRuntimeWindowResult armcpu_runtime_window_end(R &r, Lease &lease, ArmCpuRuntimeWindow &w)
{
  ArmCpuRuntimeWindowResult out = {}; out.status=w.status; out.frequency=w.frequency;
  if (w.status == 11) return out; // conflict quarantine is irreversible for this transaction
  if (!w.started) { if (!out.status) out.status=14; return out; }
  // Cannot safely restore another CPU's PMU or overwrite an unknown owner.
  if (!lease.watchdogHeld() || !lease.valid(w.cpu,w.generation) || r.cpu()!=w.cpu) {out.status=11;w.status=11;return out;}
  out.endBefore=r.time(); uint64_t endCycles=r.cycle(); out.endAfter=r.time();
  uint64_t idleAfter=r.idleEpoch();
  if (((r.pmcr() ^ ((w.originalPmcr & ~(8ULL|64ULL))|1)) & 0x7fULL) ||
      r.enabled()!=(1ULL<<31) || r.filter()!=w.filter || r.interrupts()!=0 ||
      ((r.overflow() ^ w.overflow) & ~(1ULL<<31))) {out.status=11;w.status=11;return out;}
  // No restore after lease loss or locally visible ownership interference.
  if (!lease.watchdogHeld() || !lease.valid(w.cpu,w.generation) || r.cpu()!=w.cpu) {out.status=11;w.status=11;return out;}
  const bool timerChanged=r.frequency()!=w.frequency;
  r.disable_cycle();r.barrier();uint64_t overflow=r.overflow();
  if ((overflow ^ w.overflow) & ~(1ULL<<31)) {out.status=11;w.status=11;return out;}
  if (overflow & (1ULL<<31)) r.clear_overflow(1ULL<<31);
  r.set_pmcr(w.originalPmcr);r.set_cycle(w.originalCycles);r.barrier();w.started=false;
  out.beginBefore=w.before;out.beginAfter=w.after;
  if (timerChanged) {out.status=w.status=9;return out;}
  if (idleAfter!=w.idleEpoch || (idleAfter&1) || idleAfter==UINT64_MAX) {out.status=w.status=13;return out;}
  if ((overflow & (1ULL<<31)) || endCycles>UINT32_MAX || w.cycles>UINT32_MAX || endCycles<=w.cycles) {out.status=w.status=7;return out;}
  out.cycles=endCycles-w.cycles;
  out.bounds=armcpu_cycle_bounds(out.cycles,w.frequency,w.before,w.after,out.endBefore,out.endAfter);
  out.status=out.bounds.valid ? 0 : 9;
  w.status=out.status;
  if (out.status) out.cycles=0;
  return out;
}
