/*
 * main.c — Flashy clean-room read kernel for GM E92 ECM
 *          (Freescale MPC5674F, PowerPC e200z7 Book E, 4 MB flash).
 *
 * Minimal SRAM-resident kernel that answers raw-CAN read commands:
 *   CMD_PING  (0x01)                    -> PONG + copyright string
 *   CMD_READ  (0xA0)  addr[4] len[2]    -> stream <len> bytes from <addr>
 *                                          back in 8-byte CAN frames.
 *
 * Upload expectation: loaded at 0x40001000 in SRAM by the stock GM
 * bootloader via UDS $36 downloadAndExecute, then immediately executed.
 *
 * CAN setup (inherited from boot — we assume FlexCAN_A is already
 * initialised at 500 kbps by the stock bootloader, since the host
 * just talked to us on that bus to upload this code). We only
 * reconfigure MB filters for our own use.
 *
 * This is a clean-room implementation. It shares no code with any
 * commercial flash tool. It does share the *public* FlexCAN register
 * map from the Freescale MPC5674F Reference Manual, and the *public*
 * UDS transport the stock bootloader uses to reach us. Neither is a
 * copyrightable expression — they are protocol / hardware specs.
 */

#include <stdint.h>
#include "flexcan.h"

/* ==========================================================================
 * Copyright / signature block.
 * Embedded verbatim so kernel extractors can identify our build.
 * ========================================================================== */

__attribute__((used, section(".rodata")))
static const char k_signature[] =
    "(c)2026 FLASHY E92__v0.1C - Rolling Smoke Kernel by Claude + kidturbo. MIT.";

/* ==========================================================================
 * CAN IDs.
 * Tester sends to us on 0x7E0, we reply on 0x7E8 (standard GM HS-CAN).
 * These are the public OBD-II J2534 convention — no IP claim.
 * ========================================================================== */

#define TESTER_ID   0x7E0u
#define ECU_ID      0x7E8u

#define RX_MB       0u      /* Message buffer index for receive */
#define TX_MB       8u      /* Message buffer index for transmit */

/* ==========================================================================
 * Command bytes (our own wire protocol).
 * ========================================================================== */

#define CMD_PING    0x01u
#define CMD_READ    0xA0u
#define CMD_1A      0x1Au    /* legacy GM readDataByIdentifier, stock tool-compatible */
#define CMD_11      0x11u    /* UDS ECUReset — we trigger a software reset */
#define CMD_23      0x23u    /* UDS ReadMemoryByAddress — multi-frame ISO-TP */
#define RESP_ACK    0x41u
#define RESP_NAK    0x4Eu

/* ==========================================================================
 * Low-level CAN helpers.
 * ========================================================================== */

/* Park every MB in INACTIVE and clear IFLAG. The bootloader may have
 * left unrelated MBs configured as stale TX/RX; leaving them alive
 * lets them transmit garbage on the bus or race with our MBs. This
 * mirrors stock tool's init loop — it clears all 64 MBs on kernel boot. */
static void mb_reset_all(void)
{
    for (unsigned i = 0; i < 64u; i++) {
        *fcan_mb(i, MB_CS_OFF) = 0u;
    }
    *fcan_reg(FCAN_IFLAG1_OFF) = 0xFFFFFFFFu;   /* clear all IFLAGs */
}

/* Install a standard-ID filter on RX_MB so we only accept TESTER_ID.
 * FlexCAN requires the MB to pass through INACTIVE before reconfig. */
static void rx_mb_arm(void)
{
    *fcan_mb(RX_MB, MB_CS_OFF) = 0u;                            /* INACTIVE */
    *fcan_mb(RX_MB, MB_ID_OFF) = (uint32_t)(TESTER_ID << 18);   /* 11-bit ID */
    *fcan_mb(RX_MB, MB_CS_OFF) = (uint32_t)(CS_CODE_EMPTY << 24);  /* ready */
    *fcan_reg(FCAN_IFLAG1_OFF) = (1u << RX_MB);
}

/* Block until a CAN frame arrives in RX_MB; copy into dst, return DLC. */
static uint8_t can_rx(uint8_t dst[8])
{
    /* Spin until IFLAG1 bit for RX_MB is set. */
    while ((*fcan_reg(FCAN_IFLAG1_OFF) & (1u << RX_MB)) == 0u) {
        /* no-op — host holds us here until it sends. */
    }

    uint32_t cs   = *fcan_mb(RX_MB, MB_CS_OFF);
    uint32_t d03  = *fcan_mb(RX_MB, MB_D03_OFF);
    uint32_t d47  = *fcan_mb(RX_MB, MB_D47_OFF);
    (void)*fcan_mb(RX_MB, MB_ID_OFF);       /* unlock MB */
    (void)*fcan_reg(FCAN_TIMER_OFF);        /* release lock */

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

    /* Clear IFLAG and rearm for next RX. */
    *fcan_reg(FCAN_IFLAG1_OFF) = (1u << RX_MB);
    rx_mb_arm();
    return dlc;
}

/* Send one 8-byte CAN frame on ECU_ID. Bounded spin on TX complete so a
 * mis-configured FlexCAN (stuck freeze, wrong mode, etc.) can't wedge us. */
static void can_tx(const uint8_t src[8], uint8_t dlc)
{
    /* Park MB in inactive while we load it. */
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

    /* Wait for transmission complete — bounded. At 500 kbps, one 8-byte
     * CAN frame is ~230 us on the wire; a generous cap covers any
     * reasonable bus-off/retry. Beyond that, give up and move on. */
    for (uint32_t n = 0; n < 0x00100000u; n++) {
        if (*fcan_reg(FCAN_IFLAG1_OFF) & (1u << TX_MB)) break;
    }
    *fcan_reg(FCAN_IFLAG1_OFF) = (1u << TX_MB);
}

/* If the bootloader left FlexCAN in freeze or halt mode, our TX never
 * completes. Try to bring it back to normal operation. Safe no-op if
 * already in normal mode. */
static void can_leave_freeze(void)
{
    volatile uint32_t *mcr = fcan_reg(FCAN_MCR_OFF);
    uint32_t m = *mcr;

    /* Clear HALT and MDIS bits; FRZ we leave alone so the debugger
     * can still freeze in debug mode if attached. */
    m &= ~(MCR_HALT | MCR_MDIS);
    *mcr = m;

    /* Spin-wait for FRZACK to clear (module acknowledges NOT frozen)
     * and NOTRDY to clear (module ready for bus). Bounded. */
    for (uint32_t n = 0; n < 0x00010000u; n++) {
        uint32_t cur = *mcr;
        if ((cur & (MCR_FRZACK | MCR_NOTRDY)) == 0u) break;
    }
}

/* ==========================================================================
 * Command handlers.
 * ========================================================================== */

/* All kernel replies are emitted as ISO-TP Single Frames: byte 0 is the
 * PCI (0x0N, N = payload length 1..7), then the UDS/legacy payload. */

/* Multi-frame ISO-TP transmitter. Sends a UDS positive response built as:
 *   [service] [addr:4] [data:len]
 *
 * Handles the three-state ISO-TP FC protocol (CTS / WAIT / ABORT), respects
 * the host's requested BS (block size) and a crude STmin (separation time)
 * in whole milliseconds. STmin values 0xF1..0xF9 (microsecond encoding) are
 * treated as "go as fast as possible" since we don't have a fine-grained
 * timer wired up in this kernel.
 */
static void send_response(uint8_t svc, uint32_t addr,
                          const volatile uint8_t *data, uint16_t len)
{
    uint16_t total = (uint16_t)(5u + len);   /* svc + addr[4] + data */

    /* Single-frame path */
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

    /* First frame: PCI (2 bytes) + svc + addr[4] + 1 data byte */
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

    uint16_t sent = 1u;    /* one data byte rode along in the FF */

    /* Wait for the initial Flow Control frame from the tester. */
    uint8_t bs    = 0u;
    uint8_t stmin = 0u;
    for (;;) {
        uint8_t fc[8];
        uint8_t dlc = can_rx(fc);
        if (dlc < 3u) continue;
        if ((fc[0] & 0xF0u) != 0x30u) continue;        /* not a FC */
        uint8_t fs = (uint8_t)(fc[0] & 0x0Fu);
        if (fs == 0u) { bs = fc[1]; stmin = fc[2]; break; }   /* CTS */
        if (fs == 1u) continue;                                /* WAIT */
        return;                                                /* ABORT */
    }

    /* Stream consecutive frames. */
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

        /* Crude STmin: values 1..127 are milliseconds; 0xF1..0xF9 mean
         * hundreds of microseconds (we ignore — too fine for this spin
         * loop). Each 1 ms ~= ~130k e200z7 cycles at default clock; we
         * just use a soft delay proportional to the request. */
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

static void handle_ping(void)
{
    /* payload "41 F L S H Y" = 6 bytes, so PCI = 0x06. */
    uint8_t reply[8] = { 0x06u, RESP_ACK, 'F', 'L', 'S', 'H', 'Y', 0x00 };
    can_tx(reply, 8);
}

/* Legacy GM $1A readDataByIdentifier — stock tool's kernel answers $1A BB with
 * a positive response "5A BB <data>". We answer with "5A BB FLSHY" so the
 * host can unambiguously tell our kernel is alive vs. a stock bootloader. */
static void handle_1a(const uint8_t req[8])
{
    uint8_t pid = req[1];
    /* payload "5A BB F L S H Y" = 7 bytes, PCI = 0x07. */
    uint8_t reply[8] = { 0x07u, 0x5Au, pid, 'F', 'L', 'S', 'H', 'Y' };
    can_tx(reply, 8);
}

/* UDS $23 ReadMemoryByAddress. Request payload (after ISO-TP SF unwrap):
 *   [23] [A3 A2 A1 A0] [L1 L0]
 * Response:
 *   [63] [A3 A2 A1 A0] [data:L bytes]
 * Streamed as an ISO-TP multi-frame message if L > 2.
 *
 * L is clipped to 4090 so that 5-byte header + L stays inside the 4095-byte
 * ISO-TP message limit. The host is expected to split longer reads.
 */
static void handle_23_read(const uint8_t req[8])
{
    uint32_t addr = ((uint32_t)req[1] << 24) | ((uint32_t)req[2] << 16)
                  | ((uint32_t)req[3] <<  8) | ((uint32_t)req[4] <<  0);
    uint16_t len  = (uint16_t)(((uint16_t)req[5] << 8) | req[6]);

    if (len > 4090u) len = 4090u;

    send_response(0x63u, addr, (const volatile uint8_t *)addr, len);
}

/* Legacy memory read: source address and length packed into the request.
 *
 * Request frame  : [CMD_READ] [A3 A2 A1 A0] [L1 L0] [pad]
 * Response frames: [chunk of up to 8 bytes] ...
 *   We stream (len) bytes back, 8 per frame, with a final short frame.
 *
 * Reading is harmless: no privileged writes, no flash unlock. It just
 * fetches whatever the bus master asked for and echoes it back.
 */
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
    /* If the bootloader parked FlexCAN in freeze/halt, bring it back. */
    can_leave_freeze();

    /* Wipe every MB before we set up our own, so leftover TX/RX
     * buffers from the bootloader can't race with us. */
    mb_reset_all();

    /* Arm our RX filter. We stay silent until polled — matches stock tool's
     * kernel boot pattern observed on real E92 capture. The stock bootloader
     * doesn't ACK $36 80 either, so any unsolicited frame here would be a
     * clue we executed; but we skip it to stay bus-friendly and match the
     * reference handshake exactly. */
    rx_mb_arm();

    for (;;) {
        uint8_t msg[8];
        uint8_t dlc = can_rx(msg);
        if (dlc == 0) continue;

        /* ISO-TP Single Frame unwrap: if high nibble of byte 0 is 0 and
         * low nibble is a plausible payload length (1..7), strip the PCI
         * byte and look at byte 1 as the service. This lets the host
         * talk to us over standard ISO-TP (matching stock tool's framing),
         * while the raw custom protocol still works for code that sends
         * service byte directly at offset 0. */
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
            /* UDS ECUReset — send positive response then trigger a
             * software reset via the MPC5674F's SIU SRCR register.
             * This returns the ECU to its stock bootloader/app. */
            uint8_t ack[8] = { 0x02u, 0x51u, payload[1], 0, 0, 0, 0, 0 };
            can_tx(ack, 8);
            /* Brief delay to let the ACK frame finish transmitting. */
            for (volatile uint32_t i = 0; i < 50000u; i++) { }
            /* SIU System Reset Control Register — writing SSR bit
             * triggers a software system reset on MPC5674F. */
            *(volatile uint32_t *)0xC3F90010u = 0x80000000u;
            /* Should not reach here. */
            for (;;) { }
        }
        default:
            /* Silently discard anything we don't handle. Critical: the
             * host's ISO-TP layer sends Flow Control frames (0x30) on
             * the same CAN ID between multi-frame exchanges. NAKing
             * those corrupts the host's ISO-TP state and eventually
             * causes a desync where $23 responses are misrouted. */
            break;
        }
    }
}
