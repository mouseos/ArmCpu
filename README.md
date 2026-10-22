# ArmCpu

ARM64 CPU, GIC and timer driver for UniXNU. Publishes the CPU platform device, the interrupt controller and the architected timer.

Current state: starts the CPU, GIC and timer on sheng and on the QEMU virt machine.

Linked into XNU by `unixnu build kernel`.

## GICv2 (optional)

Boards with a GICv2 interrupt controller (GIC-400, for example the MediaTek MT8167 of the Benesse TAB-A04-BR3) build
the driver with `CONFIG_XNU_ARMCPU_GICV2=y` in their defconfig, which compiles ArmCpu with `ARMCPU_GICV2=1`. The GICv2
path is then selected by `/arm-io/intc` `compatible = "arm,gic-400"` or `"arm,cortex-a15-gic"`, whose `reg` holds the
distributor and the CPU interface. Without the flag the GICv2 code is not compiled and the GICv3 driver is unchanged.

The GICv2 register use (non-secure group, GICC_IAR/GICC_EOIR, GICD_SGIR with the banked GICD_ITARGETSR0 target bit,
GICD_ITARGETSR routing for SPIs) follows the MT8167 bring-up in
[xnu_kernel_benesse_a04br3](https://github.com/mouseos/xnu_kernel_benesse_a04br3) (pexpert/arm/pe_a04br3_gic.c), which
ran on that tablet.
