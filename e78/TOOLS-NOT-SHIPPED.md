# Included and excluded acquisition/research components

The Flashy integration now includes the injected original-read state machine,
packet decoder and a pure identity decoder. These are public source, with
synthetic transport tests; they provide no serial/CAN backend or live launcher.
See [current integration scope](docs/flashy-integration.md).

The following is the earlier minimal-kit inventory. Its statement that the
reader source was not bundled is superseded for `e78_read_workflow.py` and
`e78_read_protocol.py` by this contribution. Adapter helpers remain excluded.


The minimal kit assumes you already have an appropriate private original. A public end-to-end reader tier is a planned separate deliverable, not hidden in the six-module closure.

The originating verified stock acquisition implementation uses `rh02_read_original.py`, `e78_read_workflow.py`, `e78_read_protocol.py`, `rh02_kernel_transport.py`, `rh02_identify.py` and `rh02_passive.py`. It needs POSIX serial support, can-isotp and lz4, an identity record, fixed pinned kernel, exact compatible firmware/adapter, a bounded session and restoration plan. Its direct serial transport uses termios/fcntl rather than pyserial. The originating environment additionally had python-can4.6.1, can-isotp2.0.7, pyserial3.5 and lz44.4.5; that environment lock is not the minimum direct-reader dependency list. These private reader sources are not bundled or offered as publicly downloadable paths.

Runtime tools include fixed diagnostic/RAM capture and analyzers. Their imports/provenance rely on private reference exports and saved trace/capture files; copying a lone helper would create a broken or unsafe distribution. A future reader/logging release must remove owner-specific defaults, require explicit adapter/identity inputs, retain fixed allowlists and fresh traffic gates, preserve stock/kernel dialect boundaries and pass offline PTY/negative tests before any separately authorized target validation.

Optional reverse engineering uses Ghidra12.1.4_DEV in the recorded case, bounded noanalysis traces and an SPE-capable PowerPC decoding model. The language choice is not a verified MCU identity. E78Trace.java and E78SparkLookupEmulate.java are existing research helpers; they are not included in this minimal release. The optional exact-image definition exporter here does not require a Ghidra database because it exports already established definitions and records their provenance, rather than re-running disassembly.

SavvyCAN/can-utils may assist capture work; GM-ECU-Simulator is protocol simulation; EFIGenieE78 is a custom firmware/bootloader-patch project. None substitutes for a validated stock calibration writer. No unsupported live command or recovery recipe is supplied to fill that gap.
