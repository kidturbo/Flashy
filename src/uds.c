/*
 * uds.c — Lightweight UDS (ISO 14229) service layer
 *
 * Sits on top of isotp-c.  All functions are blocking:
 * they send a request via ISO-TP, then poll for the response.
 */

#include <string.h>
#include "uds.h"
#include "isotp.h"
#include "Pass-Thru-Protocol.h"

/* ---------- poll callback ---------- */

static uds_poll_fn g_poll_cb = NULL;

void uds_set_poll_callback(uds_poll_fn fn) { g_poll_cb = fn; }

/* ---------- internal helpers ---------- */

static int uds_wait_response(IsoTpLink *link, uint8_t *buf, uint16_t buf_size,
                             uint16_t *out_len, uint32_t timeout_ms)
{
    uint32_t start = isotp_user_get_ms();

    while ((isotp_user_get_ms() - start) < timeout_ms) {
        /* Pump CAN RX into ISO-TP so we actually see responses */
        if (g_poll_cb) g_poll_cb();

        isotp_poll(link);

        int ret = isotp_receive(link, buf, buf_size, out_len);
        if (ret == ISOTP_RET_OK && *out_len > 0) {
            return UDS_OK;
        }
    }
    return UDS_ERR_TIMEOUT;
}

static void uds_parse_response(const uint8_t *raw, uint16_t raw_len,
                                uds_msg_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    if (raw_len == 0) return;

    if (raw[0] == UDS_SID_NEGATIVE_RESPONSE && raw_len >= 3) {
        resp->is_negative = true;
        resp->service     = raw[1];
        resp->nrc         = raw[2];
        return;
    }

    /* Positive response: SID = request SID + 0x40 */
    resp->service      = raw[0];
    resp->is_negative  = false;

    if (raw_len > 1) {
        resp->sub_function = raw[1];
    }
    if (raw_len > 2) {
        resp->data_len = (uint16_t)(raw_len - 2);
        if (resp->data_len > sizeof(resp->data)) {
            resp->data_len = sizeof(resp->data);
        }
        memcpy(resp->data, &raw[2], resp->data_len);
    }
}

/* ---------- core request/response ---------- */

int uds_request(IsoTpLink *link, const uint8_t *req, uint16_t req_len,
                uds_msg_t *resp, uint32_t timeout_ms)
{
    int ret = isotp_send(link, req, req_len);
    if (ret != ISOTP_RET_OK) {
        return UDS_ERR_SEND;
    }

    uint8_t  expected_pos = req[0] + 0x40;  /* positive response SID */
    uint8_t  raw[4128];
    uint16_t raw_len = 0;

    /* Loop to handle NRC 0x78 (responsePending) and stale broadcasts */
    for (;;) {
        ret = uds_wait_response(link, raw, sizeof(raw), &raw_len, timeout_ms);
        if (ret != UDS_OK) {
            return ret;
        }

        /* Check for responsePending */
        if (raw_len >= 3 &&
            raw[0] == UDS_SID_NEGATIVE_RESPONSE &&
            raw[2] == UDS_NRC_RESPONSE_PENDING) {
            /* ECU is busy — wait again with at least UDS_PENDING_TIMEOUT_MS */
            if (timeout_ms < UDS_PENDING_TIMEOUT_MS)
                timeout_ms = UDS_PENDING_TIMEOUT_MS;
            continue;
        }

        /* Check for negative response to our request */
        if (raw[0] == UDS_SID_NEGATIVE_RESPONSE && raw_len >= 2 &&
            raw[1] == req[0]) {
            break;  /* NRC for our SID — process it */
        }

        /* Check for expected positive response SID */
        if (raw[0] == expected_pos) {
            break;  /* Got our response */
        }

        /* Unexpected SID (broadcast, stale message) — discard and retry */
        continue;
    }

    if (resp) {
        uds_parse_response(raw, raw_len, resp);
        if (resp->is_negative) {
            return UDS_ERR_NEGATIVE;
        }
    }
    return UDS_OK;
}

/* ---------- convenience wrappers ---------- */

int uds_diagnostic_session(IsoTpLink *link, uint8_t session_type, uds_msg_t *resp)
{
    uint8_t req[2] = { UDS_SID_DIAG_SESSION_CTRL, session_type };
    return uds_request(link, req, sizeof(req), resp, UDS_DEFAULT_TIMEOUT_MS);
}

int uds_tester_present(IsoTpLink *link)
{
    uint8_t req[2] = { UDS_SID_TESTER_PRESENT, 0x00 };  /* sub = 0 → no response suppression */
    uds_msg_t resp;
    return uds_request(link, req, sizeof(req), &resp, UDS_DEFAULT_TIMEOUT_MS);
}

int uds_security_access_seed(IsoTpLink *link, uint8_t level,
                             uint8_t *seed, uint16_t *seed_len)
{
    uint8_t req[2] = { UDS_SID_SECURITY_ACCESS, level };  /* odd level = request seed */
    uds_msg_t resp;
    int ret = uds_request(link, req, sizeof(req), &resp, UDS_DEFAULT_TIMEOUT_MS);
    if (ret != UDS_OK) return ret;

    if (seed && seed_len) {
        *seed_len = resp.data_len;
        memcpy(seed, resp.data, resp.data_len);
    }
    return UDS_OK;
}

int uds_security_access_key(IsoTpLink *link, uint8_t level,
                            const uint8_t *key, uint16_t key_len)
{
    uint8_t req[258];
    req[0] = UDS_SID_SECURITY_ACCESS;
    req[1] = level;  /* even level = send key */
    if (key_len > sizeof(req) - 2) return UDS_ERR_OVERFLOW;
    memcpy(&req[2], key, key_len);

    uds_msg_t resp;
    return uds_request(link, req, (uint16_t)(2 + key_len), &resp, UDS_DEFAULT_TIMEOUT_MS);
}

int uds_ecu_reset(IsoTpLink *link, uint8_t reset_type, uds_msg_t *resp)
{
    uint8_t req[2] = { UDS_SID_ECU_RESET, reset_type };
    return uds_request(link, req, sizeof(req), resp, UDS_DEFAULT_TIMEOUT_MS);
}

int uds_routine_control(IsoTpLink *link, uint8_t ctrl_type, uint16_t routine_id,
                        const uint8_t *data, uint16_t data_len, uds_msg_t *resp)
{
    uint8_t req[4100];
    req[0] = UDS_SID_ROUTINE_CTRL;
    req[1] = ctrl_type;
    req[2] = (uint8_t)(routine_id >> 8);
    req[3] = (uint8_t)(routine_id & 0xFF);
    if (data && data_len > 0) {
        if (data_len > sizeof(req) - 4) return UDS_ERR_OVERFLOW;
        memcpy(&req[4], data, data_len);
    }
    return uds_request(link, req, (uint16_t)(4 + data_len), resp,
                       UDS_PENDING_TIMEOUT_MS);  /* erase can take a while */
}

int uds_routine_control_ex(IsoTpLink *link, uint8_t ctrl_type, uint16_t routine_id,
                           const uint8_t *data, uint16_t data_len, uds_msg_t *resp,
                           uint32_t timeout_ms)
{
    uint8_t req[4100];
    req[0] = UDS_SID_ROUTINE_CTRL;
    req[1] = ctrl_type;
    req[2] = (uint8_t)(routine_id >> 8);
    req[3] = (uint8_t)(routine_id & 0xFF);
    if (data && data_len > 0) {
        if (data_len > sizeof(req) - 4) return UDS_ERR_OVERFLOW;
        memcpy(&req[4], data, data_len);
    }
    return uds_request(link, req, (uint16_t)(4 + data_len), resp, timeout_ms);
}

int uds_request_download(IsoTpLink *link, uint32_t addr, uint32_t size,
                         uint16_t *max_block_len)
{
    /*  SID | dataFormatIdentifier | addressAndLengthFormatIdentifier | addr(4) | size(4) */
    uint8_t req[11];
    req[0]  = UDS_SID_REQUEST_DOWNLOAD;
    req[1]  = 0x00;  /* no compression, no encryption */
    req[2]  = 0x44;  /* 4-byte address, 4-byte length */
    req[3]  = (uint8_t)(addr >> 24);
    req[4]  = (uint8_t)(addr >> 16);
    req[5]  = (uint8_t)(addr >> 8);
    req[6]  = (uint8_t)(addr);
    req[7]  = (uint8_t)(size >> 24);
    req[8]  = (uint8_t)(size >> 16);
    req[9]  = (uint8_t)(size >> 8);
    req[10] = (uint8_t)(size);

    uds_msg_t resp;
    int ret = uds_request(link, req, sizeof(req), &resp, UDS_DEFAULT_TIMEOUT_MS);
    if (ret != UDS_OK) return ret;

    /* Parse maxNumberOfBlockLength from response */
    if (max_block_len && resp.data_len > 0) {
        uint8_t len_size = resp.sub_function >> 4;  /* high nibble = length of maxBlockLen */
        if (len_size == 0) len_size = resp.data_len; /* fallback */
        uint16_t blk = 0;
        for (uint8_t i = 0; i < len_size && i < resp.data_len; i++) {
            blk = (uint16_t)((blk << 8) | resp.data[i]);
        }
        *max_block_len = blk;
    }
    return UDS_OK;
}

int uds_request_download_ex(IsoTpLink *link, uint32_t addr, uint32_t size,
                            uint16_t *max_block_len, uint32_t timeout_ms)
{
    uint8_t req[11];
    req[0]  = UDS_SID_REQUEST_DOWNLOAD;
    req[1]  = 0x00;
    req[2]  = 0x44;
    req[3]  = (uint8_t)(addr >> 24);
    req[4]  = (uint8_t)(addr >> 16);
    req[5]  = (uint8_t)(addr >> 8);
    req[6]  = (uint8_t)(addr);
    req[7]  = (uint8_t)(size >> 24);
    req[8]  = (uint8_t)(size >> 16);
    req[9]  = (uint8_t)(size >> 8);
    req[10] = (uint8_t)(size);

    uds_msg_t resp;
    int ret = uds_request(link, req, sizeof(req), &resp, timeout_ms);
    if (ret != UDS_OK) return ret;

    if (max_block_len && resp.data_len > 0) {
        uint8_t len_size = resp.sub_function >> 4;
        if (len_size == 0) len_size = resp.data_len;
        uint16_t blk = 0;
        for (uint8_t i = 0; i < len_size && i < resp.data_len; i++) {
            blk = (uint16_t)((blk << 8) | resp.data[i]);
        }
        *max_block_len = blk;
    }
    return UDS_OK;
}

int uds_request_upload(IsoTpLink *link, uint32_t addr, uint32_t size,
                       uint16_t *max_block_len)
{
    uint8_t req[11];
    req[0]  = UDS_SID_REQUEST_UPLOAD;
    req[1]  = 0x00;
    req[2]  = 0x44;
    req[3]  = (uint8_t)(addr >> 24);
    req[4]  = (uint8_t)(addr >> 16);
    req[5]  = (uint8_t)(addr >> 8);
    req[6]  = (uint8_t)(addr);
    req[7]  = (uint8_t)(size >> 24);
    req[8]  = (uint8_t)(size >> 16);
    req[9]  = (uint8_t)(size >> 8);
    req[10] = (uint8_t)(size);

    uds_msg_t resp;
    int ret = uds_request(link, req, sizeof(req), &resp, UDS_DEFAULT_TIMEOUT_MS);
    if (ret != UDS_OK) return ret;

    if (max_block_len && resp.data_len > 0) {
        uint8_t len_size = resp.sub_function >> 4;
        if (len_size == 0) len_size = resp.data_len; /* fallback */
        uint16_t blk = 0;
        for (uint8_t i = 0; i < len_size && i < resp.data_len; i++) {
            blk = (uint16_t)((blk << 8) | resp.data[i]);
        }
        *max_block_len = blk;
    }
    return UDS_OK;
}

int uds_transfer_data(IsoTpLink *link, uint8_t block_seq,
                      const uint8_t *data, uint16_t data_len, uds_msg_t *resp)
{
    uint8_t req[4098];
    req[0] = UDS_SID_TRANSFER_DATA;
    req[1] = block_seq;
    if (data && data_len > 0) {
        if (data_len > sizeof(req) - 2) return UDS_ERR_OVERFLOW;
        memcpy(&req[2], data, data_len);
    }
    return uds_request(link, req, (uint16_t)(2 + data_len), resp,
                       UDS_PENDING_TIMEOUT_MS);
}

int uds_transfer_data_ex(IsoTpLink *link, uint8_t block_seq,
                         const uint8_t *data, uint16_t data_len, uds_msg_t *resp,
                         uint32_t timeout_ms)
{
    uint8_t req[4098];
    req[0] = UDS_SID_TRANSFER_DATA;
    req[1] = block_seq;
    if (data && data_len > 0) {
        if (data_len > sizeof(req) - 2) return UDS_ERR_OVERFLOW;
        memcpy(&req[2], data, data_len);
    }
    return uds_request(link, req, (uint16_t)(2 + data_len), resp, timeout_ms);
}

int uds_transfer_exit(IsoTpLink *link, uds_msg_t *resp)
{
    uint8_t req[1] = { UDS_SID_TRANSFER_EXIT };
    return uds_request(link, req, sizeof(req), resp, UDS_DEFAULT_TIMEOUT_MS);
}

/* ---------- GM-specific functions ---------- */

int uds_gm_request_upload(IsoTpLink *link, uint32_t addr, uds_msg_t *resp)
{
    /* GM format: 35 01 [addr_hi] [addr_mid] [addr_lo] — 5 bytes only */
    uint8_t req[5];
    req[0] = UDS_SID_REQUEST_UPLOAD;
    req[1] = 0x01;
    req[2] = (uint8_t)(addr >> 16);
    req[3] = (uint8_t)(addr >> 8);
    req[4] = (uint8_t)(addr);
    return uds_request(link, req, sizeof(req), resp, UDS_DEFAULT_TIMEOUT_MS);
}

int uds_e38_request_upload(IsoTpLink *link, uint32_t addr, uds_msg_t *resp)
{
    /* E38 format: 35 00 08 [addr:4] — 7 bytes, 4-byte addressing */
    uint8_t req[7];
    req[0] = UDS_SID_REQUEST_UPLOAD;
    req[1] = 0x00;
    req[2] = 0x08;   /* addressAndLengthFormat: 0 bytes size, 8 nibbles (4 bytes) addr */
    req[3] = (uint8_t)(addr >> 24);
    req[4] = (uint8_t)(addr >> 16);
    req[5] = (uint8_t)(addr >> 8);
    req[6] = (uint8_t)(addr);
    return uds_request(link, req, sizeof(req), resp, UDS_DEFAULT_TIMEOUT_MS);
}

int uds_receive(IsoTpLink *link, uds_msg_t *resp, uint32_t timeout_ms)
{
    uint8_t  raw[4128];
    uint16_t raw_len = 0;

    int ret = uds_wait_response(link, raw, sizeof(raw), &raw_len, timeout_ms);
    if (ret != UDS_OK) return ret;

    if (resp) {
        uds_parse_response(raw, raw_len, resp);
        if (resp->is_negative) return UDS_ERR_NEGATIVE;
    }
    return UDS_OK;
}
