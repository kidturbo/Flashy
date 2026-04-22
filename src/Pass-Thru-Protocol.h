#ifndef PASS_THRU_PROTOCOL_H
#define PASS_THRU_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

/* ---------- Version ---------- */
#ifndef PASSTHRU_VERSION
#define PASSTHRU_VERSION "1.4.2"
#endif

/* ---------- CAN defaults ---------- */
#define CAN_DEFAULT_BAUD        500000U     /* 500 kbps OBD-II standard */
#define CAN_FALLBACK_BAUD       250000U     /* 250 kbps fallback */

/* Standard OBD-II CAN IDs (11-bit) */
#define CAN_TESTER_ID           0x7E0U
#define CAN_ECU_RESPONSE_ID     0x7E8U

/* ---------- ISO-TP buffer sizes ---------- */
#define ISOTP_SEND_BUF_SIZE     4096U
#define ISOTP_RECV_BUF_SIZE     4096U

/* ---------- UDS timing ---------- */
#define UDS_DEFAULT_TIMEOUT_MS  2000U       /* Standard P2 timeout */
#define UDS_PENDING_TIMEOUT_MS  10000U      /* Extended timeout after 0x78 NRC */
#define TESTER_PRESENT_INTERVAL 2000U       /* TesterPresent interval (ms) */
#define BROADCAST_TP_INTERVAL   1000U       /* 0x0101 bus TP broadcast interval (ms) */
#define CAN_BROADCAST_ID        0x0101U     /* GM bus-wide broadcast CAN ID */

/* ---------- Serial ---------- */
#define SERIAL_BAUD             115200U
#define SERIAL_CMD_MAX_LEN      1024U

/* ---------- Flash write ---------- */
#define WRITE_BLOCK_SIZE        0x800U      /* 2048 bytes per TransferData block */
#define WRITE_MAX_RETRIES       3           /* retries per block before abort */
#define WRITE_ERASE_TIMEOUT_MS  60000U      /* 60s for erase (NRC 0x78 pending) */
#define WRITE_WDATA_TIMEOUT_MS  30000U      /* 30s to receive WDATA line from PC */

#endif /* PASS_THRU_PROTOCOL_H */
