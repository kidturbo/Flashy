#!/usr/bin/env python3
"""Export exact-image static definitions and cells. No ECU I/O or binary writes."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import struct

from e78_candidate import ORIGINAL

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ORIGINAL
SOURCE_EXPORTER_SHA = "5bb7f32cfb6aa81f09e4b5d05415f06bcaab20590095b58b8aa01b39bac846e4"
SHA = "3b9b58421977ae2a5fb0f5d4d1bf71702cd97922109a215cab1ba9405e3920ba"
REFERENCE_SHA = "3337f2ab184cb7e2cb92155705fb3d914accaa6aa75c3eec64426682eca43719"
CAN_SHA = "ec287798ec7f72cc62bdf18b10be8f563ef04784e193908779280b3a5ff298f1"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_values(data, address, count, code="f", factor=1.0):
    size = struct.calcsize(">" + code)
    if count < 1 or address < 0 or address + count * size > len(data):
        raise ValueError("Data outside image")
    raw = data[address : address + count * size]
    values = list(struct.unpack(">" + str(count) + code, raw))
    scaled = [v * factor for v in values]
    if not all(math.isfinite(v) for v in scaled):
        raise ValueError("Nonfinite calibration value")
    return dict(
        address=f"0x{address:06X}",
        count=count,
        byte_length=len(raw),
        byte_order="big",
        storage={"f": "IEEE754 binary32", "H": "uint16", "h": "int16"}[code],
        struct_code=code,
        factor=factor,
        offset=0,
        sha256=sha(raw),
        raw_values=values,
        values=scaled,
    )


def axis(data, address, count, name, unit, input_state, qualification):
    result = read_values(data, address, count)
    if any(a >= b for a, b in zip(result["values"], result["values"][1:])):
        raise ValueError(f"Axis not strictly increasing: {address:x}")
    result.update(name=name, unit=unit, input_state=input_state, evidence=qualification)
    return result


def build(data):
    if len(data) != 0x300000 or sha(data) != SHA:
        raise ValueError(
            "Requires the exact verified original; OS number alone is insufficient"
        )
    tables = []

    def add(
        identifier,
        name,
        address,
        rows,
        columns,
        row,
        column,
        unit,
        role,
        selection,
        evidence,
        limitations,
        code="f",
        factor=1.0,
        group="primary",
    ):
        t = read_values(data, address, rows * columns, code, factor)
        t.update(
            id=identifier,
            name=name,
            group=group,
            shape=[rows, columns],
            layout="row-major; column index changes fastest",
            row_axis=row,
            column_axis=column,
            unit=unit,
            role=role,
            selection=selection,
            evidence=evidence,
            limitations=limitations,
            editable=False,
            flash_ready=False,
            definition_status="static code and source corroboration",
            interpolation="linear / bilinear; axis endpoint clamping",
            interpolation_limit="Review mathematics; no claim of bit-exact SPE emulation",
        )
        tables.append(t)

    temperature = axis(
        data,
        0x4E4F0,
        7,
        "Boost-limit temperature input",
        "degC",
        "r14-2CD0",
        "ADC lookup and +273.15 conversion; physical sensor location unverified",
    )
    rpm = axis(
        data,
        0x53AD0,
        16,
        "Engine speed",
        "rpm",
        "r14-2448",
        "Own period/rate producer; sibling rate corroborated by CAN 0C9 quarter-RPM packing",
    )
    blend = axis(
        data,
        0x53B10,
        4,
        "Lowest possible ethanol percentage (reference role)",
        "%",
        "r14-3198 = uint16[r13-50F0] * 100 / 65536",
        "Percent is code-derived; ethanol name from reference. ROM 0531A8=0 selects zero producer",
    )
    pedal = axis(
        data,
        0x47C1C,
        17,
        "Pedal request",
        "%",
        "r14-3924 or calculated r14-3950",
        "Own percentage producer, CAN pedal field, alternate inverse interpolation with 0..100 clamp",
    )
    speed = axis(
        data,
        0x47D0C,
        17,
        "Selected vehicle-speed input",
        "km/h",
        "r14-386C",
        "ROM 0465B5=0 direct branch; received 0F9 source and tire conversion corroborated by both saved drives",
    )
    add(
        "boost_pressure_limit",
        "Maximum boost-control pressure limit",
        0x4DFB4,
        1,
        7,
        None,
        temperature,
        "kPa absolute",
        "Temperature-dependent ceiling in the desired absolute pressure path; not a gauge boost target",
        "Lookup normally; r13-4FC5 nonzero substitutes first cell. ROM 04DF6E=0 disables constant override",
        [
            "0F10D4..0F1258",
            "0F0F60..0F0FB8",
            "0EF7B0..0EF7C0",
            "0D4AA0..0D4AB4",
            "141478..141480",
        ],
        [
            "Pressure path is corroborated; exact physical sensor/connector is not independently verified",
            "No safe boost increase follows from a table definition",
        ],
    )
    for ident, name, address, output in (
        (
            "indicated_peak_torque",
            "Indicated peak engine torque",
            0x534A4,
            "r14-31C0 -> r14-319C",
        ),
        (
            "maximum_brake_torque",
            "Maximum brake torque limit",
            0x535A4,
            "r14-6DA4 -> r14-31B4",
        ),
    ):
        add(
            ident,
            name,
            address,
            4,
            16,
            blend,
            rpm,
            "Nm",
            (
                "Indicated torque (brake plus friction), reference KtECPR_M_IndicatedPeakTorq"
                if ident == "indicated_peak_torque"
                else "Platform brake-torque ceiling, reference KtECPR_M_MaxBrkTorqLmt"
            ),
            f"Normal row input is zero for ROM 0531A8=0. {output}; correction factor is 1 with ROM 053408=0",
            [
                "128F44..128FD8",
                "125CE4..125DA4",
                "1278D8..1278EC",
                "129000..129028",
                "12895C..128974",
                "0E896C..0E8988",
                "1400E4..140104",
                "20B614..20B650",
            ],
            [
                "Reference supplies indicated/brake role names; Nm is corroborated by the own CAN boundary",
                "Row zero is the selected software branch, not a measured fuel ethanol content",
                "Other state-dependent torque constraints remain downstream",
            ],
        )
    add(
        "overboost_torque",
        "Overboost indicated torque limit",
        0x53464,
        1,
        16,
        None,
        rpm,
        "Nm",
        "Temporary indicated-torque allowance; reference KtECPR_M_Overboost",
        "Lookup -> r14-6DA8 -> r14-31A4; ROM 053408=0 selects correction factor 1",
        ["128ED8..128F10", "129000..129028", "128930..128974"],
        [
            "Complete overboost timer, thermal and eligibility state machine is not defined here",
            "Existence of the curve does not establish that overboost is currently permitted",
        ],
    )
    for index, (label, address) in enumerate(
        (("A", 0x46944), ("B", 0x46DC8), ("C", 0x4724C), ("fallback", 0x476D0))
    ):
        add(
            "pedal_" + label.lower(),
            "Pedal progression " + label,
            address,
            17,
            17,
            pedal,
            speed,
            "kW",
            "Driver power request before modifiers, limits and power-to-torque conversion",
            (
                f"Selector {index}; normal A/B/C network request, with retained state and source/target transition"
                if index < 3
                else "Forced by r13-5BBF nonzero or dispatcher fallthrough; same mode-1 power path"
            ),
            [
                "11B6A8..11B728",
                "11FD3C..11FF2C",
                "11B8CC..11BCE8",
                "11C87C..11C934",
                "11B648..11B680",
                "11C6A4..11C754",
                "0E896C..0E8988",
            ],
            [
                "kW supported by own RPM conversion 30000/(pi*max(RPM,300)), torque boundary and reference power semantics",
                "Static mode 0465B4=1; mode-2 disabled by 04AF8C=0",
                "A/B/C are identical; only A was requested/reported in both saved drives",
                "All fallback triggers and live internal modifier states remain unobserved",
            ],
        )
    power = axis(
        data,
        0x47B54,
        17,
        "Raw pedal-map power",
        "kW",
        "result of selected pedal lookup",
        "Same mode-1 power path; reference capability-scalar row explicitly describes power",
    )
    baro = axis(
        data,
        0x47D50,
        5,
        "Barometric pressure",
        "kPa absolute",
        "r14-5940",
        "Own x256 conversion -> r14-5934 -> CAN 4C1 byte1; source scale 0.5 kPa",
    )
    add(
        "pedal_barometric_multiplier",
        "Pedal power barometric multiplier",
        0x46664,
        17,
        5,
        power,
        baro,
        "ratio",
        "Dimensionless multiplier for source and target pedal power",
        "Applied by 11B8CC when r13-5BBF=0; all 85 stored values are 32768 -> 1.0",
        ["11FA24..11FAB4", "1A7730..1A78B8", "11BA08..11BA2C", "11BAD0..11BAF4"],
        [
            "Barometric input established on this caller; reference supports alternate capability-based configurations"
        ],
        code="H",
        factor=1 / 32768,
        group="supporting",
    )
    low_pedal = axis(
        data,
        0x47B98,
        9,
        "Pedal request",
        "%",
        "r14-3924; baseline branch uses zero",
        "Caller 11C568..11C574 supplies pedal; baseline caller 11C678..11C684 supplies zero",
    )
    low_rpm = axis(
        data,
        0x47CE8,
        9,
        "Filtered engine speed",
        "rpm",
        "r14-3890",
        "ROM 0465B6=1 selects RPM rather than vehicle speed in 11BD94 / 11C19C",
    )
    add(
        "pedal_low_speed_multiplier",
        "Low-speed pedal power modifier (reference name)",
        0x4675C,
        9,
        9,
        low_pedal,
        low_rpm,
        "ratio",
        "Dimensionless multiplier on the conditioned power request and baseline",
        "ROM 0465B7=0 allows conditioning when fallback clear. Apply if r14-3870 > ROM0465E8=-1 km/h; column RPM selected by 0465B6=1",
        [
            "11C45C..11C690",
            "11BD94..11BE2C",
            "11C19C..11C238",
            "120058..120074",
            "120168..120174",
            "11FB54..11FBDC",
            "1A78BC..1A7A44",
        ],
        [
            "Surrounding state conditioning is not reproduced by isolated table interpolation",
            "Reference name does not imply an independently inferred vehicle-speed threshold",
        ],
        code="h",
        factor=1 / 32,
        group="supporting",
    )
    constants = []
    for address, fmt, name in (
        (0x465B4, "B", "driver mode"),
        (0x465B5, "B", "pedal column selector"),
        (0x465B6, "B", "modifier column selector"),
        (0x465B7, "B", "conditioning bypass"),
        (0x4AF8C, "B", "alternate mode enable"),
        (0x4D7B6, "B", "network progression selector"),
        (0x531A8, "B", "torque row source enable"),
        (0x531A9, "B", "alternate row source flag"),
        (0x53408, "B", "torque environmental correction enable"),
        (0x4DF6E, "B", "pressure override enable"),
        (0x46520, "f", "pedal transition cap multiplier"),
        (0x465D0, "f", "power conversion RPM floor"),
        (0x465E8, "f", "modifier speed gate"),
    ):
        constants.append(
            dict(
                address=f"0x{address:06X}",
                name=name,
                storage=fmt,
                value=struct.unpack_from(">" + fmt, data, address)[0],
            )
        )
    return dict(
        schema="e78-static-table-definitions-v1",
        source_sha256=SHA,
        source_bytes=len(data),
        os=12644082,
        engine_calibration=55581437,
        date="2026-11-09",
        host="tapro; native Kali ARM64",
        ram_bases={"r13": "0x40008000", "r14": "0x40017000"},
        scope="Eight previously mapped primary tables plus two supporting pedal multipliers; not every ECU table",
        status="Static definitions with field-specific qualifications; no live tune validation",
        editable=False,
        flash_ready=False,
        constants=constants,
        tables=tables,
        supersedes="Physical-unit/selected-branch placeholders in astra-pair-comparison-20261109/research-definitions.json",
        remaining=[
            "Physical sensor identification and independent signal validation",
            "Complete fallback, transition and overboost state-machine semantics",
            "Fuel, spark, airflow and exhaust/crackle strategy tables are outside this set",
            "Hardware programming/recovery validation is separate",
        ],
        original_status="Current installed calibration; factory-stock provenance is not established",
    )


def export(output, source=SOURCE):
    data = source.read_bytes()
    report = build(data)
    report["export_provenance"] = dict(
        source_exporter_sha256=SOURCE_EXPORTER_SHA,
        source_exporter_path="cruze-e78-research/tools/export_table_definitions.py",
        adaptation="Portable private baseline; source build(data) definitions and exact-image guard preserved",
        public_references=[
            "docs/discoveries.md",
            "docs/evidence-ledger.json#D09",
            "docs/evidence-ledger.json#D10",
            "docs/evidence-ledger.json#D11",
        ],
        private_traces_bundled=False,
        limitation="Public references summarize privately retained traces; traces/reference CSVs are not shipped or reverified by this export",
    )
    report["primary_guidance"] = (
        "https://www.hptuners.eu/help/vcm_editor_tuning_how_to_advanced_e78.htm"
    )
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    (output / "definitions.json").write_text(
        json.dumps(report, indent=2, allow_nan=False) + "\n"
    )
    with (output / "table-cells.csv").open("x", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "table",
                "cell_address",
                "row_index",
                "column_index",
                "row_value",
                "row_unit",
                "column_value",
                "column_unit",
                "raw_value",
                "value",
                "unit",
            ]
        )
        for t in report["tables"]:
            for i, value in enumerate(t["values"]):
                row, col = divmod(i, t["shape"][1])
                ra, ca = t["row_axis"], t["column_axis"]
                writer.writerow(
                    [
                        t["id"],
                        f"0x{int(t['address'],16)+i*struct.calcsize(t['struct_code']):06X}",
                        row,
                        col,
                        ra["values"][row] if ra else "",
                        ra["unit"] if ra else "",
                        ca["values"][col],
                        ca["unit"],
                        t["raw_values"][i],
                        value,
                        t["unit"],
                    ]
                )
    manifest = {
        p.name: sha(p.read_bytes()) for p in sorted(output.iterdir()) if p.is_file()
    }
    (output / "SHA256SUMS").write_text(
        "".join(f"{digest}  {name}\n" for name, digest in manifest.items())
    )
    print(
        json.dumps(
            dict(
                tables=len(report["tables"]),
                primary=8,
                supporting=2,
                cells=sum(t["count"] for t in report["tables"]),
                output=str(output),
                source_unchanged=sha(source.read_bytes()) == SHA,
            )
        )
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="Fresh output directory")
    parser.add_argument(
        "--source",
        type=Path,
        default=SOURCE,
        help="Private exact-image baseline; default E78_PRIVATE_BASELINE or private/original.bin",
    )
    args = parser.parse_args()
    export(args.output, args.source)
