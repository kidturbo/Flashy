# e67_read — placeholder

Clean-room read Kernel for the GM **E67** ECM. **Not yet built.**

## MCU Confirmed: Motorola MPC565 (2026-04-16)

The E67 uses a **Motorola MPC565** — an MPC500-core PowerPC, NOT MPC55xx (e200).
This is a completely different processor generation from the E92 (MPC5674F).

**Key specs:**
- Core: MPC500 (embedded PowerPC, 32-bit, 40-66 MHz)
- External flash: Spansion S29CD016 (2 MB burst) — chip has 1 MB internal flash but GM doesn't use it
- SRAM: 80 KB at `0x003F8000`–`0x0040BFFF`
- CAN: 3× TouCAN (NOT FlexCAN — different register map)
- Debug: BDM (not JTAG/Nexus)
- Slave CPU: MC9S12C32 (Drive-By-Wire)

**Load address mystery SOLVED:** A reference E67 kernel observed at load address `0x003FC400` falls squarely within MPC565 SRAM. It was never MPC5565 — that's why the address looked wrong.

Sources: BitBox/Chiptuningshop, IO Terminal, community forums, rusefi wiki (all independently confirm MPC565).

## Why it's empty

Our first Kernel build targeted load address `0x40001000`, which matches an E92 reference kernel and the E92 bench unit we used for testing. That project now lives at `Cernels/e92_read/`.

A separate kernel exists for E67 (different reference kernel, ~1862 bytes, load address **`0x003FC400`**). The E67 uses entirely different silicon (MPC565 vs MPC5674F) with a different CAN peripheral (TouCAN vs FlexCAN), so no E92 code can be reused.

## What needs to happen before this folder gets code

1. **Write a TouCAN driver** from the MPC565 Reference Manual — TouCAN has a different register layout from FlexCAN.
2. **Write a new linker script** targeting `0x003FC400` in MPC565 SRAM.
3. **Write new startup code** for MPC500 core (different from e200z7 Book E).
4. **Test on real E67 hardware** — we have a working bench unit (VIN 1G1ZJ577184252938).

This is a ground-up effort — the E92 Kernel's `flexcan.h`, `start.S`, and `kernel.ld` do not apply here.
