/*
 * can_driver.cpp — Thin C++ wrapper around Adafruit CANSAME5x
 *
 * Exposes a plain-C API (extern "C") so that isotp.c and uds.c
 * can call these functions without needing C++ linkage.
 *
 * Uses interrupt-driven RX with a 4096-element ring buffer to
 * prevent frame loss during SD writes and serial output.
 */

#include <CANSAME5x.h>
#include "can_driver.h"

static CANSAME5x CAN;

/* ---- Interrupt-driven RX ring buffer ---- */

#define CAN_RING_SIZE 4096  /* 4096 entries = ~52KB, ~1s at 500kbps */

struct can_ring_entry {
    uint32_t id;
    uint8_t  data[8];
    uint8_t  len;
};

static volatile can_ring_entry can_ring[CAN_RING_SIZE];
static volatile uint32_t ring_head = 0;     /* ISR writes here (uint32 avoids wrap issues) */
static volatile uint32_t ring_tail = 0;     /* app reads here */
static volatile uint32_t ring_overflow = 0; /* ring buffer overflow counter */
static volatile uint32_t hw_overflow = 0;   /* hardware FIFO overflow counter */
static volatile uint32_t total_rx_frames = 0; /* total frames received by ISR */

/* CAN RX interrupt callback — called by CANSAME5x on RF0N interrupt.
 * Runs at hardware interrupt priority — cannot be blocked by SD/Serial. */
static void can_rx_isr(int packetSize)
{
    if (packetSize <= 0 || packetSize > 8) return;

    total_rx_frames++;

    uint16_t next = (ring_head + 1) % CAN_RING_SIZE;
    if (next == ring_tail) {
        /* Ring full — drop frame */
        ring_overflow++;
        /* Still need to read the frame out of hardware FIFO to clear it */
        for (int i = 0; i < packetSize; i++) CAN.read();
        return;
    }

    volatile can_ring_entry *e = &can_ring[ring_head];
    e->id  = (uint32_t)CAN.packetId();
    e->len = (uint8_t)packetSize;
    for (int i = 0; i < packetSize; i++) {
        e->data[i] = (uint8_t)CAN.read();
    }
    ring_head = next;
    __DMB();  /* memory barrier — ensure ring_head visible before ISR returns */
}

extern "C" {

int can_init(uint32_t baud_rate)
{
    /* Wake the CAN transceiver */
    pinMode(PIN_CAN_STANDBY, OUTPUT);
    digitalWrite(PIN_CAN_STANDBY, false);   /* out of standby */

    /* Enable 5 V boost for transceiver */
    pinMode(PIN_CAN_BOOSTEN, OUTPUT);
    digitalWrite(PIN_CAN_BOOSTEN, true);

    if (!CAN.begin(baud_rate)) {
        return -1;
    }

    /* Reset ring buffer and counters */
    ring_head = 0;
    ring_tail = 0;
    ring_overflow = 0;
    hw_overflow = 0;
    total_rx_frames = 0;

    /* Enable interrupt-driven reception */
    CAN.onReceive(can_rx_isr);

    /* Fix 3: Set CAN ISR to highest priority (0) so USB/SPI
     * can't preempt it during sustained frame reception. */
    NVIC_SetPriority(CAN0_IRQn, 0);

    return 0;
}

int can_send(uint32_t id, const uint8_t *data, uint8_t len)
{
    CAN.beginPacket(id);
    for (uint8_t i = 0; i < len; i++) {
        CAN.write(data[i]);
    }
    if (!CAN.endPacket()) {
        return -1;
    }
    return 0;
}

int can_receive(uint32_t *id, uint8_t *data, uint8_t *len)
{
    /* Read from ring buffer (filled by ISR) */
    if (ring_tail == ring_head) {
        return -1;  /* empty */
    }

    volatile can_ring_entry *e = &can_ring[ring_tail];
    *id  = e->id;
    *len = e->len;
    for (uint8_t i = 0; i < e->len && i < 8; i++) {
        data[i] = e->data[i];
    }
    ring_tail = (ring_tail + 1) % CAN_RING_SIZE;
    __DMB();  /* memory barrier — ensure ring_tail visible to ISR */
    return 0;
}

void can_set_filter(uint32_t id, uint32_t mask)
{
    CAN.filter((int)id, (int)mask);
}

uint32_t can_get_overflow_count(void)
{
    return ring_overflow;
}

void can_reset_overflow(void)
{
    ring_overflow = 0;
    hw_overflow = 0;
    total_rx_frames = 0;
}

uint32_t can_get_hw_overflow(void)
{
    return hw_overflow;
}

uint32_t can_get_total_rx(void)
{
    return total_rx_frames;
}

void can_set_loopback(int enable)
{
    /* SAME51 MCAN: loopback mode makes the controller ACK its own frames.
     * Frames still appear on the physical CAN bus — external nodes see them.
     * This prevents bus-off when transmitting with no other node present. */
    CAN.loopback();   /* CANSAME5x sets CCCR.TEST + TEST.LBCK */
    if (!enable) {
        /* To exit loopback, re-init is cleanest. The caller should
         * call can_init() after BAM entry succeeds. */
    }
}

uint32_t can_autobaud(void)
{
    static const uint32_t bauds[] = { 500000, 250000, 1000000, 125000 };
    static const int n_bauds = sizeof(bauds) / sizeof(bauds[0]);

    for (int i = 0; i < n_bauds; i++) {
        /* Full init at this baud — can_init handles transceiver pins,
         * ring buffer reset, ISR setup. Safe to call repeatedly. */
        if (can_init(bauds[i]) != 0) continue;

        /* Listen for up to 1 s per rate. At the correct baud on an active
         * GM bus, periodic frames arrive within ~100 ms. At the wrong baud
         * the controller accumulates errors and goes bus-off — the ISR
         * never fires, we just see silence. 4 rates × 1 s = 4 s max. */
        uint32_t start = millis();
        while (millis() - start < 1000) {
            if (total_rx_frames >= 2) {
                /* Two clean frames = confident match. Ring buffer and
                 * ISR are already set up by can_init — ready to go. */
                return bauds[i];
            }
        }
    }

    /* No baud matched — bus may be silent (ECU off). */
    return 0;
}

} /* extern "C" */
