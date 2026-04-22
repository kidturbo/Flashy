/*
 * main.c — Flashy clean-room read kernel for GM T87A TCM
 *          (ST SPC564A80, PowerPC e200z4, 4 MB flash, VLE).
 *
 * Minimal SRAM-resident kernel that answers CAN read commands:
 *   CMD_PING  (0x01)                    -> PONG + copyright string
 *   CMD_1A    (0x1A)  [pid]             -> legacy GM readDataByIdentifier
 *   CMD_23    (0x23)  addr[4] len[2]    -> UDS ReadMemoryByAddress (ISO-TP)
 *   CMD_11    (0x11)  [type]            -> ECUReset (SIU software reset)
 *   CMD_READ  (0xA0)  addr[4] len[2]    -> raw-bus stream (legacy)
 *
 * Upload: loaded at 0x40010000 in SRAM by the stock GM bootloader
 * via UDS $36 downloadAndExecute, then immediately executed.
 *
 * CAN setup: inherited from bootloader. We reconfigure MB filters only.
 *
 * Clean-room implementation. No third-party kernel code referenced.
 * Public FlexCAN register map from ST/Freescale reference manuals.
 */

#include <stdint.h>
#include "flexcan.h"

/* ==========================================================================
 * Copyright / signature block.
 * ========================================================================== */

__attribute__((used, section(".rodata")))
static const char k_signature[] =
    "(c)2026 FLASHY T87A_v0.1 - Rolling Smoke Kernel by Claude + kidturbo. MIT.";

/* ==========================================================================
 * CAN IDs — T87A TCM uses 0x7E2 / 0x7EA (GM HS-CAN TCM convention).
 * ========================================================================== */

#define TESTER_ID   0x7E2u
#define ECU_ID      0x7EAu

#define RX_MB       0u
#define TX_MB       8u

/* ==========================================================================
 * Command bytes.
 * ========================================================================== */

#define CMD_PING    0x01u
#define CMD_READ    0xA0u
#define CMD_1A      0x1Au
#define CMD_11      0x11u
#define CMD_23      0x23u
#define RESP_ACK    0x41u

/* ==========================================================================
 * Low-level CAN helpers (identical to E92 kernel).
 * ========================================================================== */

static void mb_reset_all(void)
{
    for (unsigned i = 0; i < 64u; i++) {
        *fcan_mb(i, MB_CS_OFF) = 0u;
    }
    *fcan_reg(FCAN_IFLAG1_OFF) = 0xFFFFFFFFu;
}

static void rx_mb_arm(void)
{
    *fcan_mb(RX_MB, MB_CS_OFF) = 0u;
    *fcan_mb(RX_MB, MB_ID_OFF) = (uint32_t)(TESTER_ID << 18);
    *fcan_mb(RX_MB, MB_CS_OFF) = (uint32_t)(CS_CODE_EMPTY << 24);
    *fcan_reg(FCAN_IFLAG1_OFF) = (1u << RX_MB);
}

static uint8_t can_rx(uint8_t dst[8])
{
    while ((*fcan_reg(FCAN_IFLAG1_OFF) & (1u << RX_MB)) == 0u) { }

    uint32_t cs   = *fcan_mb(RX_MB, MB_CS_OFF);
    uint32_t d03  = *fcan_mb(RX_MB, MB_D03_OFF);
    uint32_t d47  = *fcan_mb(RX_MB, MB_D47_OFF);
    (void)*fcan_mb(RX_MB, MB_ID_OFF);
    (void)*fcan_reg(FCAN_TIMER_OFF);

    uint8_t dlc = (uint8_t)((cs >> 16) & 0xFu);
    if (dlc > 8u) dlc = 8u;

    dst[0] = (uint8_t)(d03 >> 24);
    dst[1] = (uint8_t)(d03 >> 16);
    dst[2] = (uint8_t)(d03 >>  8);
    dst[3] = (uint8_t)(d03 >>  0);
    dst[4] = (uint8_t)(d47 >> 24);
    dst[5] = (uint8_t)(d47 >> 16);
    dst[6] = (uint8_t)(d47 >>  8);
    dst[7] = (uint8_t)(d47 >>  0);

    *fcan_reg(FCAN_IFLAG1_OFF) = (1u << RX_MB);
    rx_mb_arm();
    return dlc;
}

static void can_tx(const uint8_t src[8], uint8_t dlc)
{
    *fcan_mb(TX_MB, MB_CS_OFF) = (uint32_t)(CS_CODE_TX_INACTIVE << 24);
    *fcan_mb(TX_MB, MB_ID_OFF) = (uint32_t)(ECU_ID << 18);

    uint32_t d03 = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16)
                 | ((uint32_t)src[2] <<  8) | ((uint32_t)src[3] <<  0);
    uint32_t d47 = ((uint32_t)src[4] << 24) | ((uint32_t)src[5] << 16)
                 | ((uint32_t)src[6] <<  8) | ((uint32_t)src[7] <<  0);
    *fcan_mb(TX_MB, MB_D03_OFF) = d03;
    *fcan_mb(TX_MB, MB_D47_OFF) = d47;

    if (dlc > 8u) dlc = 8u;
    uint32_t cs = (uint32_t)(CS_CODE_TX_DATA << 24) | ((uint32_t)dlc << 16);
    *fcan_mb(TX_MB, MB_CS_OFF) = cs;

    for (uint32_t n = 0; n < 0x00100000u; n++) {
        if (*fcan_reg(FCAN_IFLAG1_OFF) & (1u << TX_MB)) break;
    }
    *fcan_reg(FCAN_IFLAG1_OFF) = (1u << TX_MB);
}

static void can_leave_freeze(void)
{
    volatile uint32_t *mcr = fcan_reg(FCAN_MCR_OFF);
    uint32_t m = *mcr;
    m &= ~(MCR_HALT | MCR_MDIS);
    *mcr = m;
    for (uint32_t n = 0; n < 0x00010000u; n++) {
        uint32_t cur = *mcr;
        if ((cur & (MCR_FRZACK | MCR_NOTRDY)) == 0u) break;
    }
}

/* ==========================================================================
 * ISO-TP multi-frame transmitter (identical to E92 kernel).
 * ========================================================================== */

static void send_response(uint8_t svc, uint32_t addr,
                          const volatile uint8_t *data, uint16_t len)
{
    uint16_t total = (uint16_t)(5u + len);

    if (total <= 7u) {
        uint8_t sf[8];
        sf[0] = (uint8_t)total;
        sf[1] = svc;
        sf[2] = (uint8_t)(addr >> 24);
        sf[3] = (uint8_t)(addr >> 16);
        sf[4] = (uint8_t)(addr >>  8);
        sf[5] = (uint8_t)(addr >>  0);
        for (uint8_t i = 0; i < len && i < 2u; i++) sf[6 + i] = data[i];
        for (uint8_t i = (uint8_t)(6u + len); i < 8u; i++) sf[i] = 0u;
        can_tx(sf, 8);
        return;
    }

    uint8_t ff[8];
    ff[0] = (uint8_t)(0x10u | ((total >> 8) & 0x0Fu));
    ff[1] = (uint8_t)(total & 0xFFu);
    ff[2] = svc;
    ff[3] = (uint8_t)(addr >> 24);
    ff[4] = (uint8_t)(addr >> 16);
    ff[5] = (uint8_t)(addr >>  8);
    ff[6] = (uint8_t)(addr >>  0);
    ff[7] = data[0];
    can_tx(ff, 8);

    uint16_t sent = 1u;

    uint8_t bs    = 0u;
    uint8_t stmin = 0u;
    for (;;) {
        uint8_t fc[8];
        uint8_t dlc = can_rx(fc);
        if (dlc < 3u) continue;
        if ((fc[0] & 0xF0u) != 0x30u) continue;
        uint8_t fs = (uint8_t)(fc[0] & 0x0Fu);
        if (fs == 0u) { bs = fc[1]; stmin = fc[2]; break; }
        if (fs == 1u) continue;
        return;
    }

    uint8_t sn = 1u;
    uint8_t cf_in_block = 0u;
    while (sent < len) {
        uint8_t cf[8];
        cf[0] = (uint8_t)(0x20u | (sn & 0x0Fu));
        for (uint8_t i = 1; i < 8u; i++) {
            cf[i] = (sent < len) ? data[sent] : 0u;
            if (sent < len) sent++;
        }
        can_tx(cf, 8);
        sn = (uint8_t)((sn + 1u) & 0x0Fu);

        if (stmin > 0u && stmin <= 0x7Fu) {
            for (volatile uint32_t i = 0; i < (uint32_t)stmin * 10000u; i++) {}
        }

        cf_in_block++;
        if (bs > 0u && cf_in_block >= bs && sent < len) {
            cf_in_block = 0u;
            for (;;) {
                uint8_t fc2[8];
                uint8_t dlc = can_rx(fc2);
                if (dlc < 3u) continue;
                if ((fc2[0] & 0xF0u) != 0x30u) continue;
                uint8_t fs = (uint8_t)(fc2[0] & 0x0Fu);
                if (fs == 0u) { bs = fc2[1]; stmin = fc2[2]; break; }
                if (fs == 1u) continue;
                return;
            }
        }
    }
}

/* ==========================================================================
 * Command handlers.
 * ========================================================================== */

static void handle_ping(void)
{
    uint8_t reply[8] = { 0x06u, RESP_ACK, 'F', 'L', 'S', 'H', 'Y', 0x00 };
    can_tx(reply, 8);
}

static void handle_1a(const uint8_t req[8])
{
    uint8_t pid = req[1];
    uint8_t reply[8] = { 0x07u, 0x5Au, pid, 'F', 'L', 'S', 'H', 'Y' };
    can_tx(reply, 8);
}

static void handle_23_read(const uint8_t req[8])
{
    uint32_t addr = ((uint32_t)req[1] << 24) | ((uint32_t)req[2] << 16)
                  | ((uint32_t)req[3] <<  8) | ((uint32_t)req[4] <<  0);
    uint16_t len  = (uint16_t)(((uint16_t)req[5] << 8) | req[6]);
    if (len > 4090u) len = 4090u;
    send_response(0x63u, addr, (const volatile uint8_t *)addr, len);
}

static void handle_read(const uint8_t req[8])
{
    uint32_t addr = ((uint32_t)req[1] << 24) | ((uint32_t)req[2] << 16)
                  | ((uint32_t)req[3] <<  8) | ((uint32_t)req[4] <<  0);
    uint16_t len  = (uint16_t)(((uint16_t)req[5] << 8) | req[6]);

    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    uint8_t frame[8];

    while (len > 0u) {
        uint8_t n = (len >= 8u) ? 8u : (uint8_t)len;
        for (uint8_t i = 0; i < n; i++) frame[i] = p[i];
        for (uint8_t i = n; i < 8u; i++) frame[i] = 0;
        can_tx(frame, n);
        p   += n;
        len -= n;
    }
}

/* ==========================================================================
 * Entry point.
 * ========================================================================== */

void kernel_main(void)
{
    /* T87A/SPC564A80: SKIP aggressive CAN reinit. The bootloader leaves
     * FlexCAN in a working state — mb_reset_all() destroys config that
     * the SPC564A80 bootloader needs. Just reconfigure our two MBs
     * (RX and TX) without touching anything else. */
    /* can_leave_freeze(); — not needed, FlexCAN already running */
    /* mb_reset_all();     — BREAKS SPC564A80, don't clear all MBs */

    /* Clear just IFLAG for our MBs, then arm RX filter */
    *fcan_reg(FCAN_IFLAG1_OFF) = (1u << RX_MB) | (1u << TX_MB);
    rx_mb_arm();

    for (;;) {
        uint8_t msg[8];
        uint8_t dlc = can_rx(msg);
        if (dlc == 0) continue;

        /* ISO-TP Single Frame unwrap */
        uint8_t *payload = msg;
        if ((msg[0] & 0xF0u) == 0x00u && (msg[0] & 0x0Fu) >= 1u
                                      && (msg[0] & 0x0Fu) <= 7u) {
            payload = &msg[1];
        }

        switch (payload[0]) {
        case CMD_PING:
            handle_ping();
            break;
        case CMD_READ:
            handle_read(payload);
            break;
        case CMD_1A:
            handle_1a(payload);
            break;
        case CMD_23:
            handle_23_read(payload);
            break;
        case CMD_11: {
            uint8_t ack[8] = { 0x02u, 0x51u, payload[1], 0, 0, 0, 0, 0 };
            can_tx(ack, 8);
            for (volatile uint32_t i = 0; i < 50000u; i++) { }
            /* SIU System Reset Control Register — SSR bit triggers
             * software reset. Same address across MPC5xxx/SPC56x family. */
            *(volatile uint32_t *)0xC3F90010u = 0x80000000u;
            for (;;) { }
        }
        default:
            break;
        }
    }
}
