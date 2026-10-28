// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOCPU.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "CycleClock.h"

// Exact private entry from Darwin 23 arm/rtclock.h. Its full data header is
// not exported to IOKit; the IRQ path calls the real kernel implementation.
extern "C" void rtclock_intr (unsigned int);
// Kernel-internal entries used for secondary CPU startup (Darwin 23 osfmk).
extern "C" vm_offset_t ml_static_vtop (vm_offset_t);
extern "C" char LowResetVectorBase[], LowResetVectorEnd[], ResetHandlerData[], const_boot_args[];
extern "C" kern_return_t processor_start (processor_t);
extern "C" kern_return_t kernel_thread_start (void (*) (void *, wait_result_t), void *, thread_t *);
extern "C" void thread_deallocate (thread_t);
extern "C" thread_t current_thread (void);
extern "C" kern_return_t thread_terminate (thread_t);

extern "C" {
volatile uint32_t armcpu_registered = 0;
volatile uint32_t armcpu_gic_ready = 0;
volatile uint64_t armcpu_timer_interrupts = 0;
volatile uint64_t armcpu_ipi_interrupts = 0;
volatile uint64_t armcpu_ipi_sent = 0;       // SGIs the kernel asked ArmCpu::signalCPU to send
volatile uint64_t armcpu_ipi_selftest_sent = 0;
volatile uint64_t armcpu_ipi_dropped = 0;       // GICv2: IPIs to a core whose target bit is not known yet
// Boot core's own GIC registers read back once after enabling (own frame, the same one just written): 0 IGROUPR0, 1 ISENABLER0, 2 GICD_CTLR,
// 3 GICD_TYPER, 4 IGRPMODR0, 5 ICC_PMR_EL1, 6 ICC_IGRPEN1_EL1, 7 ICC_SRE_EL1.
// GICv2: 0 this core's GICD_ITARGETSR0 target bit, 1 GICD_ISENABLER0, 2 GICD_CTLR, 3 GICD_TYPER, 4 GICC_CTLR, 5 GICC_PMR,
// 6 GICD_PIDR2, 7 GICC_IIDR.
volatile uint32_t armcpu_gic_boot[8];
volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];   // per logical core: architectural timer interrupts and SGI 0 taken
static bool gSpiIpi = false;                // boot-arg armcpu-spi-ipi=1: IPIs are software-pended SPIs 1008+core routed to that core (ICC_SGI1R_EL1 generates nothing on this board)
static bool gSelfIpi = false;               // boot-arg armcpu-self-ipi=1: every core sends itself SGI 0 from initCPU
volatile uint32_t armcpu_irq_phase = 0,armcpu_irq_id = 1023;
volatile uint32_t armcpu_quiesce_calls = 0,armcpu_halt_calls = 0;
// Secondary-CPU bring-up evidence (sheng-smp=1). Step values: 1 nub started, 2 registered, 3 start thread running,
// 4 PSCI CPU_ON issued, 5 entered initCPU on the new core, 6 GIC redistributor ready, 7 running.
volatile uint32_t armcpu_smp_requested = 0, armcpu_smp_running = 0, armcpu_smp_conduit = 0;
volatile uint32_t armcpu_smp_step[8], armcpu_smp_mpidr[8], armcpu_smp_seen_mpidr[8], armcpu_smp_fail[8], armcpu_boot_mpidr = 0;
// System registers seen on each secondary core at initCPU entry: CurrentEL, SCTLR_EL1, TCR_EL1, MAIR_EL1 (low words), MIDR_EL1.
volatile uint32_t armcpu_smp_sys[8][5], armcpu_boot_sys[5];
volatile int32_t armcpu_smp_psci_rc[8];
volatile uint32_t armcpu_cycle_reporter_failed[8];
// Independent single-word controls: written on each secondary and read only
// after the cycle mailbox release/acquire. No struct copy or variadic packing.
volatile uint64_t armcpu_timer_raw_virtual[8], armcpu_timer_raw_physical[8];
volatile uint32_t armcpu_timer_raw_flags[8];
}

#define ARMCPU_SMP_SLOTS 8

class ArmCpu : public IOCPU {
  OSDeclareDefaultStructors (ArmCpu)
  OSSharedPtr<IOMemoryMap> distributor;
  OSSharedPtr<IOMemoryMap> redistributors;
  IOCPUInterruptController *controller = nullptr;
  const OSSymbol *cpuName = nullptr;
  uintptr_t dist = 0;
  uintptr_t redist = 0;
  uint64_t affinity = 0;
  uintptr_t redistBase = 0;
  uint64_t redistSize = 0;
  bool skipGic = false;
  bool diagnosePower = false;
  bool diagnoseCycleClock = false;
  ArmCpuCycleMailbox cycleMailbox = {};
  ArmCpuCycleStart cycleStart = {};
  bool secondary = false;
  uint32_t logicalId = 0;
  bool mapGic (void);
  bool initDistributor (void);
  bool wakeRedistributor (bool local);
  bool finishSecondary (void);
  static void drainPending (void);
  bool prepareGic (void);
  void startSecondary (void);
  static void startThread (void *argument, wait_result_t);
  static void cycleReportThread (void *argument, wait_result_t);
  void enableGic (void);
  void interrupt (void);
  static void dispatch (void *target, void *, void *, int);
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void initCPU (bool boot) override;
  void quiesceCPU (void) override;
  void haltCPU (void) override;
  kern_return_t startCPU (vm_offset_t, vm_offset_t) override;
  void signalCPU (IOCPU *target) override;
  uint64_t signalTarget (void) const { return affinity; }
  void enableCPUTimeBase (bool enabled) override;
  const OSSymbol *getCPUName (void) override;
};

OSDefineMetaClassAndStructors (ArmCpu, IOCPU)

// Redistributor ISENABLER0: SGIs 0..15 (only SGI 0 is used by the kernel; the others let the self-SGI probe tell which INTIDs the GIC accepts) and PPI 27.
static const uint32_t kSgiPpiEnable = 0xffffU | (1U << 27);

static const uint32_t kIpiSpiBase = 1008;   // INTIDs 1008..1015 stand for 'IPI to logical core 0..7'

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

static void write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
}

// Distributor side of the software IPI of logical core `core`: group 1, edge-triggered, priority 0x80, routed to affinity `aff`, enabled.
static void spiIpiConfigure (uintptr_t dist, uint32_t core, uint64_t aff)
{
  const uint32_t id = kIpiSpiBase + core;
  write32 (dist, 0x80 + (id / 32) * 4, read32 (dist, 0x80 + (id / 32) * 4) | (1U << (id % 32)));
  write32 (dist, 0xc00 + (id / 16) * 4, read32 (dist, 0xc00 + (id / 16) * 4) | (2U << ((id % 16) * 2)));
  write32 (dist, 0x400 + (id / 4) * 4, 0x80808080);
  *reinterpret_cast<volatile uint64_t *>(dist + 0x6000 + 8ULL * id) = aff & 0xff00ffffffULL;
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (dist, 0x100 + (id / 32) * 4, 1U << (id % 32));
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void spiIpiSend (uintptr_t dist, uint32_t core)
{
  const uint32_t id = kIpiSpiBase + core;
  __asm__ volatile ("dsb ishst" ::: "memory");
  write32 (dist, 0x200 + (id / 32) * 4, 1U << (id % 32));
  __asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t counter (void)
{
  uint64_t value;
  __asm__ volatile ("isb\n\tmrs %0, cntvct_el0" : "=r" (value));
  return value;
}

static bool waitClear (uintptr_t base, unsigned offset, uint32_t mask)
{
  uint64_t frequency;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (frequency));
  uint64_t started = counter ();
  while ((read32 (base, offset) & mask) != 0) {
    if (counter () - started > frequency / 10) {
      return false;
    }
  }
  return true;
}

// GIC state shared by all CPU nubs. The boot CPU maps the distributor and redistributor frames; a secondary core
// only needs the virtual addresses and its own redistributor frame.
static OSSharedPtr<IOMemoryMap> gDistMap, gRedistMap;
static uintptr_t gDist = 0, gFrames = 0;
static uint64_t gFramesSize = 0;
static IOCPUInterruptController *gController = nullptr;
static volatile uint32_t gGicShared = 0;
static uint64_t gRedistStride = 0; // DT redistributor-stride; 0 = infer from GICR_TYPER.VLPIS
volatile uint32_t armcpu_gic_stride = 0, armcpu_gic_typer_probe[2];
static bool gSmpWanted = false;
// Secondary nubs never wait for the boot CPU (IOSleep needs the timer that only the boot nub enables): they queue here and
// the boot nub, or a late secondary once the boot CPU is registered, finishes them.
static class ArmCpu *volatile gPending[ARMCPU_SMP_SLOTS];
static volatile uint32_t gBootRegistered = 0;
static bool gRemoteGic = false; // sheng-smp-gic=1: the boot CPU prepares a new core's redistributor before CPU_ON
// GICv2 (GIC-400, e.g. MediaTek MT8167): memory-mapped CPU interface instead of the GICv3 system registers. Selected by
// /arm-io/intc compatible "arm,gic-400" or "arm,cortex-a15-gic"; reg holds the distributor and the CPU interface.
// Register use follows the non-secure GICv2 bring-up of github.com/mouseos/xnu_kernel_benesse_a04br3
// (pexpert/arm/pe_a04br3_gic.c), which ran on the MT8167 tablet: Group 0 stays with the secure firmware,
// GICC_CTLR bit 0 enables the non-secure group, SGIs go through GICD_SGIR to the banked GICD_ITARGETSR0 target bit.
// Built only with ARMCPU_GICV2=1 (CONFIG_XNU_ARMCPU_GICV2=y in the board defconfig). Otherwise gGicV2 is a constant
// false and every GICv2 branch compiles away, so GICv3 boards build the unchanged driver.
#ifndef ARMCPU_GICV2
#define ARMCPU_GICV2 0
#endif
#if ARMCPU_GICV2
static bool gGicV2 = false;
#else
static constexpr bool gGicV2 = false;
#endif
static OSSharedPtr<IOMemoryMap> gCpuIfMap;
static uintptr_t gCpuIf = 0;
static const uint32_t kGicV2IpiSgi = 1;     // SGI used for IPIs on GICv2 (as in the MT8167 port)
static volatile uint32_t gV2Targets[ARMCPU_SMP_SLOTS];
#if ARMCPU_GICV2
// Secondary-core entry for GICv2 boards (assembly at the end of this file), and the kernel globals start_cpu reads with
// the MMU and caches still off (Darwin 23 osfmk/arm64/start.s: TCR from sysreg_restore, TTBR from cpu_ttep).
extern "C" char armcpu_gicv2_secondary_entry[];
static const size_t kGicV2EntryBytes = 0x100;   // the trampoline is 256-byte aligned and shorter than this (35 instructions)
extern "C" char sysreg_restore[];
extern "C" uint64_t cpu_ttep;
#endif

// True when the NUL-separated compatible string list contains name as one complete entry.
static bool compatibleHas (OSData *compatible, const char *name)
{
  if (compatible == nullptr) { return false; }
  const char *bytes = static_cast<const char *> (compatible->getBytesNoCopy ());
  const unsigned length = compatible->getLength ();
  unsigned want = 0;
  while (name[want] != 0) { want++; }
  for (unsigned at = 0; at < length;) {
    unsigned entry = 0;
    while (at + entry < length && bytes[at + entry] != 0) { entry++; }
    if (at + entry == length) { return false; }   // last entry without its NUL
    if (entry == want && memcmp (bytes + at, name, want) == 0) { return true; }
    at += entry + 1;
  }
  return false;
}

// Shared peripheral interrupts (GICv3 SPIs, INTID 32..1019). A driver attaches a handler for one INTID; the line is set to
// Group 1 non-secure, priority 0xa0, the given trigger and routed to the boot CPU (GICD_IROUTER, affinity routing is on),
// then enabled. The handler runs in the primary interrupt context, before the EOI, and must clear a level source.
// An SPI without a handler is counted and disabled instead of being acknowledged forever.
#define ARMCPU_SPI_SLOTS 16
struct ArmCpuSpi {uint32_t id; void (*handler) (void *); void *context; volatile uint64_t count;};
static ArmCpuSpi gSpi[ARMCPU_SPI_SLOTS];
static volatile uint32_t gSpiUsed = 0;
#ifndef ARMCPU_SPI_SERIALIZE
#define ARMCPU_SPI_SERIALIZE 0
#endif
#if ARMCPU_SPI_SERIALIZE
// Protect the registration table and GIC shared register RMWs against other
// concurrent attachers; IRQ delivery never acquires this lock.
static volatile uint32_t gSpiRegistrationLock = 0;
// One-shot boot-CPU GICv2 initialization only; not a guarantee after quiesce.
static uint32_t gSpiBootReady = 0;
struct SpiRegistrationGuard {
  boolean_t irq;
  SpiRegistrationGuard () : irq (ml_set_interrupts_enabled (false)) {
    while (__atomic_exchange_n (&gSpiRegistrationLock, 1, __ATOMIC_ACQUIRE)) { }
  }
  ~SpiRegistrationGuard () {
    __atomic_store_n (&gSpiRegistrationLock, 0, __ATOMIC_RELEASE);
    ml_set_interrupts_enabled (irq);
  }
};
#endif
static uint64_t gBootAffinity = 0;
extern "C" {
volatile uint64_t armcpu_spi_unhandled = 0;
volatile uint32_t armcpu_spi_unhandled_id = 0;
#if ARMCPU_SPI_SERIALIZE
// Counts and last INTID whose ICENABLER write did not read back disabled.
volatile uint64_t armcpu_spi_mask_failures = 0;
volatile uint32_t armcpu_spi_mask_failed_id = 0;
#endif
}

IOService *ArmCpu::probe (IOService *provider, SInt32 *score)
{
  OSData *type = OSDynamicCast (OSData, provider->getProperty ("device_type"));
  OSData *reg = OSDynamicCast (OSData, provider->getProperty ("reg"));
  uint32_t id;
  if ((type == nullptr) || (type->getLength () != 4) ||
      (memcmp (type->getBytesNoCopy (), "cpu", 4) != 0) ||
      (reg == nullptr) || (reg->getLength () < sizeof (id))) {
    return nullptr;
  }
  memcpy (&id, reg->getBytesNoCopy (), sizeof (id));
  if (ml_get_cpu_number (id) != ml_get_boot_cpu_number ()) {
    unsigned smp = 0;
    PE_parse_boot_argn ("sheng-smp", &smp, sizeof (smp));
    if (smp == 0 || ml_get_cpu_number (id) < 0 || ml_get_cpu_number (id) >= ARMCPU_SMP_SLOTS) {
      return nullptr; // Secondary startup is opt-in (sheng-smp=1).
    }
  }
  return IOCPU::probe (provider, score);
}

bool ArmCpu::mapGic (void)
{
  IORegistryEntry *soc = IORegistryEntry::fromPath ("/arm-io", gIODTPlane);
  IORegistryEntry *gic = IORegistryEntry::fromPath ("/arm-io/intc", gIODTPlane);
  if ((soc == nullptr) || (gic == nullptr)) {
    if (soc != nullptr) { soc->release (); }
    if (gic != nullptr) { gic->release (); }
    return false;
  }
  OSData *ranges = OSDynamicCast (OSData, soc->getProperty ("ranges"));
  OSData *reg = OSDynamicCast (OSData, gic->getProperty ("reg"));
  OSData *compatible = OSDynamicCast (OSData, gic->getProperty ("compatible"));
  uint64_t bus[3];
  uint64_t regions[4];
  bool v2 = ARMCPU_GICV2 && (compatibleHas (compatible, "arm,gic-400") || compatibleHas (compatible, "arm,cortex-a15-gic"));
  // GICv3: exactly distributor + redistributors. GICv2: GICD, GICC, optionally followed by GICH and GICV as in the
  // MT8167 DTS (interrupt-controller@10310000 lists four regions); only the first two are used.
  bool valid = (ranges != nullptr) && (ranges->getLength () == sizeof (bus)) &&
               (reg != nullptr) &&
               (v2 ? (reg->getLength () >= sizeof (regions) && (reg->getLength () % (2 * sizeof (uint64_t))) == 0)
                   : (reg->getLength () == sizeof (regions))) &&
               (compatible != nullptr) &&
               (v2 || ((compatible->getLength () == 11) &&
                       (memcmp (compatible->getBytesNoCopy (), "arm,gic-v3", 11) == 0)));
  if (valid) {
    memcpy (bus, ranges->getBytesNoCopy (), sizeof (bus));
    memcpy (regions, reg->getBytesNoCopy (), sizeof (regions));
  }
  OSData *strideProperty = OSDynamicCast (OSData, gic->getProperty ("redistributor-stride"));
  if (strideProperty != nullptr && strideProperty->getLength () == sizeof (uint64_t)) {
    memcpy (&gRedistStride, strideProperty->getBytesNoCopy (), sizeof (uint64_t));
    if (gRedistStride != 0x20000 && gRedistStride != 0x30000 && gRedistStride != 0x40000) { gRedistStride = 0; }
  }
  armcpu_gic_stride = static_cast<uint32_t> (gRedistStride);
  soc->release ();
  gic->release ();
  if (!valid) {
    return false;
  }
  if (!v2 && ((regions[1] < 0x10000) || (regions[3] < 0x20000))) {
    return false;
  }
  if (v2 && ((regions[1] < 0x1000) || (regions[3] < 0x1000))) {
    return false;
  }
  for (unsigned index = 0; index < 4; index += 2) {
    if ((regions[index] < bus[0]) || (regions[index] - bus[0] > bus[2]) ||
        (regions[index + 1] > bus[2] - (regions[index] - bus[0])) ||
        (bus[1] + regions[index] - bus[0] < bus[1])) {
      return false;
    }
  }
  OSSharedPtr<IOMemoryDescriptor> distMemory (IOMemoryDescriptor::withPhysicalAddress (
      bus[1] + regions[0] - bus[0], regions[1], kIODirectionInOut), OSNoRetain);
  OSSharedPtr<IOMemoryDescriptor> rdMemory (IOMemoryDescriptor::withPhysicalAddress (
      bus[1] + regions[2] - bus[0], regions[3], kIODirectionInOut), OSNoRetain);
  if (!distMemory || !rdMemory) {
    return false;
  }
  distributor = OSSharedPtr<IOMemoryMap> (distMemory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  redistributors = OSSharedPtr<IOMemoryMap> (rdMemory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  if (!distributor || !redistributors) {
    return false;
  }
  dist = static_cast<uintptr_t> (distributor->getVirtualAddress ());
  if (v2) {
    // GICv2: the second region is the CPU interface (GICC). No redistributors.
    // GICD_PIDR2 ArchRev (bits 7:4) is 2 for GICv2 (GIC-400 TRM: GICD_PIDR2 = 0x2b); anything else is a wrong mapping.
    if (((read32 (dist, 0xfe8) >> 4) & 15) != 2) {
      return false;
    }
#if ARMCPU_GICV2
    gGicV2 = true;
#endif
    gCpuIf = static_cast<uintptr_t> (redistributors->getVirtualAddress ());
    gCpuIfMap = redistributors;
    redistBase = 0;
    redistSize = 0;
    gDistMap = distributor;
    gDist = dist;
    gFrames = 0;
    gFramesSize = 0;
    return true;
  }
  if (((read32 (dist, 0xffe8) >> 4) & 15) != 3) {
    return false;
  }
  redistBase = static_cast<uintptr_t> (redistributors->getVirtualAddress ());
  redistSize = regions[3];
  gDistMap = distributor;
  gRedistMap = redistributors;
  gDist = dist;
  gFrames = redistBase;
  gFramesSize = redistSize;
  return true;
}

bool ArmCpu::initDistributor (void)
{
  if (gGicV2) {
    // Disable the inherited non-secure SPIs until their drivers attach; secure Group 0 enables are RAZ/WI here.
    unsigned interrupts = ((read32 (dist, 4) & 31) + 1) * 32;
    for (unsigned id = 32; id < interrupts && id < 1020; id += 32) {
      write32 (dist, 0x180 + id / 8, 0xffffffff);
    }
    return true;
  }
  uint32_t ctl = read32 (dist, 0);
  write32 (dist, 0, ctl & ~3U);
  if (!waitClear (dist, 0, 1U << 31)) { return false; }
  write32 (dist, 0, (ctl & ~3U) | 16U);
  if (!waitClear (dist, 0, 1U << 31)) { return false; }
  unsigned interrupts = ((read32 (dist, 4) & 31) + 1) * 32;
  for (unsigned id = 32; id < interrupts && id < 1020; id += 32) {
    write32 (dist, 0x180 + id / 8, 0xffffffff);
  }
  return true;
}

// Runs on the core whose redistributor is configured: the frame is selected by that core's own MPIDR affinity.
bool ArmCpu::wakeRedistributor (bool local)
{
  if (gGicV2) {
    if (local) { __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (affinity)); }
    return true;
  }
  uintptr_t frames = redistBase;
  // local: this core finds its own frame from MPIDR. Remote: the boot CPU prepares a not-yet-started core's frame
  // from the DT affinity, so the new core never needs GIC MMIO of its own.
  if (local) { __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (affinity)); }
  uint32_t target = static_cast<uint32_t> ((affinity & 0xffffff) | ((affinity >> 8) & 0xff000000));
  redist = 0;
  for (uint64_t offset = 0; offset + 0x20000 <= redistSize;) {
    armcpu_gic_typer_probe[0] = static_cast<uint32_t> (offset);
    uint64_t typer = *reinterpret_cast<volatile uint64_t *> (frames + offset + 8);
    armcpu_gic_typer_probe[1] = static_cast<uint32_t> (typer);
    if (static_cast<uint32_t> (typer >> 32) == target) {
      redist = frames + offset;
      break;
    }
    if ((typer & 16) != 0) { break; }
    offset += gRedistStride != 0 ? gRedistStride : ((typer & 2) != 0 ? 0x40000 : 0x20000);
  }
  if (redist == 0) { return false; }
  write32 (redist, 0x14, read32 (redist, 0x14) & ~2U);
  if (!waitClear (redist, 0x14, 4)) { return false; }
  uintptr_t sgi = redist + 0x10000;
  write32 (sgi, 0x180, 0xffffffff);
  write32 (sgi, 0x80, 0xffffffff);
  write32 (sgi, 0xd00, 0);
  write32 (sgi, 0x280, 0xffffffff);
  for (unsigned offset = 0x400; offset < 0x420; offset += 4) {
    write32 (sgi, offset, 0x80808080);
  }
  if (!local) { write32 (sgi, 0x100, kSgiPpiEnable); }
  __asm__ volatile ("dsb sy" ::: "memory");
  return waitClear (redist, 0, 8);
}

bool ArmCpu::prepareGic (void)
{
  if (!mapGic () || !initDistributor () || !wakeRedistributor (true)) { return false; }
  gGicShared = 1;
  return true;
}

void ArmCpu::enableGic (void)
{
  if (gGicV2) {
    // Banked per-core state: this core's distributor target bit, SGI/PPI enables and the CPU interface.
    uint32_t target = read32 (dist, 0x800) & 0xff;
    if ((target == 0) || ((target & (target - 1)) != 0)) { panic ("ArmCpu: invalid GICv2 CPU target mask 0x%x", target); }
    __atomic_store_n (&gV2Targets[logicalId & (ARMCPU_SMP_SLOTS - 1)], target, __ATOMIC_RELEASE);
    if (!secondary) { gBootAffinity = target; }
    write32 (dist, 0x180, 0xffffffff);                       // ICENABLER0: SGIs/PPIs off while configured
    write32 (dist, 0x280, 1U << 27);                         // ICPENDR0: drop a stale timer PPI
    const uint32_t ids[2] = {27, kGicV2IpiSgi};
    for (unsigned i = 0; i < 2; i++) {
      const unsigned offset = 0x400 + (ids[i] & ~3U), shift = (ids[i] & 3U) * 8;
      write32 (dist, offset, (read32 (dist, offset) & ~(0xffU << shift)) | (0xa0U << shift));
    }
    write32 (dist, 0x100, (1U << 27) | (1U << kGicV2IpiSgi)); // ISENABLER0
    write32 (gCpuIf, 4, 0xff);                               // GICC_PMR
    write32 (gCpuIf, 8, 0);                                  // GICC_BPR
    write32 (gCpuIf, 0, (read32 (gCpuIf, 0) & ~(1U << 9)) | 1U); // GICC_CTLR: non-secure group on, EOImode 0
    write32 (dist, 0, read32 (dist, 0) | 1U);                // GICD_CTLR: non-secure group on
    __asm__ volatile ("dsb sy\n\tisb" ::: "memory");
    if (!secondary) {
      armcpu_gic_boot[0] = target;
      armcpu_gic_boot[1] = read32 (dist, 0x100);
      armcpu_gic_boot[2] = read32 (dist, 0);
      armcpu_gic_boot[3] = read32 (dist, 4);
      armcpu_gic_boot[4] = read32 (gCpuIf, 0);
      armcpu_gic_boot[5] = read32 (gCpuIf, 4);
      armcpu_gic_boot[6] = read32 (dist, 0xfe8);
      armcpu_gic_boot[7] = read32 (gCpuIf, 0xfc);
    }
    armcpu_gic_ready = 1;
#if ARMCPU_SPI_SERIALIZE
    if (!secondary) __atomic_store_n (&gSpiBootReady, 1, __ATOMIC_RELEASE);
#endif
    return;
  }
  uint64_t value;
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (value));
  value |= 1;
  __asm__ volatile ("msr icc_sre_el1, %0\n\tisb" :: "r" (value) : "memory");
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (value));
  if ((value & 1) == 0) { panic ("ArmCpu: GICv3 system register interface unavailable"); }
  __asm__ volatile ("mrs %0, icc_ctlr_el1" : "=r" (value));
  value &= ~2ULL; // EOImode 0: EOIR also deactivates the interrupt.
  __asm__ volatile ("msr icc_ctlr_el1, %0" :: "r" (value) : "memory");
  value = 0xff;
  __asm__ volatile ("msr icc_pmr_el1, %0" :: "r" (value) : "memory");
  value = 0;
  __asm__ volatile ("msr icc_bpr1_el1, %0" :: "r" (value) : "memory");
  if (!secondary) { gBootAffinity = affinity & 0xff00ffffffULL; }
  if (!secondary || !gRemoteGic) {
    write32 (redist + 0x10000, 0x100, kSgiPpiEnable);
    write32 (dist, 0, read32 (dist, 0) | 0x12U);
  }
  value = 1;
  __asm__ volatile ("msr icc_igrpen1_el1, %0\n\tdsb sy\n\tisb" :: "r" (value) : "memory");
  if (!secondary) {
    uint64_t pmr, ig, sre;
    __asm__ volatile ("mrs %0, icc_pmr_el1" : "=r" (pmr));
    __asm__ volatile ("mrs %0, icc_igrpen1_el1" : "=r" (ig));
    __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (sre));
    armcpu_gic_boot[0] = read32 (redist + 0x10000, 0x80);
    armcpu_gic_boot[1] = read32 (redist + 0x10000, 0x100);
    armcpu_gic_boot[2] = read32 (dist, 0);
    armcpu_gic_boot[3] = read32 (dist, 4);
    armcpu_gic_boot[4] = read32 (redist + 0x10000, 0xd00);
    armcpu_gic_boot[5] = static_cast<uint32_t> (pmr);
    armcpu_gic_boot[6] = static_cast<uint32_t> (ig);
    armcpu_gic_boot[7] = static_cast<uint32_t> (sre);
  }
  armcpu_gic_ready = 1;
}


bool ArmCpu::start (IOService *provider)
{
  unsigned skip = 0;
  PE_parse_boot_argn ("armcpu-skip-gic", &skip, sizeof (skip));
  skipGic = skip != 0;
  unsigned diagnostic = 0;
  PE_parse_boot_argn ("armcpu-diagnostics", &diagnostic, sizeof (diagnostic));
  diagnosePower = diagnostic == 1; // per CPU instance
  unsigned cycleClock = 0;
  PE_parse_boot_argn ("armcpu-cycle-clock", &cycleClock, sizeof (cycleClock));
  diagnoseCycleClock = cycleClock == 1;
  uint32_t physical;
  OSData *reg = OSDynamicCast (OSData, provider->getProperty ("reg"));
  memcpy (&physical, reg->getBytesNoCopy (), sizeof (physical));
  logicalId = static_cast<uint32_t> (ml_get_cpu_number (physical));
  secondary = logicalId != static_cast<uint32_t> (ml_get_boot_cpu_number ());
  if (secondary) {
    if (!IOCPU::start (provider)) { return false; }
    armcpu_smp_step[logicalId] = 1;
    affinity = physical;
    armcpu_smp_mpidr[logicalId] = physical;
    cpuName = OSSymbol::withCStringNoCopy ("ARM architectural secondary CPU");
    if (cpuName == nullptr) { return false; }
    retain ();
    __atomic_store_n (&gPending[logicalId], this, __ATOMIC_RELEASE);
    if (__atomic_load_n (&gBootRegistered, __ATOMIC_ACQUIRE) != 0) { drainPending (); }
    return true;
  } else {
    // Only the boot CPU configures shared policy. Its release-store to
    // gBootRegistered publishes these values before any pending CPU starts.
    unsigned selfIpi = 0;
    const bool selfIpiGiven = PE_parse_boot_argn ("armcpu-self-ipi", &selfIpi, sizeof (selfIpi));
    gSelfIpi = selfIpi == 1;
    unsigned spiIpi = 0;
    PE_parse_boot_argn ("armcpu-spi-ipi", &spiIpi, sizeof (spiIpi));
    gSpiIpi = spiIpi == 1;
    uint64_t bootMpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (bootMpidr));
    armcpu_boot_mpidr = static_cast<uint32_t> (bootMpidr);
    if ((!skipGic && !prepareGic ()) || !IOCPU::start (provider)) { return false; }
    if (gGicV2) {
      // armcpu-spi-ipi uses GICD_IROUTER and INTIDs 1008+, which GICv2 does not have (GICD is a single 4 KiB frame).
      gSpiIpi = false;
      // XNU keeps a core's SIGPdisabled set, and cpu_signal() skips that core, until it takes its first IPI. The
      // MT8167 port sends every core a self SGI from initCPU (fee5652); on GICv2 that is the default and
      // armcpu-self-ipi=0 turns it off.
      if (!selfIpiGiven) { gSelfIpi = true; }
    }
  }
  cpuName = OSSymbol::withCStringNoCopy ("ARM architectural boot CPU");
  if (cpuName == nullptr) { return false; }
  ml_processor_info_t info = {};
  info.cpu_id = this;
  info.phys_id = physical;
  info.log_id = logicalId;
  info.cluster_id = static_cast<uint32_t> (ml_get_cluster_number (physical));
  info.cluster_type = ml_get_boot_cluster_type ();
  perfmon_interrupt_handler_func pmi = nullptr;
  kern_return_t status = ml_processor_register (&info, &machProcessor, &ipi_handler, &pmi);
  if (status != KERN_SUCCESS) { panic ("ArmCpu: CPU %u registration failed: %d", logicalId, status); }
  setCPUNumber (info.log_id);
  armcpu_registered = 1;
  unsigned smp = 0;
  PE_parse_boot_argn ("sheng-smp", &smp, sizeof (smp));
  gSmpWanted = smp != 0;
  unsigned remote = 0;
  PE_parse_boot_argn ("sheng-smp-gic", &remote, sizeof (remote));
  gRemoteGic = remote != 0;
  armcpu_smp_requested = gSmpWanted ? 1 : 0;
  unsigned cpuCount = static_cast<unsigned> (ml_get_max_cpu_number ()) + 1;
  controller = new IOCPUInterruptController;
  if ((controller == nullptr) || (controller->initCPUInterruptController (cpuCount, 1) != kIOReturnSuccess)) {
    panic ("ArmCpu: CPU interrupt controller allocation failed");
  }
  controller->registerCPUInterruptController ();
  controller->enableCPUInterrupt (this);
  if (controller->registerInterrupt (provider, logicalId, this, &ArmCpu::dispatch, nullptr) != kIOReturnSuccess) {
    panic ("ArmCpu: CPU IRQ action registration failed");
  }
  gController = controller;
  initCPU (true);
  registerService ();
  __atomic_store_n (&gBootRegistered, 1U, __ATOMIC_RELEASE);
  drainPending ();
  return true;
}

void ArmCpu::drainPending (void)
{
  for (unsigned id = 1; id < ARMCPU_SMP_SLOTS; id++) {
    ArmCpu *cpu = __atomic_exchange_n (&gPending[id], static_cast<ArmCpu *> (nullptr), __ATOMIC_ACQ_REL);
    if (cpu != nullptr) { cpu->finishSecondary (); }
  }
}

bool ArmCpu::finishSecondary (void)
{
  dist = gDist;
  redistBase = gFrames;
  redistSize = gFramesSize;
  if (gRemoteGic && !wakeRedistributor (false)) { armcpu_smp_fail[logicalId] = 3; return false; }
  ml_processor_info_t info = {};
  info.cpu_id = this;
  info.phys_id = static_cast<uint32_t> (affinity);
  info.log_id = logicalId;
  info.cluster_id = static_cast<uint32_t> (ml_get_cluster_number (static_cast<uint32_t> (affinity)));
  info.cluster_type = ml_get_boot_cluster_type ();
  perfmon_interrupt_handler_func pmi = nullptr;
  kern_return_t status = ml_processor_register (&info, &machProcessor, &ipi_handler, &pmi);
  if (status != KERN_SUCCESS) { armcpu_smp_fail[logicalId] = 4; return false; }
  setCPUNumber (info.log_id);
  armcpu_smp_step[logicalId] = 2;
  setCPUState (kIOCPUStateUninitalized);
  registerService ();
  startSecondary ();
  return true;
}

// PSCI conduit comes from the /psci node of the device tree ("smc" or "hvc"); no default is assumed.
static bool psciConduit (bool *hvc)
{
  IORegistryEntry *psci = IORegistryEntry::fromPath ("/psci", gIODTPlane);
  if (psci == nullptr) { return false; }
  OSData *method = OSDynamicCast (OSData, psci->getProperty ("method"));
  bool ok = false;
  if (method != nullptr && method->getLength () == 4 && memcmp (method->getBytesNoCopy (), "smc", 4) == 0) {
    *hvc = false; ok = true;
  } else if (method != nullptr && method->getLength () == 4 && memcmp (method->getBytesNoCopy (), "hvc", 4) == 0) {
    *hvc = true; ok = true;
  }
  psci->release ();
  return ok;
}

static int64_t psciCall (bool hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{
  register uint64_t x0 __asm__ ("x0") = function;
  register uint64_t x1 __asm__ ("x1") = a;
  register uint64_t x2 __asm__ ("x2") = b;
  register uint64_t x3 __asm__ ("x3") = c;
  if (hvc) {
    __asm__ volatile ("hvc #0" : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3) : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  } else {
    __asm__ volatile ("smc #0" : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3) : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  }
  return static_cast<int64_t> (x0);
}

void ArmCpu::startThread (void *argument, wait_result_t)
{
  ArmCpu *self = static_cast<ArmCpu *> (argument);
  armcpu_smp_step[self->logicalId] = 3;
  bool irqBefore = ml_get_interrupts_enabled ();
  kern_return_t result = processor_start (self->machProcessor);
#if ARMCPU_GICV2
  if (gGicV2 && self->diagnoseCycleClock)
    self->cycleStart.publish (result, irqBefore, ml_get_interrupts_enabled ());
#endif
  (void) result; (void) irqBefore;
  thread_terminate (current_thread ());
}

void ArmCpu::cycleReportThread (void *argument, wait_result_t)
{
#if ARMCPU_GICV2
  ArmCpu *self = static_cast<ArmCpu *> (argument);
  ArmCpuCycleReportState state = {};
  uint64_t deadline;
  clock_interval_to_deadline (15, 1000000000, &deadline);
  // Each log is gated individually; never sleep with IRQs disabled. A failed
  // reporter leaves both mailboxes intact and records the reason for inspection.
#define ARMCPU_REPORT_LOG(...) do { \
    if (!armcpu_report_if_irq (ml_get_interrupts_enabled (), [&] { IOLog (__VA_ARGS__); })) \
      armcpu_cycle_reporter_failed[self->logicalId] = 2; \
  } while (0)
  // Each control line has exactly one variadic integer and a literal CPU tag.
#define ARMCPU_RAW_LOG(label, value) do { \
    uint64_t raw_value = (value); \
    switch (self->logicalId) { \
    case 1: ARMCPU_REPORT_LOG ("ArmCpu: raw-cpu1 " label "=%016llx\n", static_cast<unsigned long long> (raw_value)); break; \
    case 2: ARMCPU_REPORT_LOG ("ArmCpu: raw-cpu2 " label "=%016llx\n", static_cast<unsigned long long> (raw_value)); break; \
    case 3: ARMCPU_REPORT_LOG ("ArmCpu: raw-cpu3 " label "=%016llx\n", static_cast<unsigned long long> (raw_value)); break; \
    case 4: ARMCPU_REPORT_LOG ("ArmCpu: raw-cpu4 " label "=%016llx\n", static_cast<unsigned long long> (raw_value)); break; \
    case 5: ARMCPU_REPORT_LOG ("ArmCpu: raw-cpu5 " label "=%016llx\n", static_cast<unsigned long long> (raw_value)); break; \
    case 6: ARMCPU_REPORT_LOG ("ArmCpu: raw-cpu6 " label "=%016llx\n", static_cast<unsigned long long> (raw_value)); break; \
    case 7: ARMCPU_REPORT_LOG ("ArmCpu: raw-cpu7 " label "=%016llx\n", static_cast<unsigned long long> (raw_value)); break; \
    default: break; \
    } \
  } while (0)
  while (!state.done ()) {
    if (!ml_get_interrupts_enabled ()) { armcpu_cycle_reporter_failed[self->logicalId] = 2; break; }
    int result; unsigned before, after;
    if (!state.startReported && self->cycleStart.read (result, before, after)) {
      ARMCPU_REPORT_LOG ("ArmCpu: cycle-clock-start cpu=%u result=%d irq_before=%u irq_after=%u\n", self->logicalId, result, before, after);
      state.startReported = true;
    }
    ArmCpuCycleSample sample; uint64_t mpidr;
    if (!state.sampleReported && self->cycleMailbox.read (sample, mpidr)) {
      // Independent direct-register controls, emitted before the struct report.
      uint32_t rawFlags = __atomic_load_n (&armcpu_timer_raw_flags[self->logicalId], __ATOMIC_ACQUIRE);
      ARMCPU_RAW_LOG ("format-control", 0x1122334455667788ULL);
      if (rawFlags & 1) ARMCPU_RAW_LOG ("cntvct", __atomic_load_n (&armcpu_timer_raw_virtual[self->logicalId], __ATOMIC_RELAXED));
      if (rawFlags & 2) ARMCPU_RAW_LOG ("cntpct", __atomic_load_n (&armcpu_timer_raw_physical[self->logicalId], __ATOMIC_RELAXED));
      ARMCPU_REPORT_LOG ("ArmCpu: cycle-clock cpu=%u mpidr=%llx status=%u cntfrq=%llu ticks=%llu cycles=%llu\n",
             self->logicalId, mpidr, sample.status, sample.frequency, sample.ticks, sample.cycles);
      ARMCPU_REPORT_LOG ("ArmCpu: cycle-clock-pmu cpu=%u pmcr=%llx filter=%llx interrupts=%llx overflow=%llx after=%llx\n",
             self->logicalId, sample.pmcr, sample.filter, sample.interrupts, sample.overflow, sample.overflowAfter);
      ARMCPU_REPORT_LOG ("ArmCpu: cycle-clock-window cpu=%u outer=%llu begin=%llu end=%llu\n",
             self->logicalId, sample.outerTicks, sample.beginReadTicks, sample.endReadTicks);
      ARMCPU_REPORT_LOG ("ArmCpu: cycle-clock-time cpu=%u before=%llu after=%llu end_before=%llu end_after=%llu\n",
             self->logicalId, sample.beginBefore, sample.beginAfter, sample.endBefore, sample.endAfter);
      ARMCPU_REPORT_LOG ("ArmCpu: cycle-clock-source cpu=%u source=%u ctl_before=%llx ctl_after=%llx cntkctl=%llx\n", self->logicalId, sample.timeSource, sample.timerControlBefore, sample.timerControlAfter, sample.timerAccess);
      ARMCPU_REPORT_LOG ("ArmCpu: timer-virtual-before cpu=%u state=%u first=%llu last=%llu\n", self->logicalId, sample.virtualBefore.status, sample.virtualBefore.first, sample.virtualBefore.last);
      ARMCPU_REPORT_LOG ("ArmCpu: timer-virtual-after cpu=%u state=%u first=%llu last=%llu\n", self->logicalId, sample.virtualAfter.status, sample.virtualAfter.first, sample.virtualAfter.last);
      ARMCPU_REPORT_LOG ("ArmCpu: timer-physical-after cpu=%u state=%u first=%llu last=%llu\n", self->logicalId, sample.physicalAfter.status, sample.physicalAfter.first, sample.physicalAfter.last);
      if (sample.badStage)
        ARMCPU_REPORT_LOG ("ArmCpu: cycle-clock-bad-time cpu=%u stage=%u previous=%llu current=%llu\n",
               self->logicalId, sample.badStage, sample.badPrevious, sample.badCurrent);
      state.sampleReported = true;
    }
    if (!state.done ()) {
      uint64_t now;
      clock_get_uptime (&now);
      if (state.expired (now, deadline)) {
        ARMCPU_REPORT_LOG ("ArmCpu: cycle-clock-incomplete cpu=%u start=%u sample=%u\n", self->logicalId, static_cast<unsigned> (state.startReported), static_cast<unsigned> (state.sampleReported));
        break;
      }
      if (!ml_get_interrupts_enabled ()) { armcpu_cycle_reporter_failed[self->logicalId] = 2; break; }
      IOSleep (100);
      state.waited ();
    }
  }
#undef ARMCPU_RAW_LOG
#undef ARMCPU_REPORT_LOG
  // This standalone thread holds no driver locks; termination can block and
  // therefore must not inherit a disabled interrupt state on the error path.
  if (!ml_get_interrupts_enabled ()) ml_set_interrupts_enabled (true);
#else
  (void) argument;
#endif
  thread_terminate (current_thread ());
}

void ArmCpu::startSecondary (void)
{
  thread_t thread;
#if ARMCPU_GICV2
  if (gGicV2 && diagnoseCycleClock) {
    if (kernel_thread_start (&ArmCpu::cycleReportThread, this, &thread) == KERN_SUCCESS) {
      thread_deallocate (thread);
    } else {
      // Preserve boot recovery: a diagnostic allocation failure must not stop CPU_ON.
      armcpu_cycle_reporter_failed[logicalId] = 1;
      if (ml_get_interrupts_enabled ()) IOLog ("ArmCpu: cycle-clock-reporter cpu=%u error=thread-create\n", logicalId);
    }
  }
#endif
  if (kernel_thread_start (&ArmCpu::startThread, this, &thread) != KERN_SUCCESS) {
    armcpu_smp_psci_rc[logicalId] = -1000;
#if ARMCPU_GICV2
    if (gGicV2 && diagnoseCycleClock) cycleStart.publish (-1000, ml_get_interrupts_enabled (), ml_get_interrupts_enabled ());
#endif
    return;
  }
  thread_deallocate (thread);
}

#if ARMCPU_GICV2
struct ArmCpuCycleRegisters {
  unsigned source = 0;
  uint64_t counter(unsigned which) { uint64_t v; if (which == 0) __asm__ volatile ("isb\n\tmrs %0, cntvct_el0" : "=r" (v) :: "memory"); else __asm__ volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (v) :: "memory"); return v; }
  uint64_t id() { uint64_t v; __asm__ volatile ("mrs %0, id_aa64dfr0_el1" : "=r" (v)); return v; }
  uint64_t frequency() { uint64_t v; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (v)); return v; }
  uint64_t pmcr() { uint64_t v; __asm__ volatile ("mrs %0, pmcr_el0" : "=r" (v)); return v; }
  uint64_t enabled() { uint64_t v; __asm__ volatile ("mrs %0, pmcntenset_el0" : "=r" (v)); return v; }
  uint64_t interrupts() { uint64_t v; __asm__ volatile ("mrs %0, pmintenset_el1" : "=r" (v)); return v; }
  uint64_t overflow() { uint64_t v; __asm__ volatile ("mrs %0, pmovsclr_el0" : "=r" (v)); return v; }
  void clear_overflow(uint64_t v) { __asm__ volatile ("msr pmovsclr_el0, %0" :: "r" (v) : "memory"); }
  uint64_t filter() { uint64_t v; __asm__ volatile ("mrs %0, pmccfiltr_el0" : "=r" (v)); return v; }
  uint64_t cycle() { uint64_t v; __asm__ volatile ("isb\n\tmrs %0, pmccntr_el0" : "=r" (v) :: "memory"); return v; }
  uint64_t time() { return counter(source); }
  void set_cycle(uint64_t v) { __asm__ volatile ("msr pmccntr_el0, %0" :: "r" (v) : "memory"); }
  void set_pmcr(uint64_t v) { __asm__ volatile ("msr pmcr_el0, %0" :: "r" (v) : "memory"); }
  void enable_cycle() { uint64_t v = 1ULL << 31; __asm__ volatile ("msr pmcntenset_el0, %0" :: "r" (v) : "memory"); }
  void disable_cycle() { uint64_t v = 1ULL << 31; __asm__ volatile ("msr pmcntenclr_el0, %0" :: "r" (v) : "memory"); }
  void barrier() { __asm__ volatile ("isb" ::: "memory"); }
};
#endif

void ArmCpu::initCPU (bool boot)
{
  if (secondary) {
    if (!boot) { panic ("ArmCpu: secondary resume is unsupported"); }
    armcpu_smp_step[logicalId] = 5;
    uint64_t mpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
    armcpu_smp_seen_mpidr[logicalId] = static_cast<uint32_t> (mpidr);
    // Verify the actual calling core before any PMU access.
    if (((mpidr & 0xffffffULL) | ((mpidr >> 8) & 0xff000000ULL)) != (affinity & 0xffffffffULL)) { armcpu_smp_fail[logicalId] = 1; for (;;) { __asm__ volatile ("wfe"); } }
#if ARMCPU_GICV2
    ArmCpuCycleRegisters registers;
    ArmCpuTimerProbe virtualBefore = {};
    uint64_t timerControlBefore = 0;
    if (gGicV2 && diagnoseCycleClock) {
      __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (timerControlBefore));
      virtualBefore = armcpu_timer_probe(registers, 0);
    }
#endif
    if (!gRemoteGic && !wakeRedistributor (true)) { armcpu_smp_fail[logicalId] = 2; for (;;) { __asm__ volatile ("wfe"); } }
    armcpu_smp_step[logicalId] = 6;
    enableGic ();
    if (gSpiIpi) { spiIpiConfigure (gDist, logicalId, affinity); }
    // The shared IOCPUInterruptController blocks registrations until every core is enabled, so the secondary cores
    // install their handler directly. ml_install_interrupt_handler() records it in the calling core's cpu_data.
    ml_install_interrupt_handler (this, static_cast<int> (logicalId), this, reinterpret_cast<IOInterruptHandler> (&ArmCpu::dispatch), nullptr);
    enableCPUTimeBase (true);
#if ARMCPU_GICV2
    if (gGicV2 && diagnoseCycleClock) {
      uint64_t timerControlAfter, timerAccess;
      __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r" (timerControlAfter));
      __asm__ volatile ("mrs %0, cntkctl_el1" : "=r" (timerAccess));
      uint64_t rawVirtual = registers.counter (0);
      ArmCpuTimerProbe virtualAfter, physicalAfter;
      armcpu_probe_timer_sources(registers, virtualAfter, physicalAfter);
      uint64_t rawPhysical = 0;
      uint32_t rawFlags = 1;
      if (physicalAfter.status != 3) { rawPhysical = registers.counter (1); rawFlags |= 2; }
      __atomic_store_n (&armcpu_timer_raw_virtual[logicalId], rawVirtual, __ATOMIC_RELAXED);
      __atomic_store_n (&armcpu_timer_raw_physical[logicalId], rawPhysical, __ATOMIC_RELAXED);
      __atomic_store_n (&armcpu_timer_raw_flags[logicalId], rawFlags, __ATOMIC_RELEASE);
      ArmCpuCycleSample sample = {};
      unsigned selected = armcpu_timer_select(virtualAfter, physicalAfter);
      if (selected) {
        registers.source = selected - 1;
        sample = armcpu_cycle_sample(registers);
        sample.timeSource = registers.source + 1;
      } else {
        sample.frequency = registers.frequency();
        sample.status = 10; // neither source passed the bounded liveness check
      }
      sample.virtualBefore = virtualBefore; sample.virtualAfter = virtualAfter;
      sample.physicalAfter = physicalAfter; sample.timerControlBefore = timerControlBefore;
      sample.timerControlAfter = timerControlAfter; sample.timerAccess = timerAccess;
      cycleMailbox.publish (sample, mpidr);
    }
#endif
    setCPUState (kIOCPUStateRunning);
    armcpu_smp_step[logicalId] = 7;
    armcpu_smp_running = armcpu_smp_running + 1;
    // XNU clears SIGPdisabled for a core the first time that core takes an IPI, and cpu_signal() silently skips a core until then; Apple's
    // CPU driver provokes it with a self-IPI from initCPU (osfmk/kern/machine.c, processor_up).
    if (gSelfIpi) { signalCPU (this); }
    return;
  }
  if (!boot) { panic ("ArmCpu: secondary/resume CPU startup is unsupported"); }
  if (skipGic) {
    // Explicit A/B hardware diagnostic: do not touch GIC MMIO or enable an
    // interrupt source. CPU registration still happens; the timer is left off.
    setCPUState (kIOCPUStateRunning);
    return;
  }
  enableGic ();
  if (gSpiIpi) { spiIpiConfigure (gDist, logicalId, affinity); }
  enableCPUTimeBase (true);
  setCPUState (kIOCPUStateRunning);
  if (gSelfIpi) { signalCPU (this); }
}

void ArmCpu::dispatch (void *target, void *, void *, int)
{
  static_cast<ArmCpu *> (target)->interrupt ();
}

void ArmCpu::interrupt (void)
{
  unsigned drained=0;
  for (;;) {
    armcpu_irq_phase=1;
    uint64_t id;
    uint32_t v2Raw = 0;
    if (gGicV2) {
      // GICC_IAR: INTID in bits 0..9, the SGI source CPU in bits 10..12. Classify by INTID only (the IPI SGI is
      // presented as INTID 0, the value the GICv3 path uses) and EOI with the raw value.
      v2Raw = read32 (gCpuIf, 0x0c);
      const uint32_t intid = v2Raw & 0x3ff;
      if (intid >= 1020) { armcpu_irq_phase = 0; break; }
      id = (intid == kGicV2IpiSgi) ? 0 : intid;
    } else {
      __asm__ volatile ("mrs %0, icc_iar1_el1" : "=r" (id));
    }
    armcpu_irq_id=static_cast<uint32_t>(id & 0xffffff);
    if ((id & 0xffffff) >= 1020) { armcpu_irq_phase=0;break; }
    if(diagnosePower && ++drained>128)panic("ArmCpu: IRQ drain exceeds 128 id=%llx timer=%llu IPI=%llu",id,armcpu_timer_interrupts,armcpu_ipi_interrupts);
    if ((id & 0xffffff) == 27) {
      armcpu_timer_interrupts = armcpu_timer_interrupts + 1;
      armcpu_core_timer[logicalId & 7] = armcpu_core_timer[logicalId & 7] + 1;
      armcpu_irq_phase=2;
      rtclock_intr (0);
      armcpu_irq_phase=3;
    } else if ((id & 0xffffff) == 0) {
      armcpu_ipi_interrupts = armcpu_ipi_interrupts + 1;
      armcpu_core_sgi[logicalId & 7] = armcpu_core_sgi[logicalId & 7] + 1;
      armcpu_irq_phase=4;
      if (ipi_handler != nullptr) { ipi_handler (); }
      armcpu_irq_phase=5;
    } else if (gSpiIpi && (id & 0xffffff) >= kIpiSpiBase && (id & 0xffffff) < kIpiSpiBase + 8) {
      armcpu_ipi_interrupts = armcpu_ipi_interrupts + 1;
      armcpu_core_sgi[logicalId & 7] = armcpu_core_sgi[logicalId & 7] + 1;
      armcpu_irq_phase = 4;
      if (ipi_handler != nullptr) { ipi_handler (); }
      armcpu_irq_phase = 5;
    } else {
      const uint32_t intid = static_cast<uint32_t> (id & 0xffffff);
      ArmCpuSpi *spi = nullptr;
      const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
      for (uint32_t slot = 0; intid >= 32 && slot < used && spi == nullptr; slot++) {
        if (gSpi[slot].id == intid) { spi = &gSpi[slot]; }
      }
      if (spi != nullptr) {
        armcpu_irq_phase = 7;
        spi->count = spi->count + 1;
        spi->handler (spi->context);
      } else if (intid >= 32 && gDist != 0) {
        armcpu_spi_unhandled = armcpu_spi_unhandled + 1;
        armcpu_spi_unhandled_id = intid;
        write32 (gDist, 0x180 + (intid / 32) * 4, 1U << (intid % 32));
      } else {
        panic ("ArmCpu: unexpected architectural IRQ %llu", id);
      }
    }
    armcpu_irq_phase=6;
    if (gGicV2) {
      __asm__ volatile ("dsb ish" ::: "memory");
      write32 (gCpuIf, 0x10, v2Raw);                        // GICC_EOIR with the acknowledged value (source CPU kept)
      __asm__ volatile ("isb" ::: "memory");
    } else {
      __asm__ volatile ("msr icc_eoir1_el1, %0\n\tisb" :: "r" (id) : "memory");
    }
  }
}

void ArmCpu::signalCPU (IOCPU *target)
{
  ArmCpu *peer = OSDynamicCast (ArmCpu, target);
  if (peer == nullptr || (peer->signalTarget () & 0xff) >= 16) {
    panic ("ArmCpu: unsupported CPU signal target");
  }
  uint64_t aff = peer->signalTarget ();
  uint64_t sgi = ((aff & 0xff00000000ULL) << 16) | ((aff & 0xff0000) << 16) |
                 ((aff & 0xff00) << 8) | (1ULL << (aff & 0xff));
  armcpu_ipi_sent = armcpu_ipi_sent + 1;
  if (gGicV2) {
    uint32_t target = __atomic_load_n (&gV2Targets[peer->logicalId & (ARMCPU_SMP_SLOTS - 1)], __ATOMIC_ACQUIRE);
    if (target == 0) { armcpu_ipi_dropped = armcpu_ipi_dropped + 1; return; } // core not yet up: count, do not panic
    __asm__ volatile ("dsb ishst" ::: "memory");
    write32 (gDist, 0xf00, (target << 16) | kGicV2IpiSgi);  // GICD_SGIR, target list filter
    __asm__ volatile ("isb" ::: "memory");
    return;
  }
  if (gSpiIpi) { spiIpiSend (gDist, peer->logicalId & 7); return; }
  __asm__ volatile ("dsb ishst\n\tmsr icc_sgi1r_el1, %0\n\tisb" :: "r" (sgi) : "memory");
}

// Diagnostic: sends SGI 0 to the core whose MPIDR affinity is aff (the DT "reg" value, e.g. 0x100 for core 1) exactly as signalCPU
// does. The handler on the target runs the kernel's IPI callback with nothing pending, which is harmless. Returns nothing: the caller
// compares armcpu_ipi_interrupts before and after.
// Read-only snapshot of the GIC state that decides whether SGI 0 reaches the core with MPIDR affinity aff: its redistributor's SGI frame and
// the distributor control register. out: 0 IGROUPR0, 1 ISENABLER0, 2 ISPENDR0, 3 IGRPMODR0, 4 GICR_WAKER, 5 GICD_CTLR, 6 IPRIORITYR0, 7 found(1).
// armcpu_gic_diag_stage names the access in progress, so a read that never returns is identified from another thread:
// 0x100+frame walk, 2 IGROUPR0, 3 ISENABLER0, 4 ISPENDR0, 5 IGRPMODR0, 6 WAKER, 7 GICD_CTLR, 8 IPRIORITYR0, 9 done, 0xe not found.
volatile uint32_t armcpu_gic_diag_stage = 0;
extern "C" void armcpu_gic_diag (uint64_t aff, uint32_t *out)
{
  for (unsigned i = 0; i < 8; i++) { out[i] = 0xdeadbeef; }
  if (gDist == 0 || gFrames == 0) { armcpu_gic_diag_stage = 0xe; return; }
  const uint32_t target = static_cast<uint32_t> ((aff & 0xffffff) | ((aff >> 8) & 0xff000000));
  for (uint64_t offset = 0; offset + 0x20000 <= gFramesSize;) {
    armcpu_gic_diag_stage = 0x100 + static_cast<uint32_t> (offset >> 16);
    const uint64_t typer = *reinterpret_cast<volatile uint64_t *> (gFrames + offset + 8);
    if (static_cast<uint32_t> (typer >> 32) == target) {
      const uintptr_t redist = gFrames + offset, sgi = redist + 0x10000;
      armcpu_gic_diag_stage = 2; out[0] = read32 (sgi, 0x80);
      armcpu_gic_diag_stage = 3; out[1] = read32 (sgi, 0x100);
      armcpu_gic_diag_stage = 4; out[2] = read32 (sgi, 0x200);
      armcpu_gic_diag_stage = 5; out[3] = read32 (sgi, 0xd00);
      armcpu_gic_diag_stage = 6; out[4] = read32 (redist, 0x14);
      armcpu_gic_diag_stage = 7; out[5] = read32 (gDist, 0);
      armcpu_gic_diag_stage = 8; out[6] = read32 (sgi, 0x400);
      out[7] = 1;
      armcpu_gic_diag_stage = 9;
      return;
    }
    if ((typer & 16) != 0) { break; }
    offset += gRedistStride != 0 ? gRedistStride : ((typer & 2) != 0 ? 0x40000 : 0x20000);
  }
  armcpu_gic_diag_stage = 0xe;
}

// Does an SGI sent to the calling core itself become pending at its own CPU interface, and which INTIDs? Only system registers are used (no GIC
// MMIO). With IRQs masked, SGIs 0..15 are sent one at a time to this core's own MPIDR affinity through ICC_SGI1R_EL1 (Group 1) and, when that
// shows nothing, through ICC_SGI0R_EL1 (Group 0); ICC_HPPIR1_EL1 / ICC_HPPIR0_EL1 is polled and a pending SGI is acknowledged and ended here so the
// kernel never sees it. out[8], out[9]: CNTVCT ticks of one ICC_SGI1R_EL1 write and of one ICC_PMR_EL1 read (a trapped write costs far more). out: 0 MPIDR, 1 INTIDs pending as Group 1, 2 INTIDs pending as Group 0, 3 ICC_PMR, 4 ICC_IGRPEN1, 5 ICC_SRE, 6 ICC_CTLR, 7 ICC_RPR.
extern "C" void armcpu_sgi_selfprobe (uint64_t *out)
{
  if (gGicV2) { for (unsigned i = 0; i < 10; i++) { out[i] = 0; } return; } // GICv3 system-register probe only
  uint64_t mpidr, daif, v;
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
  __asm__ volatile ("mrs %0, daif\n\tmsr daifset, #2\n\tisb" : "=r" (daif) :: "memory");
  for (unsigned i = 0; i < 10; i++) { out[i] = 0; }
  out[0] = mpidr;
  __asm__ volatile ("mrs %0, icc_pmr_el1" : "=r" (v)); out[3] = v;
  __asm__ volatile ("mrs %0, icc_igrpen1_el1" : "=r" (v)); out[4] = v;
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (v)); out[5] = v;
  __asm__ volatile ("mrs %0, icc_ctlr_el1" : "=r" (v)); out[6] = v;
  __asm__ volatile ("mrs %0, icc_rpr_el1" : "=r" (v)); out[7] = v;
  if ((mpidr & 0xff) < 16) {
    const uint64_t base = ((mpidr & 0xff00000000ULL) << 16) | ((mpidr & 0xff0000) << 16) | ((mpidr & 0xff00) << 8) | (1ULL << (mpidr & 0xff));
    for (uint64_t intid = 0; intid < 16; intid++) {
      const uint64_t sgi = base | (intid << 24);
      uint64_t h = 1023, ta, tb;
      __asm__ volatile ("dsb ishst\n\tisb\n\tmrs %0, cntvct_el0\n\tisb" : "=r" (ta) :: "memory");
      __asm__ volatile ("msr icc_sgi1r_el1, %0\n\tisb\n\tmrs %1, cntvct_el0" : "=r" (tb) : "r" (sgi) : "memory");
      if (intid == 0) { out[8] = tb - ta; }
      for (unsigned polls = 0; polls < 400; polls++) {
        __asm__ volatile ("mrs %0, icc_hppir1_el1" : "=r" (h));
        if ((h & 0xffffff) == intid) { break; }
        __asm__ volatile ("isb" ::: "memory");
      }
      if ((h & 0xffffff) == intid) {
        out[1] |= 1ULL << intid;
        __asm__ volatile ("mrs %0, icc_iar1_el1" : "=r" (v));
        __asm__ volatile ("msr icc_eoir1_el1, %0\n\tisb" :: "r" (v) : "memory");
        continue;
      }
      __asm__ volatile ("dsb ishst\n\tmsr icc_sgi0r_el1, %0\n\tisb" :: "r" (sgi) : "memory");
      for (unsigned polls = 0; polls < 400; polls++) {
        __asm__ volatile ("mrs %0, icc_hppir0_el1" : "=r" (h));
        if ((h & 0xffffff) == intid) { break; }
        __asm__ volatile ("isb" ::: "memory");
      }
      if ((h & 0xffffff) == intid) {
        out[2] |= 1ULL << intid;
        __asm__ volatile ("mrs %0, icc_iar0_el1" : "=r" (v));
        __asm__ volatile ("msr icc_eoir0_el1, %0\n\tisb" :: "r" (v) : "memory");
      }
    }
  }
  {
    uint64_t ta, tb;
    __asm__ volatile ("isb\n\tmrs %0, cntvct_el0\n\tisb\n\tmrs %2, icc_pmr_el1\n\tisb\n\tmrs %1, cntvct_el0" : "=r" (ta), "=r" (tb), "=r" (v) :: "memory");
    out[9] = tb - ta;
  }
  __asm__ volatile ("msr daif, %0\n\tisb" :: "r" (daif) : "memory");
}

extern "C" void armcpu_ipi_selftest (uint64_t aff)
{
  if (gGicV2) { return; } // GICv3 system-register probe only
  uint64_t sgi = ((aff & 0xff00000000ULL) << 16) | ((aff & 0xff0000) << 16) | ((aff & 0xff00) << 8) | (1ULL << (aff & 0xff));
  armcpu_ipi_selftest_sent = armcpu_ipi_selftest_sent + 1;
  if (gSpiIpi) { spiIpiSend (gDist, static_cast<uint32_t> ((aff >> 8) & 7)); return; }
  __asm__ volatile ("dsb ishst\n\tmsr icc_sgi1r_el1, %0\n\tisb" :: "r" (sgi) : "memory");
}

void ArmCpu::enableCPUTimeBase (bool enabled)
{
  uint64_t value = enabled ? 1 : 2;
  __asm__ volatile ("msr cntv_ctl_el0, %0\n\tisb" :: "r" (value) : "memory");
}

void ArmCpu::quiesceCPU (void)
{
  armcpu_quiesce_calls=armcpu_quiesce_calls+1;
  if(diagnosePower)panic("ArmCpu: unsupported quiesce requested before timer/GIC disable caller=%p halt=%u timer=%llu phase=%u",__builtin_return_address(0),armcpu_halt_calls,armcpu_timer_interrupts,armcpu_irq_phase);
  enableCPUTimeBase (false);
  if (gGicV2) {
    write32 (gCpuIf, 0, read32 (gCpuIf, 0) & ~1U);
    __asm__ volatile ("dsb sy\n\tisb" ::: "memory");
    return;
  }
  uint64_t value = 0;
  __asm__ volatile ("msr icc_igrpen1_el1, %0\n\tdsb sy\n\tisb" :: "r" (value) : "memory");
}

void ArmCpu::haltCPU (void)
{
  armcpu_halt_calls=armcpu_halt_calls+1;
  quiesceCPU ();
  setCPUState (kIOCPUStateStopped);
  __asm__ volatile ("msr daifset, #15" ::: "memory");
  for (;;) { __asm__ volatile ("wfi"); }
}

// The new core runs with its MMU and caches off until start_cpu enables them, so it reads the reset vector, ResetHandlerData
// and the boot arguments from memory. Write those lines back to the point of coherency first: on this SoC the boot CPU's
// dirty cache lines are not guaranteed to be visible to a core with caches off (XNU only flushes the per-CPU data).
static void cleanToMemory (const void *start, size_t length)
{
  uint64_t ctr;
  __asm__ volatile ("mrs %0, ctr_el0" : "=r" (ctr));
  uintptr_t line = static_cast<uintptr_t> (4) << ((ctr >> 16) & 15);
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~(line - 1), end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += line) { __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory"); }
  __asm__ volatile ("dsb sy\n\tisb" ::: "memory");
}

kern_return_t ArmCpu::startCPU (vm_offset_t, vm_offset_t)
{
  if (!secondary) { return KERN_NOT_SUPPORTED; }
  bool hvc = false;
  if (!psciConduit (&hvc)) { armcpu_smp_psci_rc[logicalId] = -1001; return KERN_NOT_SUPPORTED; }
  armcpu_smp_conduit = hvc ? 2 : 1;
  // PSCI 0.2 CPU_ON (SMC64 function id); the entry point is the kernel's physical reset vector page, whose reset
  // code selects this core's cpu_data entry by MPIDR affinity. The context id is unused by that code.
  uint64_t entry = static_cast<uint64_t> (ml_static_vtop (reinterpret_cast<vm_offset_t> (LowResetVectorBase)));
  cleanToMemory (LowResetVectorBase, 0x10000);   // reset vector, start_cpu and the code up to the MMU enable
  cleanToMemory (ResetHandlerData, 0x1000);
  cleanToMemory (const_boot_args, 0x1000);
#if ARMCPU_GICV2
  if (gGicV2) {
    // The MT8167 Trusted Firmware can start the core at EL2, while LowResetVectorBase expects EL1: enter through the
    // EL2-to-EL1 trampoline below, which branches to LowResetVectorBase. start_cpu also reads TCR and the kernel TTBR
    // with caches off; the MT8167 port writes both back before CPU_ON (fee5652, after ios-sweet patch 0065).
    entry = static_cast<uint64_t> (ml_static_vtop (reinterpret_cast<vm_offset_t> (armcpu_gicv2_secondary_entry)));
    cleanToMemory (armcpu_gicv2_secondary_entry, kGicV2EntryBytes);
    cleanToMemory (sysreg_restore, sizeof (uint64_t));
    cleanToMemory (&cpu_ttep, sizeof (cpu_ttep));
  }
#endif
  armcpu_smp_step[logicalId] = 4;
  int64_t result = psciCall (hvc, 0xc4000003ULL, affinity & 0xff00ffffffULL, entry, 0);
  armcpu_smp_psci_rc[logicalId] = static_cast<int32_t> (result);
  return result == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

const OSSymbol *ArmCpu::getCPUName (void)
{
  return cpuName;
}

// Readback after configuration: bit 0 group 1, bit 1 enabled, bit 2 edge, bits 8..15 priority; IROUTER separately.
extern "C" IOReturn armcpu_spi_attach (uint32_t intid, bool edge, void (*handler) (void *), void *context,
                                       uint32_t *readback, uint64_t *router)
{
  if (gDist == 0 || armcpu_gic_ready == 0 || handler == nullptr) { return kIOReturnNotReady; }
#if ARMCPU_SPI_SERIALIZE
  SpiRegistrationGuard guard;
#endif
  const unsigned interrupts = ((read32 (gDist, 4) & 31) + 1) * 32;
  if (intid < 32 || intid >= 1020 || intid >= interrupts) { return kIOReturnBadArgument; }
  const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  for (uint32_t slot = 0; slot < used; slot++) {
    if (gSpi[slot].id == intid) { return kIOReturnExclusiveAccess; }
  }
  if (used >= ARMCPU_SPI_SLOTS) { return kIOReturnNoResources; }
  const uint32_t word = (intid / 32) * 4, bit = 1U << (intid % 32);
  write32 (gDist, 0x180 + word, bit); // ICENABLER while it is reconfigured
  if (!gGicV2 && !waitClear (gDist, 0, 1U << 31)) { return kIOReturnTimeout; }
  write32 (gDist, 0x80 + word, read32 (gDist, 0x80 + word) | bit); // IGROUPR: group 1 (RAZ/WI if secure owns it)
  *reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid) = 0xa0;
  const unsigned cfg = 0xc00 + (intid / 16) * 4, shift = (intid % 16) * 2 + 1;
  write32 (gDist, cfg, (read32 (gDist, cfg) & ~(1U << shift)) | ((edge ? 1U : 0U) << shift));
  if (gGicV2) {
    *reinterpret_cast<volatile uint8_t *> (gDist + 0x800 + intid) = static_cast<uint8_t> (gBootAffinity); // GICD_ITARGETSR
  } else {
    *reinterpret_cast<volatile uint64_t *> (gDist + 0x6000 + 8 * intid) = gBootAffinity;
  }
  gSpi[used] = {intid, handler, context, 0};
  __atomic_store_n (&gSpiUsed, used + 1, __ATOMIC_RELEASE);
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gDist, 0x100 + word, bit); // ISENABLER
  if (readback != nullptr) {
    *readback = ((read32 (gDist, 0x80 + word) & bit) ? 1U : 0U) | ((read32 (gDist, 0x100 + word) & bit) ? 2U : 0U) |
                (((read32 (gDist, cfg) >> shift) & 1U) << 2) |
                (static_cast<uint32_t> (*reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid)) << 8);
  }
  if (router != nullptr) {
    *router = gGicV2 ? *reinterpret_cast<volatile uint8_t *> (gDist + 0x800 + intid)
                     : *reinterpret_cast<volatile uint64_t *> (gDist + 0x6000 + 8 * intid);
  }
  return kIOReturnSuccess;
}

#if ARMCPU_SPI_SERIALIZE
extern "C" bool armcpu_spi_boot_ready (void)
{
  return __atomic_load_n (&gSpiBootReady, __ATOMIC_ACQUIRE) != 0;
}

// MTK's low-active input is inverted by SYSIRQ before the GIC. All users of
// this API share the SPI registration lock for SYSIRQ and distributor RMWs.
// The caller owns the mapping for the lifetime of any installed handler.
extern "C" IOReturn armcpu_spi_attach_sysirq (uint32_t spi, volatile uint32_t *polarity,
                                              void (*handler) (void *), void *context,
                                              uint32_t *readback, uint64_t *router, bool *installed)
{
  if (installed) *installed = false;
  if (!installed || !polarity || !handler || !gGicV2 || !gDist || !armcpu_gic_ready || !gBootAffinity) {
    if (readback) *readback = 0x100u | (installed ? 1u : 0u) | (polarity ? 2u : 0u) |
      (handler ? 4u : 0u) | (gGicV2 ? 8u : 0u) | (gDist ? 16u : 0u) |
      (armcpu_gic_ready ? 32u : 0u) | (gBootAffinity ? 64u : 0u);
    return kIOReturnNotReady;
  }
  SpiRegistrationGuard guard;
  uint32_t intid = spi + 32;
  unsigned count = ((read32 (gDist, 4) & 31) + 1) * 32;
  if (spi >= 256 || intid >= count || intid >= 1020) return kIOReturnBadArgument;
  uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  for (uint32_t i = 0; i < used; ++i)
    if (gSpi[i].id == intid) return kIOReturnExclusiveAccess;
  if (used >= ARMCPU_SPI_SLOTS) return kIOReturnNoResources;
  uint32_t word = (intid / 32) * 4, bit = 1u << (intid % 32), polBit = 1u << (spi % 32);
  if (read32 (gDist, 0x100 + word) & bit) return kIOReturnExclusiveAccess;
  unsigned cfg = 0xc00 + (intid / 16) * 4, shift = (intid % 16) * 2 + 1;
  uint32_t oldPol = *polarity, oldGroup = read32 (gDist, 0x80 + word), oldCfg = read32 (gDist, cfg);
  uint8_t oldPriority = *reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid);
  uint8_t oldTarget = *reinterpret_cast<volatile uint8_t *> (gDist + 0x800 + intid);
  *polarity = oldPol | polBit;
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gDist, 0x80 + word, oldGroup | bit);
  *reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid) = 0xa0;
  write32 (gDist, cfg, oldCfg & ~(1u << shift));
  *reinterpret_cast<volatile uint8_t *> (gDist + 0x800 + intid) = static_cast<uint8_t> (gBootAffinity);
  __asm__ volatile ("dsb sy" ::: "memory");
  uint32_t group = read32 (gDist, 0x80 + word), currentCfg = read32 (gDist, cfg);
  uint8_t priority = *reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid);
  uint8_t target = *reinterpret_cast<volatile uint8_t *> (gDist + 0x800 + intid);
  bool valid = (*polarity & polBit) && (group & bit) && !(currentCfg & (1u << shift)) &&
               priority == 0xa0 && target == static_cast<uint8_t> (gBootAffinity);
  if (valid) {
    gSpi[used] = {intid, handler, context, 0};
    __atomic_store_n (&gSpiUsed, used + 1, __ATOMIC_RELEASE);
    *installed = true;
    write32 (gDist, 0x100 + word, bit);
    valid = (read32 (gDist, 0x100 + word) & bit) != 0;
  }
  if (!valid) {
    if (readback) *readback = 0x200u | ((*polarity & polBit) ? 1u : 0u) |
      ((group & bit) ? 2u : 0u) | (!(currentCfg & (1u << shift)) ? 4u : 0u) |
      (priority == 0xa0 ? 8u : 0u) | (target == static_cast<uint8_t> (gBootAffinity) ? 16u : 0u) |
      (*installed ? 32u : 0u);
    if (router) *router = target;
    // W1C masks only this SPI. Do it before restoring the polarity.
    write32 (gDist, 0x180 + word, bit);
    __asm__ volatile ("dsb sy" ::: "memory");
    if (read32 (gDist, 0x100 + word) & bit) {
      __atomic_fetch_add (&armcpu_spi_mask_failures, 1, __ATOMIC_RELAXED);
      __atomic_store_n (&armcpu_spi_mask_failed_id, intid, __ATOMIC_RELAXED);
      // An unmasked line must have a handler even when setup failed before
      // publishing one. Its owner keeps its state and suppresses the source.
      if (!*installed) {
        gSpi[used] = {intid, handler, context, 0};
        __atomic_store_n (&gSpiUsed, used + 1, __ATOMIC_RELEASE);
        *installed = true;
      }
    }
    *polarity = (*polarity & ~polBit) | (oldPol & polBit);
    write32 (gDist, 0x80 + word, (read32 (gDist, 0x80 + word) & ~bit) | (oldGroup & bit));
    write32 (gDist, cfg, (read32 (gDist, cfg) & ~(1u << shift)) | (oldCfg & (1u << shift)));
    *reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid) = oldPriority;
    *reinterpret_cast<volatile uint8_t *> (gDist + 0x800 + intid) = oldTarget;
    __asm__ volatile ("dsb sy" ::: "memory");
    return kIOReturnNotReady;
  }
  if (readback) *readback = 3u | (uint32_t (priority) << 8);
  if (router) *router = target;
  return kIOReturnSuccess;
}

// Interrupt-safe: ICENABLER is write-one-to-clear and never changes another
// SPI. Require the exact registered owner; no spin lock or blocking in IRQ.
extern "C" bool armcpu_spi_mask_owned (uint32_t intid, void (*handler) (void *), void *context)
{
  uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  for (uint32_t i = 0; gDist && i < used; ++i) {
    if (gSpi[i].id == intid && gSpi[i].handler == handler && gSpi[i].context == context) {
      uint32_t word = (intid / 32) * 4, bit = 1u << (intid % 32);
      write32 (gDist, 0x180 + word, bit);
      __asm__ volatile ("dsb sy" ::: "memory");
      if (!(read32 (gDist, 0x100 + word) & bit)) return true;
      __atomic_fetch_add (&armcpu_spi_mask_failures, 1, __ATOMIC_RELAXED);
      __atomic_store_n (&armcpu_spi_mask_failed_id, intid, __ATOMIC_RELAXED);
      return false;
    }
  }
  return false;
}
#endif

extern "C" uint64_t armcpu_spi_count (uint32_t intid)
{
  const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  for (uint32_t slot = 0; slot < used; slot++) {
    if (gSpi[slot].id == intid) { return gSpi[slot].count; }
  }
  return 0;
}

#if ARMCPU_GICV2
// PSCI CPU_ON entry of a GICv2 (MT8167) secondary core. It runs at its physical address with the MMU off. From EL2 it
// selects AArch64 EL1 with no stage 2 and no EL2 traps, gives EL1 the physical counter and timer, sets CNTVOFF_EL2 to 0
// and erets to EL1h with DAIF masked; from EL1 it continues directly. It then enters LowResetVectorBase, which selects
// the CPU data by MPIDR (the PSCI context id in x0 is unused). Sequence from a04br3_secondary_entry of the A04BR3 XNU
// port (mouseos/xnu_kernel_benesse_a04br3 fee5652, osfmk/arm64/start.s), which brought up all four MT8167 cores.
// CPTR_EL2 is written with its ARMv8.0 RES1 bits and MDCR_EL2.HPMN with PMCR_EL0.N (as Linux init_el2_state does).
// No GICv3 system register is touched: GIC-400 has no system register interface.
__asm__ (
  "  .text\n"
  "  .p2align 8\n"
  "  .globl _armcpu_gicv2_secondary_entry\n"
  "_armcpu_gicv2_secondary_entry:\n"
  "  msr DAIFSet, #0xf\n"
  "  mrs x1, CurrentEL\n"
  "  cmp x1, #0x4\n"                          // EL1
  "  b.eq Larmcpu_gicv2_el1\n"
  "  cmp x1, #0x8\n"                          // EL2
  "  b.ne Larmcpu_gicv2_bad_el\n"
  "  adrp x1, _LowExceptionVectorBase@PAGE\n"
  "  add x1, x1, _LowExceptionVectorBase@PAGEOFF\n"
  "  msr VBAR_EL2, x1\n"
  "  mov x1, #0x80000000\n"                   // HCR_EL2: RW (EL1 is AArch64), nothing else
  "  msr HCR_EL2, x1\n"
  "  msr VTTBR_EL2, xzr\n"
  "  mov x1, #0x33ff\n"                       // CPTR_EL2: RES1 bits 13:12 and 9:0, TFP/TTA/TCPAC clear
  "  msr CPTR_EL2, x1\n"
  "  mrs x1, PMCR_EL0\n"
  "  ubfx x1, x1, #11, #5\n"                  // MDCR_EL2.HPMN = PMCR_EL0.N, no debug or PMU traps
  "  msr MDCR_EL2, x1\n"
  "  mov x1, #3\n"                            // CNTHCTL_EL2: EL1PCTEN | EL1PCEN
  "  msr CNTHCTL_EL2, x1\n"
  "  msr CNTVOFF_EL2, xzr\n"
  "  mrs x1, MIDR_EL1\n"                       // EL1 reads of MIDR/MPIDR return VPIDR_EL2/VMPIDR_EL2, which reset UNKNOWN:
  "  msr VPIDR_EL2, x1\n"                     // copy the physical values as Linux init_el2 does (start_cpu selects
  "  mrs x1, MPIDR_EL1\n"                     // this core's cpu_data by MPIDR)
  "  msr VMPIDR_EL2, x1\n"
  "  mov x1, #0x0800\n"
  "  movk x1, #0x30d0, lsl #16\n"             // SCTLR_EL1 0x30d00800: ARMv8.0 RES1 bits, MMU and caches off, little endian
  "  msr SCTLR_EL1, x1\n"
  "  isb\n"
  "  adr x1, Larmcpu_gicv2_el1\n"
  "  msr ELR_EL2, x1\n"
  "  mov x1, #0x3c5\n"                        // SPSR_EL2: D, A, I, F masked, EL1h
  "  msr SPSR_EL2, x1\n"
  "  eret\n"
  "Larmcpu_gicv2_bad_el:\n"
  "  wfe\n"
  "  b Larmcpu_gicv2_bad_el\n"
  "Larmcpu_gicv2_el1:\n"
  "  msr SPSel, #1\n"
  "  adrp x1, _LowResetVectorBase@PAGE\n"
  "  add x1, x1, _LowResetVectorBase@PAGEOFF\n"
  "  br x1\n"
);
#endif
