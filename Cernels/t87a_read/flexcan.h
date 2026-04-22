/*
 * flexcan.h — Minimal FlexCAN_A register map.
 *
 * Documented in the Freescale MPC5674F Reference Manual Chapter 30
 * (and the near-identical peripheral across the wider MPC5500 / SPC56x
 * family). Only the fields this kernel actually touches are defined.
 */

#ifndef FLEXCAN_H
#define FLEXCAN_H

#include <stdint.h>

#define FLEXCAN_A_BASE      0xFFFC0000U

/* Core registers (offsets from BASE). */
#define FCAN_MCR_OFF        0x0000  /* Module Configuration */
#define FCAN_CTRL_OFF       0x0004  /* Control */
#define FCAN_TIMER_OFF      0x0008
#define FCAN_RXGMASK_OFF    0x0010
#define FCAN_ESR_OFF        0x0020  /* Error / Status */
#define FCAN_IFLAG1_OFF     0x0030  /* Interrupt flags 0..31 */
#define FCAN_IMASK1_OFF     0x0028

/* MB area: 64 message buffers, 16 bytes each, starting at offset 0x80. */
#define FCAN_MB_BASE_OFF    0x0080
#define FCAN_MB_STRIDE      0x10

/* Per-MB register offsets (from MB base). */
#define MB_CS_OFF           0x00    /* Control+Status: CODE, SRR, IDE, RTR, LENGTH, TIMESTAMP */
#define MB_ID_OFF           0x04    /* 32-bit ID word */
#define MB_D03_OFF          0x08    /* Data bytes 0..3 */
#define MB_D47_OFF          0x0C    /* Data bytes 4..7 */

/* MCR bits. */
#define MCR_MDIS            (1u << 31)
#define MCR_FRZ             (1u << 30)
#define MCR_HALT            (1u << 28)
#define MCR_NOTRDY          (1u << 27)
#define MCR_SOFTRST         (1u << 25)
#define MCR_FRZACK          (1u << 24)
#define MCR_SUPV            (1u << 23)
#define MCR_WRNEN           (1u << 21)
#define MCR_LPMACK          (1u << 20)
#define MCR_SRXDIS          (1u << 17)
#define MCR_BCC             (1u << 16)
#define MCR_MAXMB_MASK      0x3F

/* CS CODE field values (bits 24..27 of CS word). */
#define CS_CODE_EMPTY       0x4u    /* RX empty */
#define CS_CODE_FULL        0x2u    /* RX received data */
#define CS_CODE_TX_DATA     0xCu    /* TX ready */
#define CS_CODE_TX_INACTIVE 0x8u

/* Pointer helpers. */
static inline volatile uint32_t *fcan_reg(unsigned offset)
{
    return (volatile uint32_t *)(FLEXCAN_A_BASE + offset);
}

static inline volatile uint32_t *fcan_mb(unsigned mb, unsigned field)
{
    return (volatile uint32_t *)(FLEXCAN_A_BASE + FCAN_MB_BASE_OFF
                                 + mb * FCAN_MB_STRIDE + field);
}

#endif /* FLEXCAN_H */
