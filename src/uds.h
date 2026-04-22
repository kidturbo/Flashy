#ifndef UDS_H
#define UDS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration — defined in isotp.h */
typedef struct IsoTpLink IsoTpLink;

/* ---- UDS Service IDs (ISO 14229) ---- */
#define UDS_SID_DIAG_SESSION_CTRL   0x10
#define UDS_SID_ECU_RESET           0x11
#define UDS_SID_CLEAR_DTC           0x14
#define UDS_SID_READ_DTC            0x19
#define UDS_SID_READ_BY_ID          0x22
#define UDS_SID_SECURITY_ACCESS     0x27
#define UDS_SID_COMM_CTRL           0x28
#define UDS_SID_WRITE_BY_ID         0x2E
#define UDS_SID_ROUTINE_CTRL        0x31
#define UDS_SID_REQUEST_DOWNLOAD    0x34
#define UDS_SID_REQUEST_UPLOAD      0x35
#define UDS_SID_TRANSFER_DATA       0x36
#define UDS_SID_TRANSFER_EXIT       0x37
#define UDS_SID_TESTER_PRESENT      0x3E
#define UDS_SID_NEGATIVE_RESPONSE   0x7F

/* ---- Diagnostic Session Types ---- */
#define UDS_SESSION_DEFAULT         0x01
#define UDS_SESSION_PROGRAMMING     0x02
#define UDS_SESSION_EXTENDED        0x03

/* ---- ECU Reset Types ---- */
#define UDS_RESET_HARD              0x01
#define UDS_RESET_KEY_OFF_ON        0x02
#define UDS_RESET_SOFT              0x03

/* ---- Routine Control Sub-Functions ---- */
#define UDS_ROUTINE_START           0x01
#define UDS_ROUTINE_STOP            0x02
#define UDS_ROUTINE_REQUEST_RESULT  0x03

/* ---- Negative Response Codes ---- */
#define UDS_NRC_GENERAL_REJECT              0x10
#define UDS_NRC_SERVICE_NOT_SUPPORTED       0x11
#define UDS_NRC_SUBFUNCTION_NOT_SUPPORTED   0x12
#define UDS_NRC_INCORRECT_MSG_LENGTH        0x13
#define UDS_NRC_RESPONSE_TOO_LONG           0x14
#define UDS_NRC_CONDITIONS_NOT_CORRECT      0x22
#define UDS_NRC_REQUEST_OUT_OF_RANGE        0x31
#define UDS_NRC_SECURITY_ACCESS_DENIED      0x33
#define UDS_NRC_INVALID_KEY                 0x35
#define UDS_NRC_EXCEEDED_ATTEMPTS           0x36
#define UDS_NRC_UPLOAD_DOWNLOAD_NOT_ACCEPT  0x70
#define UDS_NRC_GENERAL_PROGRAMMING_FAILURE 0x72
#define UDS_NRC_WRONG_BLOCK_SEQ_COUNTER     0x73
#define UDS_NRC_RESPONSE_PENDING            0x78

/* ---- Return codes ---- */
#define UDS_OK              0
#define UDS_ERR_TIMEOUT    -1
#define UDS_ERR_NEGATIVE   -2
#define UDS_ERR_SEND       -3
#define UDS_ERR_OVERFLOW   -4

/* ---- UDS response container ---- */
typedef struct {
    uint8_t     service;        /* Response SID (request SID + 0x40) */
    uint8_t     sub_function;
    uint8_t     data[4096];     /* Response payload (after SID+sub) */
    uint16_t    data_len;
    bool        is_negative;
    uint8_t     nrc;            /* Negative Response Code if is_negative */
} uds_msg_t;

/* ---- Poll callback ---- */

/** Called by uds_wait_response each iteration to pump CAN RX → ISO-TP. */
typedef void (*uds_poll_fn)(void);
void uds_set_poll_callback(uds_poll_fn fn);

/* ---- API ---- */

/**
 * Send a raw UDS request and wait for the response.
 * Handles NRC 0x78 (responsePending) automatically.
 */
int uds_request(IsoTpLink *link, const uint8_t *req, uint16_t req_len,
                uds_msg_t *resp, uint32_t timeout_ms);

/* Convenience wrappers */
int uds_diagnostic_session(IsoTpLink *link, uint8_t session_type, uds_msg_t *resp);
int uds_tester_present(IsoTpLink *link);
int uds_security_access_seed(IsoTpLink *link, uint8_t level,
                             uint8_t *seed, uint16_t *seed_len);
int uds_security_access_key(IsoTpLink *link, uint8_t level,
                            const uint8_t *key, uint16_t key_len);
int uds_ecu_reset(IsoTpLink *link, uint8_t reset_type, uds_msg_t *resp);
int uds_routine_control(IsoTpLink *link, uint8_t ctrl_type, uint16_t routine_id,
                        const uint8_t *data, uint16_t data_len, uds_msg_t *resp);
int uds_routine_control_ex(IsoTpLink *link, uint8_t ctrl_type, uint16_t routine_id,
                           const uint8_t *data, uint16_t data_len, uds_msg_t *resp,
                           uint32_t timeout_ms);
int uds_request_download(IsoTpLink *link, uint32_t addr, uint32_t size,
                         uint16_t *max_block_len);
int uds_request_download_ex(IsoTpLink *link, uint32_t addr, uint32_t size,
                            uint16_t *max_block_len, uint32_t timeout_ms);
int uds_request_upload(IsoTpLink *link, uint32_t addr, uint32_t size,
                       uint16_t *max_block_len);
int uds_transfer_data(IsoTpLink *link, uint8_t block_seq,
                      const uint8_t *data, uint16_t data_len, uds_msg_t *resp);
int uds_transfer_data_ex(IsoTpLink *link, uint8_t block_seq,
                         const uint8_t *data, uint16_t data_len, uds_msg_t *resp,
                         uint32_t timeout_ms);
int uds_transfer_exit(IsoTpLink *link, uds_msg_t *resp);

/* GM T87: RequestUpload with format 35 01 [addr:3] — 5 bytes, 3-byte addressing */
int uds_gm_request_upload(IsoTpLink *link, uint32_t addr, uds_msg_t *resp);

/* GM E38: RequestUpload with format 35 00 08 [addr:4] — 7 bytes, 4-byte addressing */
int uds_e38_request_upload(IsoTpLink *link, uint32_t addr, uds_msg_t *resp);

/* Receive an unsolicited message (no send — just wait for incoming ISO-TP) */
int uds_receive(IsoTpLink *link, uds_msg_t *resp, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* UDS_H */
