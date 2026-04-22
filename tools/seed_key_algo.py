#!/usr/bin/env python3
"""
PCMHammer-compatible seed-to-key algorithm implementation.

Implements all 1280 algorithms (5 tables x 256) from UniversalPatcher's KeyAlgorithm.cs.
Each algorithm is a 13-byte instruction sequence that performs operations on a 16-bit seed
to produce a 16-bit key.

Algorithm indices (decimal, flat index into bytearray0):
  E38 ECM:  402  (Table1, offset 146)
  T87 TCM:  569  (Table2, offset 57)
  T42 TCM:  371  (Table1, offset 115)
  E92 ECM:  513  (Table2, offset 1)

Usage:
  python seed_key_algo.py                   # Run verification tests
  python seed_key_algo.py 402 0x2784        # Compute key for algo 402, seed 0x2784
  python seed_key_algo.py --find 0x2784 0x817E  # Find which algo produces this key from this seed
"""

import sys
import re
import os


def _parse_algorithms(cs_path):
    """Parse all algorithm byte arrays from KeyAlgorithm.cs."""
    with open(cs_path, 'r') as f:
        content = f.read()

    pattern = r'new byte\[\]\{([^}]+)\}'
    matches = re.findall(pattern, content)

    algos = []
    for m in matches:
        vals = m.split(',')
        try:
            byte_vals = []
            for v in vals:
                v = v.strip()
                if v.startswith('0x') or v.startswith('0X'):
                    byte_vals.append(int(v, 16))
                else:
                    byte_vals.append(int(v))
            if len(byte_vals) == 13:
                algos.append(byte_vals)
        except ValueError:
            pass
    return algos


# Embedded algorithm data for the 4 known modules (so script works standalone).
# Full table is loaded from KeyAlgorithm.cs when available.
KNOWN_ALGOS = {
    # E38 ECM - algo 402 (Table1, offset 146)
    402: [0x61, 0x7E, 0x7D, 0x58, 0x2A, 0xB8, 0x70, 0x75, 0x01, 0x80, 0x05, 0x2F, 0x98],
    # T42 TCM - algo 371 (Table1, offset 115)
    371: [0xB5, 0x75, 0x7A, 0x95, 0x4C, 0x04, 0x26, 0x7E, 0x21, 0x4C, 0x2A, 0x8E, 0x04],
    # T87 TCM - algo 569 (Table2, offset 57)
    569: [0xC2, 0x6B, 0xD0, 0x04, 0x14, 0x04, 0x4E, 0x2A, 0x41, 0xE0, 0x98, 0x01, 0x08],
    # E92 ECM - algo 513 (Table2, offset 1)
    513: [0xFE, 0x7E, 0xA4, 0x42, 0x6B, 0xFD, 0x04, 0x75, 0x37, 0xFF, 0x2A, 0xFE, 0x40],
}


def compute_key(seed, algo_data):
    """
    Compute the 16-bit key from a 16-bit seed using the given 13-byte algorithm.

    Operation codes (byte at positions 1, 4, 7, 10 of the 13-byte array):
      0x05 - ROL8: swap high/low bytes of key
      0x14 - ADD:  key += (hi<<8 | lo)
      0x2A - COMP: complement key (one's or two's based on hi>=lo)
      0x37 - SWAP_ARG_ADD: key += (lo<<8 | hi)
      0x4C - ROT_LT: rotate key left by hi bits
      0x52 - SWAP_ARG_OR: key |= (lo<<8 | hi)
      0x6B - ROT_RT: rotate key right by lo bits
      0x75 - SWAP_ARG_ADD: key += (lo<<8 | hi)  (same as 0x37)
      0x7E - SWAP_ADD: swap key bytes, then add (hi<<8|lo) or (lo<<8|hi)
      0x98 - SUB:  key -= (hi<<8 | lo)
      0xF8 - SWAP_ARG_SUB: key -= (lo<<8 | hi)
    """
    key = seed & 0xFFFF

    def op_rol8(hb, lb):
        nonlocal key
        key = ((key << 8) & 0xFF00) | ((key >> 8) & 0x00FF)

    def op_add(hb, lb):
        nonlocal key
        key = (key + ((hb << 8) | lb)) & 0xFFFF

    def op_comp(hb, lb):
        nonlocal key
        if hb >= lb:
            key = (~key) & 0xFFFF
        else:
            key = ((~key) + 1) & 0xFFFF

    def op_rot_lt(hb, lb):
        nonlocal key
        key = ((key << hb) | (key >> (16 - hb))) & 0xFFFF

    def op_rot_rt(hb, lb):
        nonlocal key
        key = ((key >> lb) | (key << (16 - lb))) & 0xFFFF

    def op_sub(hb, lb):
        nonlocal key
        key = (key - ((hb << 8) | lb)) & 0xFFFF

    def op_swap_add(hb, lb):
        nonlocal key
        swapped = ((key & 0xFF00) >> 8) | ((key & 0xFF) << 8)
        swapped &= 0xFFFF
        if hb >= lb:
            arg = (hb << 8) | lb
        else:
            arg = (lb << 8) | hb
        key = (swapped + arg) & 0xFFFF

    def op_swap_arg_or(hb, lb):
        nonlocal key
        key = (key | ((lb << 8) | hb)) & 0xFFFF

    def op_swap_arg_add(hb, lb):
        nonlocal key
        key = (key + ((lb << 8) | hb)) & 0xFFFF

    def op_swap_arg_sub(hb, lb):
        nonlocal key
        key = (key - ((lb << 8) | hb)) & 0xFFFF

    opcodes = {
        0x05: op_rol8,
        0x14: op_add,
        0x2A: op_comp,
        0x37: op_swap_arg_add,
        0x4C: op_rot_lt,
        0x52: op_swap_arg_or,
        0x6B: op_rot_rt,
        0x75: op_swap_arg_add,
        0x7E: op_swap_add,
        0x98: op_sub,
        0xF8: op_swap_arg_sub,
    }

    # Process 4 operations at byte positions 1, 4, 7, 10
    byte1 = 1
    done = True
    while True:
        if not done:
            break
        opcode = algo_data[byte1]
        if opcode in opcodes:
            opcodes[opcode](algo_data[byte1 + 1], algo_data[byte1 + 2])
        if byte1 >= 10:
            done = False
        else:
            byte1 += 3

    return key


def get_key(algo_index, seed, algo_table=None):
    """
    Get the key for a given algorithm index and seed.
    algo_index: decimal index (0-1023 for standard range, up to 1279 with Table4)
    seed: 16-bit seed value
    algo_table: list of 13-byte arrays (loaded from CS file or KNOWN_ALGOS)
    """
    if seed == 0xFFFF:
        return 0xFFFF

    if algo_table and algo_index < len(algo_table):
        return compute_key(seed, algo_table[algo_index])
    elif algo_index in KNOWN_ALGOS:
        return compute_key(seed, KNOWN_ALGOS[algo_index])
    else:
        raise ValueError(f"Algorithm index {algo_index} not available. "
                         f"Load full table from KeyAlgorithm.cs or use a known index.")


def find_algo(seed, expected_key, algo_table):
    """Find which algorithm index produces the expected key from the given seed."""
    matches = []
    for idx in range(len(algo_table)):
        try:
            result = compute_key(seed, algo_table[idx])
            if result == expected_key:
                table_num = idx // 256
                table_offset = idx % 256
                matches.append((idx, table_num, table_offset))
        except Exception:
            pass
    return matches


def load_full_table():
    """Try to load the full algorithm table from the CS file."""
    # Try several paths
    candidates = [
        r"C:/Users/Owner/Downloads/UniversalPatcher-master/UniversalPatcher-master/Source/DataLogger/KeyAlgorithm.cs",
    ]
    for path in candidates:
        if os.path.exists(path):
            return _parse_algorithms(path)
    return None


def main():
    if len(sys.argv) == 1:
        # Run verification tests
        print("=== Seed-to-Key Algorithm Verification ===\n")
        tests = [
            ("E38 ECM", 402, 0x2784, 0x817E),
            ("T87 TCM", 569, 0x09E9, 0x6A0C),
            ("T42 TCM", 371, 0x791A, 0x72F5),
            ("E92 ECM", 513, 0x8B3A, 0x22DC),
        ]
        all_pass = True
        for name, algo, seed, expected in tests:
            result = get_key(algo, seed)
            status = "PASS" if result == expected else "FAIL"
            if result != expected:
                all_pass = False
            print(f"  {name}: algo={algo}, seed=0x{seed:04X} -> key=0x{result:04X} "
                  f"(expected 0x{expected:04X}) [{status}]")

        print(f"\n{'All tests passed!' if all_pass else 'SOME TESTS FAILED!'}")
        print(f"\nAlgorithm index reference (decimal):")
        print(f"  E38 ECM:  402  (Table1, offset 146)")
        print(f"  T87 TCM:  569  (Table2, offset 57)")
        print(f"  T42 TCM:  371  (Table1, offset 115)")
        print(f"  E92 ECM:  513  (Table2, offset 1)")

    elif sys.argv[1] == '--find':
        # Find mode: seed key
        seed = int(sys.argv[2], 0)
        expected = int(sys.argv[3], 0)
        table = load_full_table()
        if not table:
            print("ERROR: Could not load KeyAlgorithm.cs")
            sys.exit(1)
        matches = find_algo(seed, expected, table)
        if matches:
            print(f"Seed=0x{seed:04X}, Key=0x{expected:04X} matches:")
            for idx, tbl, off in matches:
                print(f"  Algo {idx} (0x{idx:04X}) = Table{tbl}, offset {off} (0x{off:02X})")
        else:
            print(f"No algorithm found for seed=0x{seed:04X} -> key=0x{expected:04X}")

    else:
        # Compute mode: algo seed
        algo = int(sys.argv[1], 0)
        seed = int(sys.argv[2], 0)
        table = load_full_table()
        result = get_key(algo, seed, table)
        print(f"Algo {algo}, Seed 0x{seed:04X} -> Key 0x{result:04X}")


if __name__ == '__main__':
    main()
