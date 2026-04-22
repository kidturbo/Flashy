# BAM Mode Wiring (Bench)

**BAM** (Boot Assist Mode) is a low-level boot mode available on many PowerPC-based automotive MCUs. When the MCU is strapped into BAM at reset, it listens on its CAN peripheral for a small boot payload — bypassing the application firmware entirely. This is how Flashy can recover a module with corrupted or unsigned application code, or read out flash from a module whose normal HS-CAN programming mode is locked down.

This page collects the bench wiring details for each module known to support BAM entry from Flashy.

> **Scope:** This page is about the *physical wiring* needed to power the module and put it into BAM. For the protocol/command side (`BAMREAD`, `BAMWRITE`), see the [Read / Write Instructions](https://kidturbo.github.io/Flashy/Read-Write-Instructions.html).

---

## T87A TCM (SPC564A80)

The T87A transmission control module uses the SPC564A80 (ST Qorivva, e200z4 VLE core). BAM entry is triggered by flooding a specific CAN ID during the module's power-up window, before the application firmware takes over.

### Connector Pinout

The T87A shares its connector pinout with the T87 and T93 TCMs (same physical connector, different firmware). The Flashy-relevant pins are:

| Pin | Function | Wire to |
|-----|----------|---------|
| 37 | CAN-H | Flashy CAN-H screw terminal |
| 38 | CAN-L | Flashy CAN-L screw terminal |
| 65 | GND | Bench supply negative + Flashy GND screw terminal |
| 66 | B+ (battery) | Bench supply +12V |
| 35 | IGN (ignition) | Bench supply +12V (switched) |
| 51 | IGN (ignition) | Bench supply +12V (switched, alt pin) |

> **Bench supply:** A 5A @ 12V bench supply is sufficient for TCM powerup. The TCM draws ~1-2A idle.

> **Ground bonding:** Tie bench supply GND to Flashy's GND terminal. **Do not** share the +12V rail with Flashy (Flashy is USB-powered from the PC).

<!-- TODO: Add bench pinout photo showing T87A connector with pin numbers -->
<!-- Suggested filename: wiring/t87a-connector-pinout.jpg -->

*[Connector pinout photo to be added]*

### BAM Entry Sequence

BAM is entered by the Flashy `BAMREAD` or `BAMWRITE` commands. The software side:

1. Issues a heartbeat flood on CAN ID `0x7E2` every 2 ms
2. Waits for the echoed "RAMEXEC" response from the module
3. Continues BAM communication on CAN IDs `0x026` / `0x027` at 500 kbps

The user just has to power-cycle the module at the right moment — Flashy handles the timing once `BAMREAD` is running.

<!-- TODO: Add photo of bench setup showing T87A wired to bench supply + Flashy -->

*[Bench setup photo to be added]*

### Verified Status

- **BAMREAD:** working — 4 MB in ~7 minutes at 9.2 KB/s, zero errors
- **BAMWRITE:** working — 23 sectors in ~6 minutes, verified against JTAG readback
- **BAM flash range:** reads/writes start at `0x020000` (OS region). Boot sector `0x000000–0x01FFFF` is not accessible via BAM.

### Notes and Gotchas

- **Power sequencing matters.** Start `BAMREAD` in Flashy *before* powering on the module. The heartbeat flood must already be on the bus when the module boots.
- **Bus speed is fixed at 500 kbps** for BAM on this MCU family.
- **Don't add a third terminator.** If you're on a bench harness with a single module, the bus likely needs 120Ω somewhere. Either leave Flashy's onboard terminator intact, or add an external one — but not both.

---

## Other Modules

<!-- TODO: Add sections for other BAM-capable modules as they're verified -->

BAM-capable modules to document next (from the [ECU Reference Guide](https://kidturbo.github.io/Flashy/ecu-reference.html)):

- **E92 ECM** (MPC5674F) — BAM protocol: MPC5XXXBam / kernel: MPC5674BAM.dat
- **E80 ECM** (MPC5676R) — BAM protocol: MPC5XXXBam / kernel: MPC5676BAM.dat
- **E78 / E39 ECMs** (MPC5566) — BAM kernel: MPC5566BAM.dat

---

## Safety

- **Always verify +12V and GND polarity** before powering up a module. Reversed polarity will destroy it instantly.
- **Use a bench supply with current limiting** set to 3–5A. If the module draws more than expected on power-up, something is miswired.
- **Isolate Flashy's USB ground from high-current paths.** The CAN transceiver's ground reference is tied to USB ground. A bad bench ground can push noise into the PC.
