#ifndef CAN_DRIVER_H
#define CAN_DRIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize CAN peripheral + transceiver pins. Returns 0 on success.
 * Enables interrupt-driven RX with a 4096-element ring buffer.
 * Sets CAN ISR to highest priority (0). */
int can_init(uint32_t baud_rate);

/* Send a raw CAN frame. Returns 0 on success. */
int can_send(uint32_t id, const uint8_t *data, uint8_t len);

/* Non-blocking poll for a received CAN frame from the ring buffer.
 * Returns 0 and populates id/data/len if a frame is available.
 * Returns -1 if no frame available. */
int can_receive(uint32_t *id, uint8_t *data, uint8_t *len);

/* Set hardware CAN RX filter. Only frames matching (id & mask) are received.
 * Pass id=0, mask=0 to accept all frames (default). */
void can_set_filter(uint32_t id, uint32_t mask);

/* Enable/disable internal loopback (self-ACK). Frames still go on the
 * physical bus wire, but the controller ACKs its own frames so it won't
 * go bus-off when no other node is present. Essential for BAM entry
 * where the TCM is powered off during initial heartbeat flood. */
void can_set_loopback(int enable);

/* Auto-detect CAN baud rate by listening at each common rate.
 * Tries 500k, 250k, 1M, 125k in order, 300 ms each.
 * Returns detected baud (e.g. 500000) or 0 if no bus activity. */
uint32_t can_autobaud(void);

/* Ring buffer overflow count (frames dropped by ISR when ring was full). */
uint32_t can_get_overflow_count(void);

/* Hardware FIFO overflow count (frames dropped by CAN peripheral). */
uint32_t can_get_hw_overflow(void);

/* Total frames received by ISR since last reset. */
uint32_t can_get_total_rx(void);

/* Reset all overflow and frame counters. */
void can_reset_overflow(void);

#ifdef __cplusplus
}
#endif

#endif /* CAN_DRIVER_H */
