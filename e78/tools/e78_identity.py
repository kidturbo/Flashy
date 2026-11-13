# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure identity decoding for the E78 read workflow; no device access.

Adapted from the originating RH02 identity helper. Only the three identifiers
consumed by E78ReadWorkflow are included; no scanner or serial dependency.
"""


def decode_result(request, response):
    """Decode a complete application payload, rejecting malformed identities."""
    if request not in (b"\x09\x02", b"\x09\x04", b"\x1a\xc9"):
        raise ValueError("Identity identifier is outside the read workflow")
    if response[:2] != bytes((request[0] + 0x40, request[1])):
        raise ValueError("Identity response service or identifier mismatch")
    if request == b"\x09\x02" and len(response) == 20 and response[2] == 1:
        return {"vin": response[3:].decode("ascii")}
    if request == b"\x09\x04" and len(response) >= 3:
        count = response[2]
        if count and len(response) == 3 + count * 16:
            return {
                "calibration_ids": [
                    response[3 + n * 16 : 19 + n * 16].decode("ascii").rstrip("\x00 ")
                    for n in range(count)
                ]
            }
    if request == b"\x1a\xc9" and len(response) == 6:
        return {"gm_1ac9_identifier": int.from_bytes(response[2:6], "big")}
    raise ValueError("Malformed identity payload")
