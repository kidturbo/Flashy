# Welcome to Flashy

**Flashy** is a J2534-style Pass-Thru tool for **diagnostics, reversing, and programming automotive ECUs**. It bridges a PC to the vehicle's CAN bus (over OBD-II or bench connection) to read, write, and interact with firmware on Engine Control Modules (ECMs), Transmission Control Modules (TCMs), Body Control Modules (BCMs), and any other node on the CAN network.

The project is being actively expanded. It currently has verified support for the modules listed below, with new module families added as they're tested.

The device is built on the [Adafruit Feather M4 CAN Express](https://www.adafruit.com/product/4759) and a paired [AdaLogger FeatherWing](https://www.adafruit.com/product/2922) for on-device SD logging. Firmware is MIT-licensed and the design is open for anyone to build, modify, or port to other CAN-capable hardware.

This wiki is written for users — building the hardware, flashing the firmware, running your first read. For protocol-level and module-internal details, jump to the [Documentation Index](Documentation-Index).

---

## Getting Started

| | |
|---|---|
| [Hardware Assembly](Hardware-Assembly) | Stack the boards, break the 120Ω terminator, wire the OBD-II cable |
| [Getting Started](Getting-Started) | Flash the firmware and connect your first time |
| [Downloads](Downloads) | What's in the `Flashy-Tool.zip` release and how to use it |
| [Documentation Index](Documentation-Index) | All the deep technical docs in one place |

---

## Quick Links

- **Repo:** [github.com/kidturbo/Flashy](https://github.com/kidturbo/Flashy)
- **Latest release:** [Flashy-Tool.zip](https://github.com/kidturbo/Flashy/releases/latest)
- **Report a bug:** [New issue](https://github.com/kidturbo/Flashy/issues/new?template=bug_report.md)
- **Request a feature (or new module support):** [New issue](https://github.com/kidturbo/Flashy/issues/new?template=feature_request.md)

## Currently Tested Modules

For the full verification matrix (MCU, memory map, kernel status) see the [ECU Reference Guide](https://kidturbo.github.io/Flashy/ecu-reference.html).

- **ECMs:** E38, E67, E92 (read/write verified), E78, E80, E82, E83, E84, E39/E39A (verified, kernels pending)
- **TCMs:** T87, T87A (read/write verified)
- **BCMs and other CAN nodes:** planned — if you have a module you'd like supported, [open a feature request](https://github.com/kidturbo/Flashy/issues/new?template=feature_request.md)

## Hardware at a Glance

![Feather M4 CAN Express](https://raw.githubusercontent.com/kidturbo/Flashy/main/docs/images/hardware/Flashy1.jpg)

*Adafruit Feather M4 CAN Express — the heart of Flashy. Screw terminals on the left are the CAN high/low/ground connection.*
