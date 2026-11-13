# Synthetic checksum and protocol onboarding

Run `python3 examples/synthetic/demo.py` from the repository root. The eight-byte checksum buffer and CRC check vector are invented/non-firmware examples. The demo detects a deliberate one-bit mutation and shows that a zero-filled3MiB buffer is rejected by the production profile. It does not replace that guard or construct an accepted synthetic ECU image.

Run `PYTHONPATH=tools python3 -m unittest discover -s tests -v` for additional algorithm, malformed-transfer and signature-provenance checks. Tests requiring the real private baseline skip explicitly; no firmware download or device connection is attempted.
