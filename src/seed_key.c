/*
 * seed_key.c — GM seed-key bytecode interpreter
 *
 * Implements the PCMHammer/UniversalPatcher KeyAlgorithm engine.
 * Supports opcodes: add, sub, complement, rotate left/right, rol8,
 * swap_add, swap_arg_add, swap_arg_or, swap_arg_sub.
 */

#include <string.h>
#include "seed_key.h"

/* T87 TCM default: algo index 569 */
static uint8_t g_algo[13] = {
    0xC2, 0x6B, 0xD0, 0x04,
         0x14, 0x04, 0x4E,
         0x2A, 0x41, 0xE0,
         0x98, 0x01, 0x08
};

void seedkey_set_algo(const uint8_t *bytecodes)
{
    memcpy(g_algo, bytecodes, 13);
}

uint16_t seedkey_compute(uint16_t seed)
{
    uint16_t key = seed;
    uint8_t pos = 1;
    int count = 0;

    while (count < 4 && pos + 2 < 13) {
        uint8_t op   = g_algo[pos];
        uint8_t high = g_algo[pos + 1];
        uint8_t low  = g_algo[pos + 2];
        uint16_t val;

        switch (op) {
        case 5:   /* rol8 — swap bytes */
            key = (uint16_t)(((key << 8) & 0xFF00) | ((key >> 8) & 0x00FF));
            break;

        case 20:  /* 0x14 — add */
            val = (uint16_t)((high << 8) | low);
            key = (uint16_t)(key + val);
            break;

        case 42:  /* 0x2A — complement */
            if (high >= low)
                key = (uint16_t)(~key);
            else
                key = (uint16_t)((~key) + 1);
            break;

        case 55:  /* 0x37 — swap_arg_add (same as 0x75) */
        case 117: /* 0x75 — swap_arg_add */
            val = (uint16_t)((low << 8) | high);
            key = (uint16_t)(key + val);
            break;

        case 76:  /* 0x4C — rotate left */
            key = (uint16_t)((key << high) | (key >> (16 - high)));
            break;

        case 82:  /* 0x52 — swap_arg_or */
            val = (uint16_t)((low << 8) | high);
            key = (uint16_t)(key | val);
            break;

        case 107: /* 0x6B — rotate right */
            key = (uint16_t)((key >> low) | (key << (16 - low)));
            break;

        case 126: /* 0x7E — swap_add (byte-swap key, then add) */
        {
            uint16_t hi_byte = (key >> 8) & 0xFF;
            uint16_t lo_byte = (key & 0xFF) << 8;
            uint16_t swapped = hi_byte | lo_byte;
            if (high >= low)
                val = (uint16_t)((high << 8) | low);
            else
                val = (uint16_t)((low << 8) | high);
            key = (uint16_t)(swapped + val);
            break;
        }

        case 152: /* 0x98 — subtract */
            val = (uint16_t)((high << 8) | low);
            key = (uint16_t)(key - val);
            break;

        case 248: /* 0xF8 — swap_arg_sub */
            val = (uint16_t)((low << 8) | high);
            key = (uint16_t)(key - val);
            break;

        default:
            break; /* unknown opcode, skip */
        }

        pos += 3;
        count++;
    }

    return key;
}
