# Worked acquisition and runtime boundaries

The worked case used native Linux ARM64, RH02 Elite running Elmue CANable 2.5 SLCAN firmware 2490643, and classical CAN at 500 kbit/s. Other hosts/backends need their own checks. Adapter FD marketing and virtual FD tests did not establish physical FD operation.

Moulded OBD pin numbers resolved the early empty capture: 6 to H, 14 to L, 5 to isolated bus-side ground; adapter termination OFF on the existing vehicle network. OBD16 and adapter 5V were unused in this setup. This documents that setup, not a universal connector/bench wiring recipe. Elmue silent open is OS; bare L reports bus load. Generic slcand listen-only initialization is incompatible with this firmware dialect.

Before active requests, verify firmware, wiring, grounding, bus conditions and actual passive reception. Configuration acknowledgements alone are not received traffic. A physical read-only diagnostic request still transmits. Keep a CAN process on the physically connected host and protect it from suspend and session loss.

Two matching main-flash reads completed in 225.34 seconds. Coverage was 0x00000000–0x002FFFFF, 3,145,728 bytes. The pinned kernel synthesizes 64 FF bytes at 0x3FE0–0x3FFF and 0x1FFE0–0x1FFFF. Shadow/password data is excluded; this is not a complete cloning backup. No persistent flash write occurred.

The accepted read sequence depended on the published stock host and exact kernel, including stock broadcast suppression 28→68 before programming preparation. Omitting that step caused 7FA522. Stock diagnostics, stock loader and running kernel have distinct service dialects. The kernel dummy seed is not a stock unlock method. This package publishes no captured security seed/key exchanges and no live uploader.

The pinned release kernel is 30,704 bytes, entry 0x40014000, SHA-256 f694f67e639db1e4465797545c3cca4b00883bd5f068ae2a3bf3dcd08d97c861. A ready99 gate precedes kernel reads; matching byte counts/sequences, final77 and a second matching read were required. Upstream references are in sources.md; a reader must be separately validated before use.

After completed RAM-kernel execution, ignition OFF/ON did not restore ECM diagnostics on this vehicle. A reported full power cycle restored matching identities/CVNs. Kernel1002 returns to a loader path, not normal engine control; no normal kernel reset was established. Do not infer a disconnect requirement for a failure before RAM execution or generalize successful restoration to recovery after damaged flash.

This case kept ignition OFF throughout preparation, engine OFF, and used freshly confirmed bounded RUN batches followed immediately by an OFF instruction. Historical battery-only success is not a universal power specification. PID42 returned zero and was unusable for this vehicle's voltage monitoring. Follow the exact tool/vehicle power and restoration requirements for a new operation.

Later work verified fixed stock diagnostic channels and selected ordinary service23 RAM reads without a kernel, unlock or session change. Only the exact prepared allowlists were tested; restricted/security ranges remain excluded. The earlier address-zero rejection did not test ordinary RAM. The final stationary pedal batch has limited matching samples, not whole-map/loaded validation. No additional sweep or live read is part of this release task.
