/*
 * main.cpp — Arduino entry point for J2534 Pass-Thru (Feather M4 CAN)
 *
 * Serial command interface + ISO-TP/UDS orchestration.
 */

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <SPI.h>
#include <SdFat.h>
#include <Wire.h>
#include "RTClib.h"
#include "Pass-Thru-Protocol.h"
#include "can_driver.h"

extern "C" {
#include "isotp.h"
#include "uds.h"
#include "seed_key.h"
#include "gm5byte_key.h"
}

#include "gm_kernels.h"
#include "t87a_kernel.h"
#include "e67_kernel.h"
#include "kernel_registry.h"

/* === Runtime parameter overrides (Phase 4) ==========================
 * Users can override kernel-metadata defaults via the SET serial command
 * without rebuilding. -1 / 0xFFFF means "use kernel metadata default". */
struct flashy_params_t {
    int32_t boot_delay_ms;     /* -1 = use kern->boot_delay_ms */
    int16_t probe_svc;         /* -1 = use kern->probe_svc; 0 = skip probe */
    int16_t probe_pid;         /* -1 = use kern->probe_pid */
    int8_t  use_txe;           /* -1 = use kern->use_txe */
    int8_t  send_3e_after;     /* -1 = default (send once); 0 = suppress */
    int16_t cadence_3e_ms;     /* 0 = no cadence; else ms between $3E during boot_delay */
};
static flashy_params_t g_params = {-1, -1, -1, -1, -1, 0};

static void cmd_params(void)
{
    Serial.println("PARAMS:");
    Serial.print("  BOOT_DELAY_MS   = ");
    if (g_params.boot_delay_ms < 0) Serial.println("(default, kernel meta)");
    else                            Serial.println(g_params.boot_delay_ms);
    Serial.print("  PROBE_SVC       = ");
    if (g_params.probe_svc < 0) Serial.println("(default, kernel meta)");
    else if (g_params.probe_svc == 0) Serial.println("0 (no probe)");
    else { Serial.print("0x"); Serial.println(g_params.probe_svc, HEX); }
    Serial.print("  PROBE_PID       = ");
    if (g_params.probe_pid < 0) Serial.println("(default, kernel meta)");
    else { Serial.print("0x"); Serial.println(g_params.probe_pid, HEX); }
    Serial.print("  USE_TXE         = ");
    if (g_params.use_txe < 0) Serial.println("(default, kernel meta)");
    else                      Serial.println(g_params.use_txe ? "1" : "0");
    Serial.print("  SEND_3E_AFTER   = ");
    if (g_params.send_3e_after < 0) Serial.println("(default = 1)");
    else                            Serial.println(g_params.send_3e_after ? "1" : "0");
    Serial.print("  CADENCE_3E_MS   = ");
    Serial.println(g_params.cadence_3e_ms);
}

static int32_t _parse_signed_hex_or_int(const char *s)
{
    if (!s || !*s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        return (int32_t)strtol(s, nullptr, 16);
    }
    return (int32_t)strtol(s, nullptr, 10);
}

static void cmd_set(const char *arg)
{
    if (!arg || !*arg) {
        Serial.println("usage: SET <KEY> <VALUE>   (or SET RESET)");
        Serial.println("  KEYs: BOOT_DELAY_MS, PROBE_SVC, PROBE_PID, USE_TXE,");
        Serial.println("        SEND_3E_AFTER, CADENCE_3E_MS");
        Serial.println("  VALUE: integer or 0x-prefixed hex. -1 restores default.");
        Serial.println("  e.g.:  SET BOOT_DELAY_MS 1500");
        Serial.println("         SET PROBE_SVC 0      (skip probe entirely)");
        Serial.println("         SET PROBE_SVC -1     (restore kernel default)");
        return;
    }
    /* Copy-safe parse: key UPCASE, value after first whitespace. */
    char keybuf[24];
    const char *space = strchr(arg, ' ');
    if (!space) {
        if (strcasecmp(arg, "RESET") == 0 || strcasecmp(arg, "CLEAR") == 0) {
            g_params = {-1, -1, -1, -1, -1, 0};
            Serial.println("SET: all params reset to kernel-metadata defaults");
            return;
        }
        Serial.println("SET: missing value");
        return;
    }
    size_t klen = (size_t)(space - arg);
    if (klen >= sizeof(keybuf)) klen = sizeof(keybuf) - 1;
    for (size_t i = 0; i < klen; i++) keybuf[i] = (char)toupper((unsigned char)arg[i]);
    keybuf[klen] = '\0';
    while (*space == ' ') space++;
    int32_t v = _parse_signed_hex_or_int(space);

    if      (strcmp(keybuf, "BOOT_DELAY_MS") == 0) g_params.boot_delay_ms = v;
    else if (strcmp(keybuf, "PROBE_SVC")     == 0) g_params.probe_svc     = (int16_t)v;
    else if (strcmp(keybuf, "PROBE_PID")     == 0) g_params.probe_pid     = (int16_t)v;
    else if (strcmp(keybuf, "USE_TXE")       == 0) g_params.use_txe       = (int8_t)v;
    else if (strcmp(keybuf, "SEND_3E_AFTER") == 0) g_params.send_3e_after = (int8_t)v;
    else if (strcmp(keybuf, "CADENCE_3E_MS") == 0) g_params.cadence_3e_ms = (int16_t)v;
    else {
        Serial.print("SET: unknown key '"); Serial.print(keybuf); Serial.println("'");
        return;
    }
    Serial.print("SET "); Serial.print(keybuf); Serial.print(" = ");
    Serial.println(v);
}

/* Resolve effective params for a T42READ run: overrides win over kernel metadata. */
static uint16_t _eff_boot_delay(const kernel_entry_t *k) {
    return (g_params.boot_delay_ms < 0) ? (uint16_t)(k ? k->boot_delay_ms : 500)
                                        : (uint16_t)g_params.boot_delay_ms;
}
static uint8_t  _eff_probe_svc(const kernel_entry_t *k) {
    return (g_params.probe_svc < 0) ? (uint8_t)(k ? k->probe_svc : 0x1A)
                                    : (uint8_t)g_params.probe_svc;
}
static uint8_t  _eff_probe_pid(const kernel_entry_t *k) {
    return (g_params.probe_pid < 0) ? (uint8_t)(k ? k->probe_pid : 0xBB)
                                    : (uint8_t)g_params.probe_pid;
}
static bool     _eff_send_3e(void) {
    return (g_params.send_3e_after < 0) ? true : (g_params.send_3e_after != 0);
}
/* Clean-room "Kernel" for E92 ECM (MPC5xxx-family, exact variant
 * unconfirmed) — see Kernels/e92_read/.
 * Generated from Kernels/e92_read/kernel.bin by tools/bin2header.py.
 * Private file (gitignored) — regenerate after rebuilding the kernel. */
#if __has_include("kernel_e92.h")
#include "kernel_e92.h"
#define KERNEL_E92_AVAILABLE 1
#else
#define KERNEL_E92_AVAILABLE 0
#endif

/* Clean-room T87A kernel (VLE, SPC564A80). */
#if __has_include("kernel_t87a_private.h")
#include "kernel_t87a_private.h"
#define KERNEL_T87A_AVAILABLE 1
#else
#define KERNEL_T87A_AVAILABLE 0
#endif

/* Rollin Smoke T87A — HS-CAN read kernel, classic PPC, loads at 0x40028000.
 * Drives the $A0 streaming read path for the HSREAD command. First smoke-kernel
 * in the T87A HS-CAN family, naming mirrors kernel_t87a_private.h. */
#if __has_include("kernel_t87a_rollin_smoke_private.h")
#include "kernel_t87a_rollin_smoke_private.h"
#define ROLLIN_SMOKE_T87A_AVAILABLE 1
#else
#define ROLLIN_SMOKE_T87A_AVAILABLE 0
#endif

/* T42 clean-room read kernel (MC68377 CPU32X, loads at 0xFF9000). */
#if __has_include("kernel_t42_private.h")
#include "kernel_t42_private.h"
#define KERNEL_T42_AVAILABLE 1
#else
#define KERNEL_T42_AVAILABLE 0
#endif

#if __has_include("e92_kernel_private.h")
#include "e92_kernel_private.h"
#define E92_KERNEL_AVAILABLE 1
/* $34 RequestDownload parameters + $36 block-sequence from E92 capture */
#define E92_RD_VALUE_0    0x00
#define E92_RD_VALUE_1    0x10
#define E92_RD_VALUE_2    0x00
#define E92_BLOCK_SEQ     0x00   /* two-step: $36 00 upload, then $36 80 execute */
#else
#define E92_KERNEL_AVAILABLE 0
#endif

/* ---------- NeoPixel status LED ---------- */
#define NEOPIXEL_PIN       8
#define NEOPIXEL_COUNT     1
static Adafruit_NeoPixel pixel(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

/* LED color scheme:
 *   OFF      = black        IDLE     = dim blue      CONNECTED = green
 *   READING  = cyan flash   WRITING  = orange flash  ERASING   = red flash (slow)
 *   ERROR    = solid red    AUTH     = yellow         SCAN      = dim white
 *   SCAN_HIT = teal         VIN_WRITE= orange        VIN_OK    = bright green
 *   SUCCESS  = bright green flash
 */

/* Blink state for non-blocking flash */
static uint8_t  g_blink_r = 0, g_blink_g = 0, g_blink_b = 0;
static uint16_t g_blink_interval = 0;   /* 0 = solid (no blink) */
static uint32_t g_blink_last = 0;
static bool     g_blink_on = true;

static void led_set(uint8_t r, uint8_t g, uint8_t b) {
    pixel.setPixelColor(0, r, g, b);
    pixel.show();
    g_blink_interval = 0;  /* solid — stop blinking */
}

static void led_set_blink(uint8_t r, uint8_t g, uint8_t b, uint16_t interval_ms) {
    g_blink_r = r; g_blink_g = g; g_blink_b = b;
    g_blink_interval = interval_ms;
    g_blink_last = millis();
    g_blink_on = true;
    pixel.setPixelColor(0, r, g, b);
    pixel.show();
}

/* Call from loop() to update blink — non-blocking smooth fade */
static void led_update(void) {
    if (g_blink_interval == 0) return;
    uint32_t now = millis();
    /* Full cycle = 2 × interval (fade up + fade down) */
    uint32_t cycle = (uint32_t)g_blink_interval * 2;
    uint32_t phase = (now - g_blink_last) % cycle;
    /* Triangle wave: 0→255→0 over one full cycle */
    uint8_t brightness;
    if (phase < (uint32_t)g_blink_interval)
        brightness = (uint8_t)(phase * 255UL / g_blink_interval);
    else
        brightness = (uint8_t)((cycle - phase) * 255UL / g_blink_interval);
    /* Minimum brightness floor so LED never fully off */
    if (brightness < 15) brightness = 15;
    uint8_t r = (uint8_t)((uint16_t)g_blink_r * brightness / 255);
    uint8_t g = (uint8_t)((uint16_t)g_blink_g * brightness / 255);
    uint8_t b = (uint8_t)((uint16_t)g_blink_b * brightness / 255);
    pixel.setPixelColor(0, r, g, b);
    pixel.show();
}

static void led_off(void)       { led_set(0, 0, 0); }
static void led_idle(void)      { led_set(0, 0, 80); }           /* dim blue */
static void led_connected(void) { led_set(0, 150, 0); }          /* green */
static void led_reading(void)   { led_set_blink(0, 100, 200, 250); }  /* cyan flash 4Hz */
static void led_writing(void)   { led_set_blink(255, 100, 0, 250); }  /* orange flash 4Hz */
static void led_erasing(void)   { led_set_blink(200, 0, 0, 500); }    /* red flash 2Hz */
static void led_error(void)     { led_set(200, 0, 0); }          /* solid red */
static void led_auth(void)      { led_set(150, 100, 0); }        /* yellow */
static void led_scan_probe(void){ led_set(60, 60, 80); }         /* dim white */
static void led_scan_hit(void)  { led_set(0, 150, 120); }        /* teal */
static void led_vin_write(void) { led_set_blink(200, 80, 0, 300); }   /* orange flash */
static void led_vin_ok(void)    { led_set(0, 255, 0); }          /* bright green */
static void led_success(void)   { led_set_blink(0, 255, 0, 200); }    /* green flash 5Hz */
static void led_celebrate(void) {
    /* Slot machine strobe — rapid random colors for 7 seconds */
    uint32_t end = millis() + 7000;
    uint32_t seed = micros();
    while (millis() < end) {
        seed = seed * 1103515245 + 12345;  /* LCG random */
        uint8_t r = (seed >> 16) & 0xFF;
        uint8_t g = (seed >> 8) & 0xFF;
        uint8_t b = seed & 0xFF;
        /* Boost brightness — at least one channel maxed */
        uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        if (mx < 128) { r = ~r; g = ~g; b = ~b; }
        pixel.setPixelColor(0, r, g, b);
        pixel.show();
        delay(50);
    }
    led_set(0, 255, 0);  /* end on solid green */
}

static void led_waiting(uint32_t duration_ms) {
    /* Smooth fade between orange and yellow — "standby/waiting" indicator */
    uint32_t start = millis();
    while (millis() - start < duration_ms) {
        uint32_t phase = (millis() - start) % 2000;  /* 2-second cycle */
        /* Triangle wave: 0→255→0 over 2 seconds */
        uint8_t blend;
        if (phase < 1000)
            blend = (uint8_t)(phase * 255UL / 1000);
        else
            blend = (uint8_t)((2000 - phase) * 255UL / 1000);
        /* Fade: orange (255,60,0) ↔ yellow (255,220,0) */
        uint8_t g = 60 + (uint8_t)(blend * 160UL / 255);
        pixel.setPixelColor(0, 255, g, 0);
        pixel.show();
        delay(20);
    }
    led_set(0, 255, 0);  /* end on solid green */
}

/* ---------- ISO-TP link instance ---------- */
static IsoTpLink g_isotp_link;
static uint8_t   g_isotp_send_buf[ISOTP_SEND_BUF_SIZE];
static uint8_t   g_isotp_recv_buf[ISOTP_RECV_BUF_SIZE];

/* ---------- State ---------- */
static bool     g_can_initialized = false;
static uint32_t g_can_baud_rate   = 0;             /* current CAN baud (for display) */
static uint32_t g_tester_id       = CAN_TESTER_ID;
static uint32_t g_ecu_id          = CAN_ECU_RESPONSE_ID;
static uint32_t g_last_tester_ms  = 0;
static uint32_t g_last_bcast_ms   = 0;       /* 0x0101 broadcast timer */
static bool     g_auto_tester     = false;   /* auto TesterPresent */
static bool     g_auto_broadcast  = false;   /* auto 0x0101 bus TP broadcast */
static gm_module_t g_module      = MODULE_NONE; /* set by ALGO/MENU; none at boot */
static bool     g_extkern_active   = false;   /* external-kernel raw CAN mode — skip ISO-TP RX */
static bool     g_reading_active  = false;   /* true during any read — suppresses NeoPixel IRQ interference */
static bool     g_t87a_detected  = false;   /* set when 5-byte seed seen on T87 */

/* E92 has two hardware/firmware generations with different unlock strategies:
 *   EARLY (pre-2017, "4-wire") — algo 513 unlocks, live algo in Flashy.
 *   LATE  (2017+ "E92A", "a-module", "3-wire") — algo unknown publicly,
 *                                                 algo 513 will NOT unlock.
 * Variant is detected at the top of cmd_e92_fullread (or via E92ID command)
 * from OSID PN cluster + VIN model-year character. AUTH refuses to fire a
 * $27 02 on LATE so we don't burn the ECM's MEC counter. */
typedef enum {
    E92_VARIANT_UNKNOWN = 0,
    E92_VARIANT_EARLY,
    E92_VARIANT_LATE
} e92_variant_t;
static e92_variant_t g_e92_variant = E92_VARIANT_UNKNOWN;
static e92_variant_t e92_variant_probe(void);  /* reads UDS, sets g_e92_variant */

/* T93 is hardware-identical to T87A (same SPC564A80 MCU). Use this helper
 * anywhere the firmware needs to treat T87/T87A/T93 the same way. */
static inline bool is_t87_family(void) {
    return g_module == MODULE_T87 || g_module == MODULE_T93;
}

/* ---------- SD Card (Adalogger FeatherWing) ---------- */
#define SD_CS_PIN  10
static SdFat     g_sd;
static bool      g_sd_ok = false;
static char      g_sd_filename[128] = {0};  /* set by FULLREAD before read; 128 B to fit /Write/long_bin_name.bin */


/* ---------- Clock.
 * The Adalogger FeatherWing carries a PCF8523 RTC on I2C (if populated).
 * When that's alive, timestamps come from it directly. If it isn't, we
 * fall back to a lightweight in-RAM clock seeded by SETTIME (or to a
 * session-local sequence counter "000001" when nothing is set). */
static RTC_PCF8523 g_rtc;
static bool        g_rtc_ok      = false;
static bool        g_clock_set   = false;
static uint32_t    g_clock_set_ms = 0;
static uint32_t    g_clock_date  = 0;   /* YYMMDD packed, e.g. 260415 */
static uint32_t    g_clock_time  = 0;   /* HHMMSS packed, e.g. 193045 */
static uint32_t    g_read_counter = 0;  /* last-resort filename uniqueness */

/* Write "YYMMDD-HHMMSS" (RTC or in-RAM) or a session counter ("NNNNNN")
 * fallback when no clock is available at all. */
static void format_timestamp(char *out, size_t n)
{
    if (g_rtc_ok) {
        DateTime now = g_rtc.now();
        snprintf(out, n, "%02u%02u%02u-%02u%02u%02u",
                 now.year() % 100, now.month(), now.day(),
                 now.hour(), now.minute(), now.second());
        return;
    }
    if (g_clock_set) {
        uint32_t elapsed = (millis() - g_clock_set_ms) / 1000UL;
        uint32_t hh = (g_clock_time / 10000UL) % 100UL;
        uint32_t mm = (g_clock_time / 100UL) % 100UL;
        uint32_t ss = g_clock_time % 100UL;
        uint32_t total = hh * 3600UL + mm * 60UL + ss + elapsed;
        total %= 86400UL;
        hh = total / 3600UL;
        mm = (total / 60UL) % 60UL;
        ss = total % 60UL;
        snprintf(out, n, "%06lu-%02lu%02lu%02lu",
                 (unsigned long)g_clock_date,
                 (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
        return;
    }
    /* No clock at all — session-local sequence, 1-indexed. */
    g_read_counter++;
    snprintf(out, n, "%06lu", (unsigned long)g_read_counter);
}

/* Human-readable status string for menu header. Separate from the
 * filename-safe format above — may include punctuation. */
static void format_clock_status(char *out, size_t n)
{
    if (g_rtc_ok) {
        DateTime now = g_rtc.now();
        snprintf(out, n, "RTC %04u-%02u-%02u %02u:%02u:%02u",
                 now.year(), now.month(), now.day(),
                 now.hour(), now.minute(), now.second());
    } else if (g_clock_set) {
        char ts[20];
        format_timestamp(ts, sizeof(ts));
        snprintf(out, n, "soft clock %s", ts);
    } else {
        snprintf(out, n, "clock: unset (SETCLOCK YYMMDD HHMMSS)");
    }
}

/* SETCLOCK YYMMDD HHMMSS — seeds the wall clock used for SD filenames.
 * If an RTC is detected on the Adalogger FeatherWing, we write to it so
 * the setting survives a reboot. Otherwise we fall back to an in-RAM
 * soft clock maintained from millis(). Digits only; no punctuation.
 *   SETCLOCK            -> print current
 *   SETCLOCK 260415 1930   -> set 2026-04-15, 19:30:00
 *   SETCLOCK 260415 193045 -> set 2026-04-15, 19:30:45
 */
static void cmd_setclock(const char *arg)
{
    if (!arg || !*arg) {
        char status[40];
        format_clock_status(status, sizeof(status));
        Serial.print("  "); Serial.println(status);
        Serial.println("  usage: SETCLOCK YYMMDD HHMM[SS]");
        Serial.println("  example: SETCLOCK 260415 1930");
        return;
    }
    char buf[32];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *d = strtok(buf, " ");
    char *t = strtok(NULL, " ");
    if (!d || !t) { Serial.println("ERR: SETCLOCK YYMMDD HHMM[SS]"); return; }
    uint32_t date = (uint32_t)strtoul(d, NULL, 10);
    uint32_t tnum = (uint32_t)strtoul(t, NULL, 10);
    if (date == 0 || date > 991231) {
        Serial.println("ERR: date out of range (YYMMDD 0..991231)"); return;
    }
    /* Accept HHMM (pad :00 seconds) or HHMMSS. */
    if (tnum <= 2359) tnum *= 100;
    if (tnum > 235959) { Serial.println("ERR: time out of range"); return; }

    uint32_t yr = 2000 + (date / 10000);
    uint32_t mo = (date / 100) % 100;
    uint32_t dy = date % 100;
    uint32_t hh = tnum / 10000;
    uint32_t mm = (tnum / 100) % 100;
    uint32_t ss = tnum % 100;

    if (g_rtc_ok) {
        g_rtc.adjust(DateTime((uint16_t)yr, (uint8_t)mo, (uint8_t)dy,
                              (uint8_t)hh, (uint8_t)mm, (uint8_t)ss));
    }
    g_clock_date   = date;
    g_clock_time   = tnum;
    g_clock_set_ms = millis();
    g_clock_set    = true;

    char status[40];
    format_clock_status(status, sizeof(status));
    Serial.print("  clock set -> "); Serial.println(status);
    Serial.println("OK");
}

/* ---------- Serial command buffer ---------- */
static char     g_cmd_buf[SERIAL_CMD_MAX_LEN];
static uint16_t g_cmd_pos = 0;

/* ---------- Helpers ---------- */

static void print_ok(void)   { Serial.println("OK"); }
static void print_err(const char *msg) {
    Serial.print("ERR: ");
    Serial.println(msg);
}

static void print_uds_error(int ret, const uds_msg_t *resp) {
    if (ret == UDS_ERR_TIMEOUT) {
        print_err("timeout");
    } else if (ret == UDS_ERR_NEGATIVE && resp) {
        Serial.print("ERR: NRC 0x");
        Serial.println(resp->nrc, HEX);
    } else if (ret == UDS_ERR_SEND) {
        print_err("send failed");
    } else {
        print_err("unknown");
    }
}

static uint32_t parse_hex(const char *s) {
    return (uint32_t)strtoul(s, NULL, 16);
}

static uint32_t parse_dec_or_hex(const char *s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        return (uint32_t)strtoul(s, NULL, 16);
    }
    return (uint32_t)strtoul(s, NULL, 10);
}

/* ---------- 0x0101 bus broadcast helpers ---------- */

/* "FLASHY JACKED IN!!!" rotating 3-stamp signature stuffed into the trailing
 * pad bytes of each 0x101 broadcast — same idea as USBJTAG stamping
 * "USBJTAG" into its heartbeat. Receivers parse only FE <len> [data...]
 * per GMLAN, so extra pad bytes are protocol-inert.
 *
 * Stamps are length-specific so the data bytes never clobber a signature
 * char: frames with 5 pad bytes get the 5-char stamps, frames with 4 pad
 * bytes (2-byte requests like $10 02 / $A5 01 / $A5 03) get 4-char stamps. */
static uint8_t  g_bcast_stamp_idx = 0;
static const char kFlashyStamp5[3][6] = { "FLSHY", "JACKD", "IN!!!" };
static const char kFlashyStamp4[3][5] = { "FLSH", "JACK", "IN!!" };

static void send_broadcast_cmd(const uint8_t *data, uint8_t data_len)
{
    uint8_t frame[8];
    frame[0] = 0xFE;
    frame[1] = data_len;
    if (data_len > 6) data_len = 6;
    memcpy(&frame[2], data, data_len);

    uint8_t pad_start = (uint8_t)(2 + data_len);
    uint8_t pad_len   = (uint8_t)(8 - pad_start);
    if (pad_len >= 5) {
        memcpy(&frame[pad_start], kFlashyStamp5[g_bcast_stamp_idx], 5);
    } else if (pad_len >= 4) {
        memcpy(&frame[pad_start], kFlashyStamp4[g_bcast_stamp_idx], 4);
    } else if (pad_len > 0) {
        memset(&frame[pad_start], 0x00, pad_len);
    }
    g_bcast_stamp_idx = (uint8_t)((g_bcast_stamp_idx + 1) % 3);
    can_send(CAN_BROADCAST_ID, frame, 8);
}

static void send_broadcast_tp(void)
{
    /* FE 01 3E — bus-wide TesterPresent on CAN ID 0x0101.
     * send_broadcast_cmd does the signature stamping. */
    uint8_t tp[1] = { 0x3E };
    send_broadcast_cmd(tp, 1);
    g_last_bcast_ms = millis();
}

static void send_broadcast_tp_if_due(void)
{
    if (!g_reading_active) led_update();  /* skip LED during reads — pixel.show() disables IRQs */
    if (g_auto_broadcast && g_can_initialized) {
        uint32_t now = millis();
        if ((now - g_last_bcast_ms) >= BROADCAST_TP_INTERVAL) {
            send_broadcast_tp();
        }
    }
}

/* E38 programming mode entry — broadcast sequence matching stock tool capture */
static void e38_enter_programming_mode(void)
{
    uint8_t cmd[2];

    Serial.println("E38: DiagSession programming (broadcast)...");
    cmd[0] = 0x10; cmd[1] = 0x02;
    send_broadcast_cmd(cmd, 2);
    delay(500);

    Serial.println("E38: CommunicationControl disable (broadcast)...");
    cmd[0] = 0x28;
    send_broadcast_cmd(cmd, 1);
    delay(100);

    Serial.println("E38: disableDTCSetting (broadcast)...");
    cmd[0] = 0xA2;
    send_broadcast_cmd(cmd, 1);
    delay(1000);

    Serial.println("E38: requestProgrammingMode (broadcast)...");
    cmd[0] = 0xA5; cmd[1] = 0x01;
    send_broadcast_cmd(cmd, 2);
    delay(500);

    Serial.println("E38: enableProgrammingMode (broadcast)...");
    cmd[0] = 0xA5; cmd[1] = 0x03;
    send_broadcast_cmd(cmd, 2);
    delay(500);

    Serial.println("E38: TesterPresent (broadcast x2)...");
    cmd[0] = 0x3E;
    send_broadcast_cmd(cmd, 1);
    delay(13);
    send_broadcast_cmd(cmd, 1);
    delay(50);

    Serial.println("E38: programming mode active");
}

/* E38 return to normal after read — broadcast 0x20 */
static void e38_return_to_normal(void)
{
    uint8_t cmd[1] = { 0x20 };
    send_broadcast_cmd(cmd, 1);
    Serial.println("E38: returnToNormalMode sent");
}

/* ---------- CAN RX polling → ISO-TP ---------- */

static void poll_can_rx(void)
{
    /* In external-kernel raw CAN mode, do NOT consume frames — leave them
     * for extkern_recv() to pick up. The ISO-TP layer has no business
     * seeing raw kernel protocol frames. */
    if (g_extkern_active) return;

    /* Keep broadcast TesterPresent alive during long UDS waits (erase etc.) */
    send_broadcast_tp_if_due();

    uint32_t rx_id;
    uint8_t  rx_data[8];
    uint8_t  rx_len;

    while (can_receive(&rx_id, rx_data, &rx_len) == 0) {
        if (rx_id == g_ecu_id) {
            isotp_on_can_message(&g_isotp_link, rx_data, rx_len);
            /*
             * If a complete ISO-TP message is now ready, stop processing
             * more CAN frames. This prevents the next incoming frame
             * (e.g. a First Frame) from overwriting the receive buffer
             * before the caller has a chance to consume the message.
             */
            if (g_isotp_link.receive_status == ISOTP_RECEIVE_STATUS_FULL) {
                break;
            }
        }
    }
}

/* ---------- Command handlers ---------- */

static void cmd_init(const char *arg)
{
    uint32_t baud = CAN_DEFAULT_BAUD;
    if (arg && *arg) {
        baud = parse_dec_or_hex(arg);
    }

    if (can_init(baud) != 0) {
        print_err("CAN init failed");
        return;
    }

    isotp_init_link(&g_isotp_link, g_tester_id,
                    g_isotp_send_buf, sizeof(g_isotp_send_buf),
                    g_isotp_recv_buf, sizeof(g_isotp_recv_buf));

    uds_set_poll_callback(poll_can_rx);
    g_can_initialized = true;
    g_can_baud_rate   = baud;
    g_extkern_active   = false;  /* reset external-kernel mode on re-init */
    led_connected();
    Serial.print("CAN initialized at ");
    Serial.print(baud);
    Serial.println(" bps");
    print_ok();
}

static void cmd_autoinit(void)
{
    Serial.println("AUTOINIT: scanning 500k / 250k / 1M / 125k ...");
    Serial.flush();
    uint32_t baud = can_autobaud();
    if (baud == 0) {
        print_err("no CAN bus activity detected — is the ECU powered on?");
        return;
    }

    isotp_init_link(&g_isotp_link, g_tester_id,
                    g_isotp_send_buf, sizeof(g_isotp_send_buf),
                    g_isotp_recv_buf, sizeof(g_isotp_recv_buf));

    uds_set_poll_callback(poll_can_rx);
    g_can_initialized = true;
    g_can_baud_rate   = baud;
    g_extkern_active   = false;
    led_connected();
    Serial.print("CAN locked at ");
    Serial.print(baud);
    Serial.println(" bps");
    print_ok();
}

static void cmd_setid(const char *arg)
{
    /* SETID <tester_hex> <ecu_hex> */
    char buf[64];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tstr = strtok(buf, " ");
    char *estr = strtok(NULL, " ");
    if (!tstr || !estr) {
        print_err("usage: SETID <tester_id> <ecu_id>");
        return;
    }

    g_tester_id = parse_hex(tstr);
    g_ecu_id    = parse_hex(estr);

    /* Re-init ISO-TP link with new tester ID */
    if (g_can_initialized) {
        isotp_init_link(&g_isotp_link, g_tester_id,
                        g_isotp_send_buf, sizeof(g_isotp_send_buf),
                        g_isotp_recv_buf, sizeof(g_isotp_recv_buf));
    }

    Serial.print("Tester=0x"); Serial.print(g_tester_id, HEX);
    Serial.print(" ECU=0x");   Serial.println(g_ecu_id, HEX);
    print_ok();
}

static void cmd_diag(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    uint8_t session = UDS_SESSION_DEFAULT;
    if (arg && *arg) session = (uint8_t)parse_dec_or_hex(arg);

    uds_msg_t resp;
    int ret = uds_diagnostic_session(&g_isotp_link, session, &resp);
    if (ret == UDS_OK) {
        Serial.print("Session 0x"); Serial.print(session, HEX);
        Serial.println(" active");
        print_ok();
    } else {
        print_uds_error(ret, &resp);
    }
}

static void cmd_ping(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    int ret = uds_tester_present(&g_isotp_link);
    if (ret == UDS_OK) {
        Serial.println("ECU alive");
        print_ok();
    } else {
        print_err("no response");
    }
}

static bool seed_is_all_zero(const uint8_t *seed, uint16_t len);

/* ------------------------------------------------------------------
 * Public-build check
 *
 * In a public Flashy build (no proprietary *_private.h files supplied
 * by the user), kernel arrays and seed-key bytecode are zero stubs.
 * Any command that needs to upload a kernel or compute a seed-key
 * would fail in a confusing way (NRC 0x35 invalidKey, or 0-byte
 * upload). Call this at the top of those commands so the user sees
 * a clear "no kernel for this module" message instead.
 *
 * Returns true if the active module has no kernel embedded — caller
 * should bail. Returns false if the module is supported.
 * ------------------------------------------------------------------ */
static bool no_kernel_for_active_module(void)
{
    bool stubbed = false;
    const char *name = "?";

    /* No module picked yet — give the user a clear next-step nudge
     * instead of treating this as a 'no kernel' situation. */
    if (g_module == MODULE_NONE) {
        led_error();
        Serial.println("ERR: No module selected.");
        Serial.println("     Type MENU to pick interactively, or ALGO <module> on the");
        Serial.println("     command line. Modules: e38, e67, e92, t87, t87a, t93, t42.");
        return true;
    }

    switch (g_module) {
        case MODULE_E38:
            name = "E38";
            stubbed = (E38_KERNEL_SIZE == 0);
            break;
        case MODULE_E38N:
            name = "E38N";
            stubbed = (E38N_KERNEL_SIZE == 0);
            break;
        case MODULE_E67:
            name = "E67";
            stubbed = (E67_KERNEL_SIZE == 0);
            break;
        case MODULE_T87:
            name = g_t87a_detected ? "T87A" : "T87";
#if KERNEL_T87A_AVAILABLE
            if (g_t87a_detected) { stubbed = false; break; }
#endif
            stubbed = (T87_KERNEL_SIZE == 0);
            break;
        case MODULE_T93:
            name = "T93";
            stubbed = true;     /* no T93 kernel yet */
            break;
        case MODULE_T42:
            name = "T42";
            stubbed = true;     /* no T42 kernel yet */
            break;
        case MODULE_E40:
            name = "E40";
            stubbed = true;     /* no E40 kernel yet */
            break;
        case MODULE_E92:
            name = "E92";
#if KERNEL_E92_AVAILABLE
            stubbed = false;
#else
            stubbed = true;
#endif
            break;
        default:
            name = "?";
            stubbed = true;
            break;
    }

    if (!stubbed) return false;

    led_error();
    Serial.print("ERR: No kernel/algo embedded for ");
    Serial.print(name);
    Serial.println(" in this build.");
    Serial.println("     This public Flashy build supports only E92 (clean-room kernel).");
    Serial.println("     For other modules, see CONTRIBUTING.md to supply your own kernel");
    Serial.println("     headers and rebuild firmware locally.");
    return true;
}

static void cmd_auth(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (no_kernel_for_active_module()) return;

    /* E92 variant probe: lazy-fill so the seed_len==5 branch below knows
     * to route LATE (E92A) through algo 146 instead of T87A's algo 135. */
    if (g_module == MODULE_E92 && g_e92_variant == E92_VARIANT_UNKNOWN) {
        (void)e92_variant_probe();
    }

    led_auth();
    /* Step 1: request seed (level 0x01) */
    uint8_t  seed[32];
    uint16_t seed_len = 0;
    int ret = uds_security_access_seed(&g_isotp_link, 0x01, seed, &seed_len);
    if (ret != UDS_OK) { print_err("seed request failed"); return; }

    Serial.print("SEED: ");
    for (uint16_t i = 0; i < seed_len; i++) {
        if (seed[i] < 0x10) Serial.print('0');
        Serial.print(seed[i], HEX);
    }
    Serial.println();

    /* 5-byte seed = T87A family — except for E92A LATE, which also uses
     * a 5-byte seed but a different algo (146 vs T87A's 135). Don't flag
     * T87A in that case. */
    if (seed_len == 5 && g_module != MODULE_E92) g_t87a_detected = true;

    /* Check for all-zero seed (already unlocked) */
    if (seed_is_all_zero(seed, seed_len)) {
        led_connected();
        Serial.println(g_t87a_detected ? "Security: already unlocked (T87A, seed=0)" : "Security: already unlocked (seed=0)");
        print_ok();
        return;
    }

    /* Compute or parse the key */
    uint8_t  key[128];
    uint16_t key_len = 0;

    if (arg && *arg) {
        /* Manual key provided as hex */
        const char *p = arg;
        while (*p && key_len < sizeof(key)) {
            char hex[3] = { p[0], p[1], '\0' };
            key[key_len++] = (uint8_t)strtoul(hex, NULL, 16);
            p += 2;
            if (*p == ' ') p++;
        }
    } else if (seed_len == 2) {
        /* Auto-compute key from 16-bit seed using seed-key algorithm */
        uint16_t seed16 = (uint16_t)((seed[0] << 8) | seed[1]);
        uint16_t key16  = seedkey_compute(seed16);
        key[0] = (uint8_t)(key16 >> 8);
        key[1] = (uint8_t)(key16 & 0xFF);
        key_len = 2;
        Serial.print("KEY:  ");
        if (key[0] < 0x10) Serial.print('0');
        Serial.print(key[0], HEX);
        if (key[1] < 0x10) Serial.print('0');
        Serial.println(key[1], HEX);
    } else if (seed_len == 5) {
        /* 5-byte seed: route to the right algo by module.
         *   - E92 LATE / E92A    -> algo 146 (bench-verified 2026-04-25)
         *   - T87A / T93 / others -> algo 135 (existing) */
        bool ok;
        const char *algo_name;
        if (g_module == MODULE_E92 && g_e92_variant == E92_VARIANT_LATE) {
            ok = gm5byte_compute_key_e92a(seed, key);
            algo_name = "146";
        } else {
            ok = gm5byte_compute_key(seed, key);
            algo_name = "135";
        }
        if (!ok) {
            print_err("5-byte seed out of range");
            return;
        }
        key_len = 5;
        Serial.print("KEY (algo "); Serial.print(algo_name); Serial.print("):  ");
        for (int i = 0; i < 5; i++) {
            if (key[i] < 0x10) Serial.print('0');
            Serial.print(key[i], HEX);
        }
        Serial.println();
    } else {
        Serial.println("Provide key with: AUTH <key_hex>");
        return;
    }

    ret = uds_security_access_key(&g_isotp_link, 0x02, key, key_len);
    if (ret == UDS_OK) {
        led_connected();
        Serial.println("Security unlocked");
        print_ok();
    } else {
        led_error();
        print_err("key rejected");
    }
}

static void cmd_algo(const char *arg)
{
    if (!arg || !*arg) {
        Serial.println("usage: ALGO <t87|e38|e38n|t42|e92>");
        return;
    }
    char name[16];
    strncpy(name, arg, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    /* uppercase */
    for (int i = 0; name[i]; i++) {
        if (name[i] >= 'a' && name[i] <= 'z') name[i] -= 32;
    }

    /* ECM slot 0 (0x7E0/0x7E8) vs TCM slot 2 (0x7E2/0x7EA) per ISO 15765-4 GM.
     * Before this fix, cmd_algo only set the seed-key algo + g_module — it left
     * the CAN IDs at their boot defaults (0x7E0/0x7E8), which meant `ALGO T87`
     * followed by HSREAD/HSWRITE/FULLREAD silently shouted at the ECM slot and
     * the TCM never heard anything. menu_select_module always set the IDs so
     * the MENU path worked, masking the bug. */
    uint32_t new_tx = g_tester_id;
    uint32_t new_rx = g_ecu_id;

    if (strcmp(name, "T87") == 0) {
        seedkey_set_algo(SEEDKEY_T87);
        g_module = MODULE_T87;
        new_tx = 0x7E2; new_rx = 0x7EA;
        Serial.println("Algo: T87 (569) — module set, IDs 0x7E2/0x7EA");
    } else if (strcmp(name, "E38") == 0) {
        seedkey_set_algo(SEEDKEY_E38);
        g_module = MODULE_E38;
        new_tx = 0x7E0; new_rx = 0x7E8;
        Serial.println("Algo: E38 (402) — module set, IDs 0x7E0/0x7E8");
    } else if (strcmp(name, "T42") == 0) {
        seedkey_set_algo(SEEDKEY_T42);
        g_module = MODULE_T42;
        new_tx = 0x7E2; new_rx = 0x7EA;
        Serial.println("Algo: T42 (371) — module set, IDs 0x7E2/0x7EA");
    } else if (strcmp(name, "E92") == 0) {
        seedkey_set_algo(SEEDKEY_E92);
        g_module = MODULE_E92;
        new_tx = 0x7E0; new_rx = 0x7E8;
        Serial.println("Algo: E92 (513) — module set, IDs 0x7E0/0x7E8");
    } else if (strcmp(name, "E67") == 0) {
        seedkey_set_algo(SEEDKEY_E67);
        g_module = MODULE_E67;
        new_tx = 0x7E0; new_rx = 0x7E8;
        Serial.println("Algo: E67 (393) — module set, IDs 0x7E0/0x7E8");
    } else if (strcmp(name, "E38N") == 0) {
        seedkey_set_algo(SEEDKEY_E38);  /* same seed-key as E38 */
        g_module = MODULE_E38N;
        new_tx = 0x7E0; new_rx = 0x7E8;
        Serial.println("Algo: E38N (EXTKERN) — module set, IDs 0x7E0/0x7E8");
    } else if (strcmp(name, "E40") == 0) {
        g_module = MODULE_E40;
        new_tx = 0x7E0; new_rx = 0x7E8;
        Serial.println("Algo: E40 — module set, IDs 0x7E0/0x7E8 (no seed-key algo yet)");
    } else if (strcmp(name, "T93") == 0) {
        seedkey_set_algo(SEEDKEY_T87);  /* T93 same hw as T87A — reuse T87 algo for HS-CAN until T93 algo confirmed */
        g_module = MODULE_T93;
        new_tx = 0x7E2; new_rx = 0x7EA;
        Serial.println("Algo: T93 — module set, IDs 0x7E2/0x7EA (SPC564A80, BAM heartbeat 0x5D)");
    } else {
        print_err("unknown module. Use: T87 E38 E67 E38N E40 T42 E92 T93");
        return;
    }

    if (new_tx != g_tester_id || new_rx != g_ecu_id) {
        g_tester_id = new_tx;
        g_ecu_id    = new_rx;
        isotp_init_link(&g_isotp_link, g_tester_id,
                        g_isotp_send_buf, sizeof(g_isotp_send_buf),
                        g_isotp_recv_buf, sizeof(g_isotp_recv_buf));
    }
    print_ok();
}

/* Forward declarations for EXTKERN helpers used in cmd_kernel */
static bool extkern_wait_heartbeat(uint32_t timeout_ms);
static void extkern_drain_rx(void);
static void t87_enter_programming_mode(void);

#if KERNEL_E92_AVAILABLE
/* Wait up to timeout_ms for our clean-room kernel's hello ACK on g_ecu_id.
 * The Kernel emits:  41 46 4C 53 48 59 01 00  ("A" "F" "L" "S" "H" "Y" 01 00).
 * Returns true if seen. Prints first few frames for diagnostics. */
/* Scan for a kernel response on 0x7E8. Kernel answers $1A BB with the
 * ISO-TP single frame: 07 5A BB 46 4C 53 48 59 ("5A BB FLSHY").
 * Returns true on that match. Logs every 7E8 frame seen for diagnostics. */
static bool kernel_wait_ack(uint32_t timeout_ms)
{
    uint32_t deadline = millis() + timeout_ms;
    uint32_t frame_count = 0;
    while (millis() < deadline) {
        uint32_t id; uint8_t data[8]; uint8_t len;
        if (can_receive(&id, data, &len) != 0) continue;
        if (id != g_ecu_id) continue;   /* ignore vehicle bus chatter */
        frame_count++;
        Serial.print("  RX 0x"); Serial.print(id, HEX);
        Serial.print(" ["); Serial.print(len); Serial.print("]");
        for (uint8_t i = 0; i < len && i < 8; i++) {
            Serial.print(' ');
            if (data[i] < 0x10) Serial.print('0');
            Serial.print(data[i], HEX);
        }
        Serial.println();
        /* Match "07 5A BB F L S H Y" — the kernel's $1A BB positive reply. */
        if (len == 8 && data[0] == 0x07 && data[1] == 0x5A &&
            data[2] == 0xBB && data[3] == 'F' && data[4] == 'L' &&
            data[5] == 'S' && data[6] == 'H' && data[7] == 'Y') {
            Serial.println("Kernel ALIVE: 5A BB FLSHY");
            return true;
        }
    }
    return false;
}

/* Full programming-mode entry + kernel upload + execute + ACK check.
 * Returns true only if the kernel answered "5A BB FLSHY" on 0x7E8.
 * Callers (cmd_kernel_upload, cmd_e92_fullread) do their own framing. */
static bool kernel_upload_and_verify(void)
{
    if (!g_can_initialized) { print_err("not initialized — INIT first"); return false; }
    if (g_module != MODULE_E92) {
        print_err("KERNEL is E92-only at present — run ALGO e92 first");
        return false;
    }

    const uint8_t *kernel_data = KERNEL_E92_KERNEL;
    const uint16_t kernel_size = KERNEL_E92_SIZE;
    const uint32_t load_addr   = KERNEL_E92_LOAD_ADDR;

    Serial.print("KERNEL: Flashy clean-room kernel, ");
    Serial.print(kernel_size);
    Serial.print(" bytes @ 0x"); Serial.println(load_addr, HEX);

    led_writing();

    /* Do the full stock tool-style programming-mode entry inline so there is
     * no step-order ambiguity. Matches the known-good E92 capture:
     *   $28 00 broadcast     — disableNormalCommunication
     *   $27 01 / $27 02      — SecurityAccess (seed/key)
     *   $A5 01 broadcast     — requestProgrammingMode
     *   $A5 03 broadcast     — enableProgrammingMode
     *   $34                  — RequestDownload (no reboot delay)
     */
    uds_msg_t resp;
    int ret;
    bool already_in_prog_mode = false;   /* set when $27 returns seed=0 */

    /* $10 02 programmingSession — establish diagnostic session so $27 works.
     * stock tool's capture doesn't show this because the host had already
     * been reading $1A identifiers earlier. From a cold firmware state
     * we need it explicitly. */
    Serial.println("  $10 02 programmingSession");
    {
        uint8_t r[2] = { 0x10, 0x02 };
        ret = uds_request(&g_isotp_link, r, 2, &resp, UDS_PENDING_TIMEOUT_MS);
        if (ret != UDS_OK) {
            led_error();
            Serial.print("ERR: $10 02 failed ret="); Serial.println(ret);
            if (ret == UDS_ERR_NEGATIVE) { Serial.print("  NRC=0x"); Serial.println(resp.nrc, HEX); }
            return false;
        }
    }
    delay(20);

    /* $28 00 broadcast */
    Serial.println("  $28 00 disableNormalCommunication");
    {
        uint8_t b[8] = { 0xFE, 0x01, 0x28, 0x00, 0, 0, 0, 0 };
        can_send(0x101, b, 8);
    }
    delay(20);

    /* $27 01 / $27 02 SecurityAccess (AUTH) */
    Serial.println("  $27 SecurityAccess");
    {
        uint8_t sa_req[2] = { 0x27, 0x01 };
        ret = uds_request(&g_isotp_link, sa_req, 2, &resp, UDS_PENDING_TIMEOUT_MS);
        /* UDS layer strips the sub-function byte — for $27 01 the positive
         * reply "67 01 XX YY" arrives as service=0x67, data = {XX, YY}. */
        if (ret != UDS_OK || resp.service != 0x67 || resp.data_len < 2) {
            led_error();
            Serial.print("ERR: seed request failed ret="); Serial.println(ret);
            Serial.print("  resp.service=0x");  Serial.println(resp.service, HEX);
            Serial.print("  resp.data_len=");   Serial.println(resp.data_len);
            Serial.print("  resp.nrc=0x");      Serial.println(resp.nrc, HEX);
            return false;
        }
        /* Detect seed length from the response. E92 EARLY = 2 bytes,
         * E92 LATE / E92A = 5 bytes (algo 146). */
        const bool is_e92a_late = (g_module == MODULE_E92 &&
                                   g_e92_variant == E92_VARIANT_LATE &&
                                   resp.data_len >= 5);

        if (is_e92a_late) {
            /* 5-byte path: algo 146 */
            uint8_t seed5[5];
            memcpy(seed5, resp.data, 5);
            Serial.print("  SEED: ");
            for (int i = 0; i < 5; i++) {
                if (seed5[i] < 0x10) Serial.print('0');
                Serial.print(seed5[i], HEX);
            }
            Serial.println();

            /* All-zero seed = already unlocked */
            bool seed0 = true;
            for (int i = 0; i < 5; i++) if (seed5[i]) { seed0 = false; break; }
            if (seed0) {
                Serial.println("  seed=0 -> already authenticated & in prog. mode");
                Serial.println("  (skipping key + $A5 broadcasts, direct to $34)");
                already_in_prog_mode = true;
            } else {
                uint8_t key5[5];
                if (!gm5byte_compute_key_e92a(seed5, key5)) {
                    led_error();
                    Serial.println("ERR: 5-byte seed out of range (algo 146)");
                    return false;
                }
                Serial.print("  KEY (algo 146):  ");
                for (int i = 0; i < 5; i++) {
                    if (key5[i] < 0x10) Serial.print('0');
                    Serial.print(key5[i], HEX);
                }
                Serial.println();

                uint8_t sa_key[7] = { 0x27, 0x02,
                                       key5[0], key5[1], key5[2], key5[3], key5[4] };
                ret = uds_request(&g_isotp_link, sa_key, 7, &resp, UDS_PENDING_TIMEOUT_MS);
                if (ret != UDS_OK) {
                    led_error();
                    Serial.print("ERR: key rejected ret="); Serial.println(ret);
                    if (ret == UDS_ERR_NEGATIVE) { Serial.print("  NRC=0x"); Serial.println(resp.nrc, HEX); }
                    return false;
                }
                Serial.println("  Security unlocked (E92A algo 146)");
            }
        } else {
            /* 2-byte path: algo 513 (E92 EARLY) */
            uint16_t seed = ((uint16_t)resp.data[0] << 8) | resp.data[1];
            Serial.print("  SEED: 0x"); Serial.println(seed, HEX);

            /* GM convention: seed == 0 means "already authenticated". The ECU
             * remembers our prior $27 02 across the kernel crash and signals
             * we're still unlocked. In that case DON'T repeat $A5 01/03 —
             * re-broadcasting programming-mode entry while already in it
             * makes $34 fail with NRC 0x22 conditionsNotCorrect. Observed
             * clearly on E92-Kernel_Full_Read2.csv re-upload #3. */
            if (seed == 0) {
                Serial.println("  seed=0 -> already authenticated & in prog. mode");
                Serial.println("  (skipping key + $A5 broadcasts, direct to $34)");
                already_in_prog_mode = true;
            } else {
                uint16_t key = seedkey_compute(seed);
                Serial.print("  KEY:  0x"); Serial.println(key, HEX);

                uint8_t sa_key[4] = { 0x27, 0x02, (uint8_t)(key >> 8), (uint8_t)key };
                ret = uds_request(&g_isotp_link, sa_key, 4, &resp, UDS_PENDING_TIMEOUT_MS);
                if (ret != UDS_OK) {
                    led_error();
                    Serial.print("ERR: key rejected ret="); Serial.println(ret);
                    if (ret == UDS_ERR_NEGATIVE) { Serial.print("  NRC=0x"); Serial.println(resp.nrc, HEX); }
                    return false;
                }
                Serial.println("  Security unlocked");
            }
        }
    }

    /* $A5 01 / $A5 03 programming-mode broadcasts — only on a fresh auth.
     * Skip entirely when the prior auth survives (seed==0 path). */
    if (!already_in_prog_mode) {
        Serial.println("  $A5 01 requestProgrammingMode");
        {
            uint8_t b[8] = { 0xFE, 0x02, 0xA5, 0x01, 0, 0, 0, 0 };
            can_send(0x101, b, 8);
        }
        delay(50);
        Serial.println("  $A5 03 enableProgrammingMode");
        {
            uint8_t b[8] = { 0xFE, 0x02, 0xA5, 0x03, 0, 0, 0, 0 };
            can_send(0x101, b, 8);
        }
        delay(5);  /* capture shows <1 frame gap before $34 */
    }

    /* Step 1: RequestDownload — GM E92 format is "34 00 00 10 00"
     * (declares 4096-byte transfer buffer). The 24-bit value is the
     * bootloader's max block size it can accept, NOT the upload size.
     * Matching stock tool's known-good $34 exactly. */
    uint8_t rd_req[5] = {
        UDS_SID_REQUEST_DOWNLOAD, 0x00, 0x00, 0x10, 0x00
    };
    ret = uds_request(&g_isotp_link, rd_req, sizeof(rd_req), &resp,
                      UDS_PENDING_TIMEOUT_MS);
    if (ret != UDS_OK) {
        led_error();
        Serial.print("ERR: RequestDownload failed ret="); Serial.println(ret);
        if (ret == UDS_ERR_NEGATIVE) {
            Serial.print("  NRC=0x"); Serial.println(resp.nrc, HEX);
        }
        return false;
    }
    Serial.println("RequestDownload accepted");

    /* Step 2a: TransferData — $36 00 [load_addr:4] [kernel_data...]
     * block_seq = 0x00 → upload only, no execute. E92's bootloader
     * answers with $76 when the upload has been received. */
    static uint8_t td_buf[6 + KERNEL_E92_SIZE];
    td_buf[0] = UDS_SID_TRANSFER_DATA;
    td_buf[1] = 0x00;
    td_buf[2] = (uint8_t)(load_addr >> 24);
    td_buf[3] = (uint8_t)(load_addr >> 16);
    td_buf[4] = (uint8_t)(load_addr >> 8);
    td_buf[5] = (uint8_t)(load_addr);
    memcpy(&td_buf[6], kernel_data, kernel_size);
    uint16_t td_len = (uint16_t)(6 + kernel_size);

    Serial.println("  $36 00 TransferData (upload)");
    ret = uds_request(&g_isotp_link, td_buf, td_len, &resp, 15000);
    if (ret != UDS_OK) {
        led_error();
        Serial.print("ERR: TransferData upload failed ret="); Serial.println(ret);
        if (ret == UDS_ERR_NEGATIVE) { Serial.print("  NRC=0x"); Serial.println(resp.nrc, HEX); }
        return false;
    }

    /* Step 2b: Execute — $36 80 [load_addr:4], no payload. This is the
     * separate jump-to-address call stock tool issues after the upload. */
    Serial.println("  $36 80 downloadAndExecute");
    {
        uint8_t exec_req[6] = {
            UDS_SID_TRANSFER_DATA, 0x80,
            (uint8_t)(load_addr >> 24), (uint8_t)(load_addr >> 16),
            (uint8_t)(load_addr >> 8),  (uint8_t)(load_addr)
        };
        /* Short timeout — once the kernel starts running, the UDS layer
         * won't see a reply. UDS_OK, UDS_ERR_TIMEOUT, or UDS_ERR_NEGATIVE
         * are all plausible. Any of them is fine; we'll look for the
         * FLSHY ACK on raw CAN. */
        (void)uds_request(&g_isotp_link, exec_req, 6, &resp, 1000);
    }

    /* Match stock tool's capture: wait ~1 s after $36 80 before polling.
     * Kernel does not send anything unsolicited — we poll with $1A BB
     * (legacy GM readDataByIdentifier) exactly like stock tool does. Our
     * kernel answers with "5A BB FLSHY" so we can tell it apart from
     * any stock bootloader reply. */
    delay(1000);

    Serial.println("  $1A BB poll (legacy readDataByIdentifier)");
    {
        uint8_t poll[8] = { 0x02, 0x1A, 0xBB, 0, 0, 0, 0, 0 };
        can_send(g_tester_id, poll, 8);
    }

    if (kernel_wait_ack(3000)) {
        led_connected();
        Serial.println("=== KERNEL ALIVE ===");
        return true;
    }
    led_error();
    Serial.println("WARN: no kernel response on 0x7E8");
    Serial.println("  -> kernel likely didn't execute (icache? VLE?)");
    return false;
}

static void cmd_kernel_upload(void)
{
    if (kernel_upload_and_verify()) print_ok();
}

/*
 * KERNELREAD [<addr_hex> [<size_hex>]]
 *
 * Streams flash (or any readable memory) from a live Kernel via UDS
 * $23 ReadMemoryByAddress. Must be run after KERNEL has reported ALIVE.
 * Defaults to a full 4 MB flash dump starting at 0x00000000.
 *
 * Per-request block size is 2 KB — matches what stock tool uses on E92 and
 * stays well within the kernel's 4 KB ISO-TP limit.
 */
static void cmd_kernel_read(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (g_module != MODULE_E92) {
        print_err("KERNELREAD is E92-only");
        return;
    }

    uint32_t addr  = 0x00000000UL;
    uint32_t total = 0x00400000UL;   /* 4 MB default */

    if (arg && *arg) {
        char buf[64];
        strncpy(buf, arg, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *a = strtok(buf, " ");
        char *s = strtok(NULL, " ");
        if (a) addr  = parse_hex(a);
        if (s) total = parse_hex(s);
    }

    const uint16_t BLOCK       = 2048;
    const uint8_t  MAX_RETRIES = 3;

    /* Match g_sd_filename's 128-byte size — long paths like
     * /Read/E92_<16-char OSID>_FULLREAD_<13-char ts>.bin run ~54 chars
     * and were getting truncated mid-filename in the old 48-byte buffer. */
    char fname[128];
    if (g_sd_filename[0]) {
        strncpy(fname, g_sd_filename, sizeof(fname) - 1);
        fname[sizeof(fname) - 1] = '\0';
    } else {
        snprintf(fname, sizeof(fname), "kernel_%06lX.bin", (unsigned long)addr);
    }
    FsFile sd_file;
    bool use_sd = false;
    if (g_sd_ok) {
        sd_file = g_sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
        if (sd_file) {
            sd_file.preAllocate(total);
            use_sd = true;
            Serial.print("SD: "); Serial.println(fname);
        } else {
            Serial.println("WARN: SD open failed, progress-only");
        }
    }

    Serial.print("KERNELREAD addr=0x"); Serial.print(addr, HEX);
    Serial.print(" size=0x"); Serial.print(total, HEX);
    Serial.print(" block="); Serial.println(BLOCK);
    led_reading();

    uint32_t start_ms   = millis();
    uint32_t bytes_read = 0;
    uint32_t checksum   = 0;
    uint32_t cur_addr   = addr;

    uint32_t skipped_blocks = 0;
    const uint32_t MAX_SKIPS = 64;   /* abort if >128 KB had to be 0xFF'd */

    /* Broadcast TesterPresent every 1.5 s to keep the whole bus in
     * programming mode. stock tool's reference capture does this ~1 Hz
     * across the full 4 MB read; without it other modules (and sometimes
     * the ECU itself) drop session and flood the bus with noise that
     * races our $23 responses. */
    /* stock tool sends TP on 0x101 roughly every 1.7 s across a full read.
     * 2 s is inside that envelope and gives a clearer rhythm on CAN logs. */
    uint32_t last_tp_ms = millis() - 1000;   /* fire soon after loop entry */
    const uint32_t TP_INTERVAL_MS = 2000;

    while (bytes_read < total) {
        if ((millis() - last_tp_ms) >= TP_INTERVAL_MS) {
            uint8_t tp[8] = { 0xFE, 0x01, 0x3E, 0x00, 0, 0, 0, 0 };
            can_send(0x101, tp, 8);
            last_tp_ms = millis();
        }

        uint16_t blk = (total - bytes_read) > BLOCK ? BLOCK
                                                    : (uint16_t)(total - bytes_read);

        /* MPC5674F flash: the last 2 KB of each 64 KB mid-block (M0 at
         * 0x10000, M1 at 0x20000, etc.) crashes our kernel — likely an
         * ECC/shadow region that triggers a machine-check exception we
         * don't have a handler for. stock tool's kernel handles it; ours
         * doesn't yet. Skip proactively and fill 0xFF to avoid a crash
         * that requires a full power-cycle to recover from.
         *
         * Pattern: addresses of the form (N*0x10000 + 0xF800) for N>=1
         * where N*0x10000 >= 0x10000 (i.e., past the four 16 KB low
         * blocks which read fine). */
        if (cur_addr >= 0x10000UL &&
            (cur_addr & 0xFFFFu) == 0xF800u) {
            uint8_t fill[64];
            memset(fill, 0xFF, sizeof(fill));
            uint16_t written = 0;
            while (written < blk) {
                uint16_t chunk = (blk - written) > sizeof(fill)
                                 ? sizeof(fill)
                                 : (uint16_t)(blk - written);
                if (use_sd) sd_file.write(fill, chunk);
                for (uint16_t i = 0; i < chunk; i++) checksum += 0xFFu;
                written += chunk;
            }
            Serial.print("  PREFILL 0x"); Serial.print(cur_addr, HEX);
            Serial.println(" (known flash-boundary skip)");
            skipped_blocks++;
            bytes_read += blk;
            cur_addr   += blk;
            continue;
        }
        uint8_t req[7] = {
            0x23,
            (uint8_t)(cur_addr >> 24), (uint8_t)(cur_addr >> 16),
            (uint8_t)(cur_addr >>  8), (uint8_t)(cur_addr      ),
            (uint8_t)(blk >> 8),       (uint8_t)(blk           )
        };

        /* Up to MAX_RETRIES attempts on transport-level timeout (ret = -1).
         * An NRC (ret = -2) or short reply is a protocol error and we give
         * up on the retry loop — re-trying the same request won't unstick
         * the kernel once it's crashed, the ECU will just keep NRCing. */
        uds_msg_t resp;
        int ret = UDS_ERR_TIMEOUT;
        uint8_t attempt;
        for (attempt = 1; attempt <= MAX_RETRIES; attempt++) {
            ret = uds_request(&g_isotp_link, req, 7, &resp, 5000);
            if (ret == UDS_OK && resp.service == 0x63 &&
                resp.data_len >= (uint16_t)(3 + blk)) {
                break;   /* success */
            }
            if (ret != UDS_ERR_TIMEOUT) break;   /* non-retryable */
            Serial.print("  retry "); Serial.print(attempt);
            Serial.print("/"); Serial.print(MAX_RETRIES);
            Serial.print(" at 0x"); Serial.println(cur_addr, HEX);
            delay(50);
        }

        bool block_ok = (ret == UDS_OK && resp.service == 0x63 &&
                         resp.data_len >= (uint16_t)(3 + blk));

        if (block_ok) {
            /* Kernel responds: [63][addr:4][data:blk]. The UDS parser
             * stashes raw[0] as service (0x63) and raw[1] as sub_function
             * (= addr MSB), so resp.data starts at raw[2] — that is,
             * [addr:3][data:blk]. */
            const uint8_t *data = &resp.data[3];
            if (use_sd) sd_file.write(data, blk);
            for (uint16_t i = 0; i < blk; i++) checksum += data[i];
        } else {
            /* Block unreadable. Our kernel doesn't send NRCs, so an NRC
             * here means the kernel crashed and the stock bootloader took
             * over. Fill this block with 0xFF so the output .bin stays
             * correctly aligned, log the address, then re-upload the
             * kernel and continue with the NEXT block. */
            Serial.print("  SKIP 0x"); Serial.print(cur_addr, HEX);
            Serial.print(" len="); Serial.print(blk);
            Serial.print(" ret="); Serial.print(ret);
            if (ret == UDS_ERR_NEGATIVE) {
                Serial.print(" NRC=0x"); Serial.print(resp.nrc, HEX);
            }
            Serial.println(" — filling 0xFF + re-uploading kernel");

            uint8_t fill[64];
            memset(fill, 0xFF, sizeof(fill));
            uint16_t written = 0;
            while (written < blk) {
                uint16_t chunk = (blk - written) > sizeof(fill)
                                 ? sizeof(fill)
                                 : (uint16_t)(blk - written);
                if (use_sd) sd_file.write(fill, chunk);
                for (uint16_t i = 0; i < chunk; i++) checksum += 0xFFu;
                written += chunk;
            }

            skipped_blocks++;
            if (skipped_blocks >= MAX_SKIPS) {
                led_error();
                if (use_sd) sd_file.close();
                Serial.print("ABORT: too many skips (");
                Serial.print(skipped_blocks); Serial.println(")");
                return;
            }

            /* Re-upload kernel. After a kernel crash the E92 bootloader
             * has a ~10 s anti-brute-force lockout on $27 (NRC 0x37).
             * $11 01 ECUReset is not supported by this bootloader.
             * Strategy: drop to defaultSession ($10 01) to cleanly exit
             * the programming session, wait for the lockout to expire,
             * then kernel_upload_and_verify does a fresh $10 02 → full
             * auth cycle. Five retries at 5 s each gives 25 s total —
             * enough for the lockout to clear even after edge cases. */
            bool reupload_ok = false;
            for (uint8_t retry = 1; retry <= 5; retry++) {
                Serial.print("  re-upload attempt "); Serial.print(retry);
                Serial.println("/5: $10 01 session-drop + 5 s wait");
                {
                    uint8_t ds[2] = { 0x10, 0x01 };
                    uds_msg_t rresp;
                    (void)uds_request(&g_isotp_link, ds, 2, &rresp, 2000);
                }
                uint32_t wait_start = millis();
                uint32_t next_tp    = millis();
                while (millis() - wait_start < 5000) {
                    if ((int32_t)(millis() - next_tp) >= 0) {
                        uint8_t tp[8] = { 0xFE, 0x01, 0x3E, 0x00, 0, 0, 0, 0 };
                        can_send(0x101, tp, 8);
                        next_tp = millis() + 2000;
                    }
                    delay(20);
                }
                last_tp_ms = millis();
                if (kernel_upload_and_verify()) { reupload_ok = true; break; }
            }
            if (!reupload_ok) {
                led_error();
                if (use_sd) sd_file.close();
                Serial.println("ABORT: kernel re-upload failed after 3 tries "
                               "— power-cycle the ECU and restart E92FULLREAD");
                return;
            }
        }

        bytes_read += blk;
        cur_addr   += blk;

        /* Progress every 64 KB. */
        if ((bytes_read & 0xFFFFu) == 0u) {
            uint32_t el = millis() - start_ms;
            Serial.print("  "); Serial.print(bytes_read);
            Serial.print(" / "); Serial.print(total);
            Serial.print(" ("); Serial.print((bytes_read * 100UL) / total);
            Serial.print("%) "); Serial.print((bytes_read * 1000UL) / (el ? el : 1) / 1024);
            Serial.println(" KB/s");
        }
    }

    if (use_sd) sd_file.close();
    uint32_t elapsed = millis() - start_ms;
    led_connected();

    Serial.print("DONE: "); Serial.print(bytes_read); Serial.print(" bytes in ");
    Serial.print(elapsed / 1000.0f); Serial.print(" s, ");
    Serial.print((bytes_read * 1000UL) / (elapsed ? elapsed : 1) / 1024);
    Serial.println(" KB/s");
    Serial.print("Checksum: 0x"); Serial.println(checksum, HEX);
    print_ok();
}

/*
 * E92FULLREAD — one-shot full flash dump for a 2014+ E92 ECM.
 *
 * Wrapper that chains everything a cold-start dump needs:
 *   1. Ensure CAN bus is up at 500 kbps (INIT if needed)
 *   2. Set module = E92 (pulls in seedkey algo 513, tester/ECU IDs)
 *   3. Read OSID via $1A B4 while still in default session (for filename)
 *   4. Upload + verify our Kernel (kernel_upload_and_verify)
 *   5. Stream the whole 4 MB via $23 ReadMemoryByAddress to SD as
 *        <OSID>-<YYMMDD>-<HHMMSS>.bin   (or -m<millis> if SETTIME unset)
 *
 * Designed to be triggered from the menu — one keypress, full read.
 */
static void cmd_init(const char *);        /* forward decls for chaining */
static void cmd_algo(const char *);

/* T87A kernel-exit trailer — toggle + helper (definition after cmd_calread).
 * Default ON: HSREAD/CALREAD run the 16 MB kill stream inline so the TCM
 * kernel exits cleanly without a power cycle. Adds ~2 min to each session
 * but eliminates the hand-power-cycle step. AUTOKEXIT off to disable. */
static bool g_auto_kexit = true;  /* AUTOKEXIT on/off — default ON */
static bool kexit_run_trailer_and_observe(void);

/* Forward decls needed by cmd_t87a_calwrite (defined earlier in file than
 * VIN/OSID helpers and the Rollin Smoke kernel uploader). */
extern char g_vin[];
extern char g_osid[];
static bool read_vin_from_ecu(void);
static bool do_security_access(void);
static bool read_osid_from_ecu(void);
static bool hsread_upload_rollin_smoke(void);

/* Short module tag for bin filenames: "T87A", "E38", "E92", etc. */
static const char* module_name_for_filename(void);

/* Self-contained OSID fetch. Prefer OBD-II Mode 9 PID 04 (decimal 8-digit PN
 * like "24293216") to match .bin filename convention; fall back to GM $1A B4
 * (16-char alphanumeric cal ID) if Mode 9 isn't answered. */
static bool e92_read_osid_local(char *out, size_t n)
{
    if (n == 0) return false;
    out[0] = '\0';
    uds_msg_t resp;

    /* Try OBD-II $09 04 first. Response: 49 04 <num_ids> <id0:16> ... */
    uint8_t req9[2] = { 0x09, 0x04 };
    int ret = uds_request(&g_isotp_link, req9, 2, &resp, UDS_DEFAULT_TIMEOUT_MS);
    if (ret == UDS_OK && resp.data_len >= 2) {
        uint16_t off = 1;              /* skip num_ids byte */
        uint16_t end = off + 16;
        if (end > resp.data_len) end = resp.data_len;
        size_t j = 0;
        for (uint16_t i = off; i < end && j < n - 1; i++) {
            uint8_t c = resp.data[i];
            if (c == 0x00) break;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z')) {
                out[j++] = (char)c;
            }
        }
        out[j] = '\0';
        if (j > 0) return true;
    }

    /* Fallback: GM $1A B4 (legacy Cal/OS ID). resp.data starts AFTER the
     * stripped sub_function byte, so the first ID byte lives at
     * resp.sub_function, the rest at resp.data[]. */
    uint8_t req[2] = { 0x1A, 0xB4 };
    ret = uds_request(&g_isotp_link, req, 2, &resp, UDS_DEFAULT_TIMEOUT_MS);
    if (ret != UDS_OK) return false;
    size_t j = 0;
    char first = (char)resp.sub_function;
    if (first >= ' ' && first <= '~') {
        if ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
            (first >= '0' && first <= '9') || first == '-' || first == '_') {
            if (j < n - 1) out[j++] = first;
        } else if (j < n - 1) out[j++] = '_';
    }
    for (uint16_t i = 0; i < resp.data_len && j < n - 1; i++) {
        char c = (char)resp.data[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out[j++] = c;
        } else if (c >= ' ' && c <= '~') {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
    return (j > 0);
}

/* E92 OEM part numbers documented in public part-number databases as
 * belonging to each generation. Sourced from research memory
 * e92_variants.md — public data only, safe for public release. */
static const uint32_t E92_EARLY_PN[] = {
    12669908, 12676683, 12680876, 12683660, 12687328, 12687467,
    12688128, 12688973, 12695198, 12697788, 12700116, 12702505,
    12636060, 19432266,
    12671993   /* 2016 bench unit, VIN 1G1YU2D60G5XXXXXXX — confirmed
                * 2026-04-25: $27 01/03 returns 2-byte seed (660B aliased
                * across both levels), algo 513 unlocks. */
};
static const uint32_t E92_LATE_PN[]  = {
    12674052, 12692067, 12692069, 12704475, 12680656, 12686383,
    12688528,
    12699552   /* 2020 bench unit, VIN masked — confirmed 2026-04-24:
                * $27 01 returns 2-byte seed, $27 02 returns NRC 0x12
                * (level 02 not supported). Algo 513 may still be correct
                * but sub-level pair is wrong — see E92SAPROBE. */
};

/* VIN position 10 (0-indexed 9) encodes model year per ISO 3779/3780.
 * 2010-2039 cycle chars: A..Y skipping I/O/Q/U/Z. Returns 4-digit year
 * or 0 on unrecognised char. */
static uint16_t e92_vin_model_year(const char *vin)
{
    if (!vin || strlen(vin) < 10) return 0;
    char c = vin[9];
    switch (c) {
        case 'A': return 2010; case 'B': return 2011; case 'C': return 2012;
        case 'D': return 2013; case 'E': return 2014; case 'F': return 2015;
        case 'G': return 2016; case 'H': return 2017; case 'J': return 2018;
        case 'K': return 2019; case 'L': return 2020; case 'M': return 2021;
        case 'N': return 2022; case 'P': return 2023; case 'R': return 2024;
        case 'S': return 2025; case 'T': return 2026; case 'V': return 2027;
        case 'W': return 2028; case 'X': return 2029; case 'Y': return 2030;
        default: return 0;
    }
}

/* Parse an OSID/PN string (Mode 9 $04 cal ID, or $1A B4 OS PN) to an
 * 8-digit GM PN if possible. Extracts the first run of digits of length
 * 8; returns 0 if nothing matches. */
static uint32_t e92_parse_pn(const char *osid)
{
    if (!osid) return 0;
    const char *p = osid;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            int n = 0;
            uint32_t v = 0;
            while (p[n] >= '0' && p[n] <= '9' && n < 9) {
                v = v * 10 + (uint32_t)(p[n] - '0');
                n++;
            }
            if (n == 8) return v;
            p += n;
        } else {
            p++;
        }
    }
    return 0;
}

/* Classify from already-known PN + VIN strings. Does NOT touch the bus.
 * Priority: PN exact match > VIN year > UNKNOWN. */
static e92_variant_t e92_classify_variant(uint32_t pn, uint16_t year)
{
    if (pn) {
        for (size_t i = 0; i < sizeof(E92_EARLY_PN)/sizeof(E92_EARLY_PN[0]); i++) {
            if (E92_EARLY_PN[i] == pn) return E92_VARIANT_EARLY;
        }
        for (size_t i = 0; i < sizeof(E92_LATE_PN)/sizeof(E92_LATE_PN[0]); i++) {
            if (E92_LATE_PN[i] == pn) return E92_VARIANT_LATE;
        }
    }
    if (year >= 2014 && year <= 2016) return E92_VARIANT_EARLY;
    if (year >= 2018)                 return E92_VARIANT_LATE;
    /* 2017 is the overlap year; without a PN match we can't tell. */
    return E92_VARIANT_UNKNOWN;
}

/* Probe E92: read VIN + OSID over CAN, classify, print banner, cache
 * result in g_e92_variant. Returns the detected variant. Safe to call
 * multiple times — each call is three short UDS requests ($1A 90,
 * $09 04 or $1A B4). No writes, no session changes. */
static e92_variant_t e92_variant_probe(void)
{
    if (!g_can_initialized) {
        Serial.println("E92ID: CAN not initialized — run INIT first");
        return E92_VARIANT_UNKNOWN;
    }

    char osid[20] = {0};
    (void)read_vin_from_ecu();           /* fills g_vin */
    (void)e92_read_osid_local(osid, sizeof(osid));

    uint32_t pn    = e92_parse_pn(osid);
    uint16_t year  = e92_vin_model_year(g_vin);
    e92_variant_t v = e92_classify_variant(pn, year);
    g_e92_variant = v;

    Serial.print("E92 ID: PN=");
    if (pn) Serial.print(pn);
    else    Serial.print(osid[0] ? osid : "?");
    Serial.print("  VIN=");
    Serial.print(g_vin[0] ? g_vin : "?");
    if (year) { Serial.print(" (year="); Serial.print(year); Serial.print(")"); }
    Serial.println();

    Serial.print("E92 Variant: ");
    switch (v) {
        case E92_VARIANT_EARLY:
            Serial.println("EARLY (pre-2017, algo 513) — SUPPORTED");
            break;
        case E92_VARIANT_LATE:
            Serial.println("LATE / E92A (2017+, algo 146) — SUPPORTED");
            break;
        default:
            Serial.println("UNKNOWN — PN not in catalog, VIN year inconclusive");
            Serial.println("  AUTH will attempt EARLY (algo 513) by default;");
            Serial.println("  if it fails, run E92ID then AUTH again to retry as LATE.");
            break;
    }
    return v;
}

static void cmd_e92id(void)
{
    (void)e92_variant_probe();
    print_ok();
}

/* E92SAPROBE — diagnostic sweep of SecurityAccess sub-function levels.
 * Sends $27 <odd-level> requestSeed to each common GM Gen-V pair and
 * reports which levels return a seed, the seed length, and whether the
 * seed is non-zero/non-stub. NEVER sends a key ($27 even-level), so the
 * MEC counter is untouched — safe to run repeatedly.
 *
 * Use case: figure out which level pair E92A wants for programming
 * unlock, since $27 01/02 returns NRC 0x12 on the LATE variant. Run on
 * both EARLY and LATE bench units and compare to identify the pair. */
static void cmd_e92saprobe(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }

    /* Levels GM Gen-V ECMs are commonly seen using. Odd values only
     * (request-seed). Sub-functions $01-$3F are SAE/UDS-defined; higher
     * values are OEM-specific. Keep the list small but representative. */
    static const uint8_t LEVELS[] = {
        0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F, 0x11, 0x13
    };

    Serial.println("E92SAPROBE: scanning SecurityAccess sub-function levels");
    Serial.println("  (request-seed only; no key submission, MEC counter safe)");
    Serial.print  ("  module: ");
    Serial.print(g_module == MODULE_E92 ? "E92" : "(non-E92)");
    Serial.print  ("  variant: ");
    switch (g_e92_variant) {
        case E92_VARIANT_EARLY: Serial.println("EARLY"); break;
        case E92_VARIANT_LATE:  Serial.println("LATE / E92A"); break;
        default:                Serial.println("UNKNOWN (run E92ID first)"); break;
    }

    int got_count = 0;
    for (size_t i = 0; i < sizeof(LEVELS)/sizeof(LEVELS[0]); i++) {
        uint8_t  lvl = LEVELS[i];
        uint8_t  seed[32];
        uint16_t seed_len = 0;
        int ret = uds_security_access_seed(&g_isotp_link, lvl,
                                           seed, &seed_len);

        Serial.print("  $27 ");
        if (lvl < 0x10) Serial.print('0');
        Serial.print(lvl, HEX);
        Serial.print(": ");
        if (ret == UDS_OK) {
            got_count++;
            Serial.print("SEED(");
            Serial.print(seed_len);
            Serial.print("B)=");
            for (uint16_t j = 0; j < seed_len && j < 16; j++) {
                if (seed[j] < 0x10) Serial.print('0');
                Serial.print(seed[j], HEX);
            }
            if (seed_is_all_zero(seed, seed_len)) {
                Serial.print(" [ZERO=already unlocked]");
            }
            Serial.println();
        } else if (ret == UDS_ERR_NEGATIVE) {
            /* Try to extract NRC. uds_security_access_seed doesn't expose
             * the response struct directly, so re-issue via uds_request to
             * get the NRC byte. */
            uint8_t req[2] = { 0x27, lvl };
            uds_msg_t resp;
            int r2 = uds_request(&g_isotp_link, req, 2, &resp,
                                 UDS_DEFAULT_TIMEOUT_MS);
            if (r2 == UDS_ERR_NEGATIVE) {
                Serial.print("NRC 0x"); Serial.println(resp.nrc, HEX);
            } else {
                Serial.println("NEGATIVE (NRC unread)");
            }
        } else if (ret == UDS_ERR_TIMEOUT) {
            Serial.println("timeout");
        } else {
            Serial.print("ret=");
            Serial.println(ret);
        }

        /* Small gap so the ECU doesn't see this as a flood. */
        delay(50);
    }

    Serial.print("E92SAPROBE: ");
    Serial.print(got_count);
    Serial.print(" of ");
    Serial.print((int)(sizeof(LEVELS)/sizeof(LEVELS[0])));
    Serial.println(" levels returned a seed");
    Serial.println("  Hint: on EARLY, expect only $27 01 alive.");
    Serial.println("  On LATE (E92A), look for an additional alive level — that's the");
    Serial.println("  programming-unlock pair we don't yet know the algo for.");
    print_ok();
}

static void cmd_e92_fullread(void)
{
    if (!g_sd_ok) { print_err("no SD card"); return; }

    /* 1. CAN bus. */
    if (!g_can_initialized) {
        Serial.println("E92FULLREAD: initializing CAN @ 500 kbps");
        cmd_init("500000");
        if (!g_can_initialized) return;
    }

    /* 2. Module context. Force E92 IDs + seed-key algo. */
    cmd_algo("e92");

    /* 2b. Variant banner — PN+VIN classifier. If LATE or UNKNOWN, the user
     *     must confirm with YES before we continue, since algo 513 will
     *     fail to unlock and a bad $27 02 increments the ECM's MEC counter. */
    e92_variant_t v = e92_variant_probe();
    if (v == E92_VARIANT_UNKNOWN) {
        Serial.println("  Variant could not be classified (PN not catalogued, year inconclusive).");
        Serial.println("  Proceed anyway? Type YES to continue, anything else aborts:");
        char yn[8]; int yp = 0; uint32_t t0 = millis();
        while (millis() - t0 < 20000) {
            if (Serial.available()) {
                char c = (char)Serial.read();
                if (c == '\n' || c == '\r') { if (yp > 0) break; }
                else if (c == 0x08 || c == 0x7F) { if (yp > 0) yp--; }
                else if (yp < (int)sizeof(yn) - 1) yn[yp++] = c;
            }
        }
        yn[yp] = '\0';
        for (int i = 0; yn[i]; i++) {
            if (yn[i] >= 'a' && yn[i] <= 'z') yn[i] -= 32;
        }
        if (strcmp(yn, "YES") != 0) {
            print_err("E92FULLREAD aborted (variant not classified)");
            return;
        }
    }
    /* EARLY uses algo 513, LATE uses algo 146 — both wired in do_security_access. */

    /* 3. OSID (best-effort; fall back to "E92" if the bootloader is still
     *    in control and rejects both $09 04 and $1A B4). */
    char osid[20] = "E92";
    (void)e92_read_osid_local(osid, sizeof(osid));
    if (osid[0] == '\0') snprintf(osid, sizeof(osid), "E92");

    /* 4. Build the filename and stash it for cmd_kernel_read to use.
     *    Drops into /Read/ alongside HSREAD/BAMREAD outputs. mkdir is a
     *    no-op if the dir exists; only warn on a real failure
     *    (write-protect, full card, FS error). */
    if (g_sd_ok && !g_sd.exists("/Read")) {
        if (!g_sd.mkdir("/Read")) {
            Serial.println("WARN: /Read/ create failed (write-protect or full?) — file save will fail");
        }
    }
    char ts[20];
    format_timestamp(ts, sizeof(ts));
    snprintf(g_sd_filename, sizeof(g_sd_filename),
             "/Read/E92_%s_FULLREAD_%s.bin", osid, ts);
    Serial.print("E92FULLREAD target: "); Serial.println(g_sd_filename);

    /* 5. Kernel. */
    if (!kernel_upload_and_verify()) {
        g_sd_filename[0] = '\0';
        print_err("E92FULLREAD: kernel failed to start — aborting");
        return;
    }

    /* 6. Full 4 MB read. cmd_kernel_read honors g_sd_filename. */
    cmd_kernel_read("0 400000");

    /* 7. Reset the ECU back to normal mode. Kernel handles $11 01 by
     * sending a positive ACK then triggering the MPC5674F's SIU
     * software reset. After reset the stock bootloader / application
     * firmware takes over and normal bus traffic resumes. */
    Serial.println("  Resetting ECU ($11 01)...");
    {
        uint8_t rst[8] = { 0x02, 0x11, 0x01, 0, 0, 0, 0, 0 };
        can_send(g_tester_id, rst, 8);
    }
    delay(2000);
    Serial.println("  ECU reset sent — normal bus traffic should resume");

    /* Clear the override so later commands pick their own names. */
    g_sd_filename[0] = '\0';
}
#else
static void cmd_kernel_upload(void)
{
    print_err("kernel_e67_private.h not present. Run:\n"
              "  python tools/bin2header.py Kernels/e67_read/kernel.bin "
              "src/kernel_e92_private.h KERNEL_E92 0x40001000");
}
static void cmd_kernel_read(const char * /*arg*/)
{
    print_err("KERNELREAD unavailable — rebuild with Kernel header");
}
static void cmd_e92_fullread(void)
{
    print_err("E92FULLREAD unavailable — rebuild with Kernel header");
}
#endif /* KERNEL_E92_AVAILABLE */

/* ============================================================
 * cmd_t42_read — T42 TCM (MC68377) 1 MB read via Flashy clean-room kernel
 *
 * MVP: auth + upload + ping. Proves the kernel boots and responds.
 * Flash read loop is a follow-up once this lands on bench.
 *
 * Sequence (mirrors a captured commercial-tool T42 flow):
 *   1. ALGO T42      — sets IDs 0x7E2/0x7EA + algo 371
 *   2. $27 01/02     — 2-byte seed-key auth
 *   3. $34 00 FF 90 00           — RequestDownload to 0xFF9000
 *   4. $36 80 FF 90 00 <kernel>  — TransferData (1012 bytes kernel payload)
 *   5. Optional $37  — TransferExit (some bootloaders need this to jump)
 *   6. Wait ~500 ms  — kernel boot
 *   7. $1A 55        — FLASHY ping; expect $5A 55 F L A S H
 * ============================================================ */
#if KERNEL_T42_AVAILABLE
/* T42READ variant dispatcher. Accepts an optional letter arg A-E so the
 * bench operator can rapidly A/B-test hypotheses without a firmware rebuild.
 * All variants share the same kernel binary (KERNEL_T42_KERNEL[]). Only
 * host-side protocol flow differs.
 *
 * Variants:
 *   A (default): patches 1+2 — $3E-after-$36 + $1A BB probe. Baseline.
 *   B:           A + full programming-mode sequence ($10 02 + $A2)
 *   C:           A + sustained 100 ms $3E heartbeat from $A5 03 onward
 *   D:           A + probe fan-out ($1A BB, $1A 21, $1A 55, $35, $3E)
 *   E:           A + extended kernel boot delay (2000 ms vs 500 ms)
 */
static void t42_send_3e_bc(void)
{
    uint8_t tp[8] = { 0xFE, 0x01, 0x3E, 0x00, 0, 0, 0, 0 };
    can_send(0x101, tp, 8);
}

static void cmd_t42_read(const char *arg)
{
    char variant = 'A';
    if (arg && arg[0]) {
        char c = arg[0];
        if (c >= 'a' && c <= 'e') c = (char)(c - 'a' + 'A');
        if (c >= 'A' && c <= 'E') variant = c;
    }

    if (!g_can_initialized) { print_err("not initialized — INIT first"); return; }

    Serial.print("T42READ: variant ");
    Serial.print(variant);
    Serial.println(" — auth + kernel upload + FLASHY ping");
    switch (variant) {
        case 'A': Serial.println("  (baseline: patches 1+2)"); break;
        case 'B': Serial.println("  (+ full prog-mode sequence: $10 02, $A2)"); break;
        case 'C': Serial.println("  (+ sustained 100 ms $3E heartbeat from $A5 03)"); break;
        case 'D': Serial.println("  (+ probe fan-out: BB, 21, 55, $35, $3E)"); break;
        case 'E': Serial.println("  (+ 2000 ms kernel boot delay vs default 500)"); break;
    }
    /* Pick kernel from registry: KUSE selection takes priority, else first
     * public T42 entry. Fall back to legacy KERNEL_T42_KERNEL if registry
     * is empty for some reason (shouldn't happen in practice). */
    const kernel_entry_t *kern = kernel_selected();
    if (!kern || strcmp(kern->target, "T42") != 0) {
        kern = kernel_find_default("T42");
    }
    if (kern) {
        Serial.print("T42READ: kernel=");
        Serial.print(kern->id);
        Serial.print(" (");
        Serial.print(kern->display_name ? kern->display_name : "");
        Serial.print(", ");
        Serial.print(kern->src == KERNEL_SRC_PRIVATE ? "private" : "public");
        Serial.println(")");
    } else {
        Serial.println("T42READ: (no registry kernel; using legacy hardcoded)");
    }

    /* Force T42 IDs + algo. cmd_algo re-inits ISO-TP link if IDs change. */
    cmd_algo("t42");

    /* $1A B4 cal-ID read — GM T42 bootloader handshake. Captured
     * commercial flash tools send this (or similar $1A queries) before
     * $27 on every successful capture. */
    Serial.println("T42READ: $1A B4 handshake");
    {
        uds_msg_t probe_resp;
        uint8_t p[2] = { 0x1A, 0xB4 };
        int r = uds_request(&g_isotp_link, p, 2, &probe_resp, 1500);
        if (r == UDS_OK) {
            Serial.print("  cal-ID probe OK, data_len=");
            Serial.println(probe_resp.data_len);
        } else {
            Serial.print("  cal-ID probe ret="); Serial.println(r);
            Serial.println("  (continuing; some bench states reject this)");
        }
    }

    /* Variant B: full programming-mode sequence (captured commercial-tool style).
     * Sends $10 02 DiagSession and $A2 ProgrammingMode BEFORE $27 01,
     * in addition to the standard $28/$A5 01/$A5 03. Matches captured
     * order from one family of commercial tools; another family (and
     * Flashy default) skips these. */
    if (variant == 'B') {
        Serial.println("T42READ: [B] $10 02 programmingSession (0x101 broadcast)");
        {
            uint8_t b[8] = { 0xFE, 0x02, 0x10, 0x02, 0, 0, 0, 0 };
            can_send(0x101, b, 8);
        }
        delay(130);
    }

    /* $28 00 disableNormalCommunication — broadcast on 0x101.
     * Silences normal-mode chatter from all modules so diag traffic
     * has the bus. REQUIRED before $34 on GM bootloaders (NRC 0x22
     * observed on T42 when skipped). */
    Serial.println("T42READ: $28 disableNormalCommunication (0x101 broadcast)");
    {
        uint8_t b[8] = { 0xFE, 0x01, 0x28, 0x00, 0, 0, 0, 0 };
        can_send(0x101, b, 8);
    }
    delay(50);

    /* Variant B: $A2 ProgrammingMode request between $28 and $27 */
    if (variant == 'B') {
        Serial.println("T42READ: [B] $A2 ProgrammingMode request (0x101 broadcast)");
        {
            uint8_t b[8] = { 0xFE, 0x01, 0xA2, 0x00, 0, 0, 0, 0 };
            can_send(0x101, b, 8);
        }
        delay(100);
    }

    /* $27 Security Access (2-byte seed, algo 371) */
    Serial.println("T42READ: $27 SecurityAccess");
    led_auth();
    if (!do_security_access()) {
        led_error();
        print_err("T42READ: security access denied");
        return;
    }

    /* $A5 01 requestProgrammingMode — broadcast on 0x101.
     * Captured commercial tools send this ~7 ms after $27 02 unlock response. */
    Serial.println("T42READ: $A5 01 requestProgrammingMode (0x101 broadcast)");
    {
        uint8_t b[8] = { 0xFE, 0x02, 0xA5, 0x01, 0, 0, 0, 0 };
        can_send(0x101, b, 8);
    }
    delay(280);   /* Captured timing: ~280 ms between $A5 01 and $A5 03 */

    /* $A5 03 enableProgrammingMode — broadcast on 0x101.
     * After this the module is primed to accept $34. */
    Serial.println("T42READ: $A5 03 enableProgrammingMode (0x101 broadcast)");
    {
        uint8_t b[8] = { 0xFE, 0x02, 0xA5, 0x03, 0, 0, 0, 0 };
        can_send(0x101, b, 8);
    }

    /* Variant C: sustained 100 ms $3E heartbeat from $A5 03 through $34.
     * Target: ~230 ms gap from $A5 03 → $34 = 2 heartbeats. */
    if (variant == 'C') {
        Serial.println("T42READ: [C] sustained 100 ms $3E heartbeat (pre-$34)");
        for (int i = 0; i < 2; i++) {
            delay(100);
            t42_send_3e_bc();
        }
        delay(30);
    } else {
        delay(230);   /* Captured timing: ~230 ms between $A5 03 and $34 */
    }

    /* Resolve kernel blob/size/addr from registry entry (or legacy fallback). */
    const uint8_t *use_blob = kern ? kern->blob       : KERNEL_T42_KERNEL;
    uint32_t       use_size = kern ? kern->blob_size  : (uint32_t)KERNEL_T42_KERNEL_SIZE;
    uint32_t       use_addr = kern ? kern->load_addr  : 0x00FF8B40u;

    /* $34 00 <size3> — 3-byte SIZE format (confirmed working on T42). */
    Serial.print("T42READ: $34 RequestDownload SIZE=");
    Serial.println(use_size);
    uds_msg_t resp;
    {
        uint32_t sz = use_size;
        uint8_t rd_req[5] = {
            0x34, 0x00,
            (uint8_t)(sz >> 16),
            (uint8_t)(sz >> 8),
            (uint8_t)(sz)
        };
        int ret = uds_request(&g_isotp_link, rd_req, sizeof(rd_req), &resp,
                              UDS_PENDING_TIMEOUT_MS);
        if (ret != UDS_OK) {
            led_error();
            Serial.print("ERR: $34 failed ret="); Serial.println(ret);
            if (ret == UDS_ERR_NEGATIVE) {
                Serial.print("  NRC=0x"); Serial.println(resp.nrc, HEX);
            }
            return;
        }
        Serial.println("  $34 accepted");
    }

    /* $36 80 <addr4> <kernel> — TransferData. addr4 from registry entry. */
    Serial.print("T42READ: $36 TransferData to 0x");
    Serial.print(use_addr, HEX);
    Serial.print(", size=");
    Serial.print(use_size); Serial.println(" bytes");
    {
        /* ISO-TP caps a single multi-frame payload at 4095 bytes, so the
         * kernel + 6-byte $36 header must fit that. Static buffer sized
         * to that upper bound. */
        static uint8_t td_buf[6 + 4089];
        if (use_size > 4089) {
            Serial.print("ERR: kernel too big for ISO-TP MF ("); Serial.print(use_size);
            Serial.println(" > 4089 bytes)");
            led_error();
            return;
        }
        td_buf[0] = 0x36;
        td_buf[1] = 0x80;
        td_buf[2] = (uint8_t)((use_addr >> 24) & 0xFF);
        td_buf[3] = (uint8_t)((use_addr >> 16) & 0xFF);
        td_buf[4] = (uint8_t)((use_addr >>  8) & 0xFF);
        td_buf[5] = (uint8_t)( use_addr        & 0xFF);
        memcpy(&td_buf[6], use_blob, use_size);
        uint16_t td_len = (uint16_t)(6 + use_size);
        /* Short timeout — GM bootloaders often JUMP to the loaded code on
         * $36 completion without sending a $76 ACK (same pattern as E92's
         * $36 80 downloadAndExecute). A timeout here isn't fatal; we'll
         * proceed and probe for kernel aliveness via $1A 55 ping. */
        int ret = uds_request(&g_isotp_link, td_buf, td_len, &resp, 2000);
        if (ret == UDS_OK) {
            Serial.println("  $36 accepted (got $76 ACK)");
        } else if (ret == UDS_ERR_NEGATIVE) {
            led_error();
            Serial.print("ERR: $36 got NRC=0x"); Serial.println(resp.nrc, HEX);
            return;
        } else {
            Serial.print("  $36 no-ACK (ret="); Serial.print(ret);
            Serial.println(") — assuming bootloader jumped to kernel");
        }
    }

    /* $37 TransferExit REMOVED in Day3-40.
     * The T42 bootloader doesn't support $37 (always NRC 0x11). Sending
     * it after $36 may interrupt the pending auto-jump. The captured
     * commercial flow skips $37 and the kernel boots fine. Matching
     * that flow here. */

    /* Post-$36 TesterPresent broadcast — the timing-map agent flagged
     * this as a concrete differentiator: captured commercial tools
     * emit EXACTLY ONE $3E broadcast in the 1-second window after the
     * last $36 CF lands. Flashy emits zero. This may be what the
     * bootloader or kernel expects immediately after the jump. One
     * shot on 0x101 right after upload, before the probe. */
    if (_eff_send_3e()) {
        Serial.println("T42READ: $3E TesterPresent (0x101 broadcast, post-upload)");
        t42_send_3e_bc();
    } else {
        Serial.println("T42READ: $3E post-upload suppressed by SET SEND_3E_AFTER 0");
    }

    /* Variant C: sustained 100 ms $3E heartbeat during kernel-boot window.
     * ~10 more heartbeats across the boot delay window. */
    if (variant == 'C') {
        Serial.println("T42READ: [C] sustained 100 ms $3E heartbeat during boot (500 ms window)");
        for (int i = 0; i < 5; i++) {
            delay(100);
            t42_send_3e_bc();
        }
    } else if (variant == 'E') {
        /* Variant E: extended 2000 ms kernel boot delay (default 500).
         * Hypothesis: kernel's init takes longer than 500 ms and we
         * probe too early. Give it 4x more time. */
        Serial.println("T42READ: [E] extended 2000 ms kernel boot delay");
        delay(2000);
    } else {
        /* Default A/B/D: use effective boot delay (kernel meta or SET override).
         * If CADENCE_3E_MS is set, break the delay into $3E-heartbeat slices. */
        uint16_t boot_ms = _eff_boot_delay(kern);
        uint16_t cad = g_params.cadence_3e_ms;
        Serial.print("T42READ: waiting "); Serial.print(boot_ms);
        Serial.print(" ms for kernel boot");
        if (cad > 0) { Serial.print(" (cadence $3E every "); Serial.print(cad); Serial.print(" ms)"); }
        Serial.println();
        if (cad > 0 && cad < boot_ms) {
            uint16_t elapsed = 0;
            while (elapsed < boot_ms) {
                uint16_t step = (uint16_t)((boot_ms - elapsed) < cad ? (boot_ms - elapsed) : cad);
                delay(step);
                elapsed = (uint16_t)(elapsed + step);
                t42_send_3e_bc();
            }
        } else {
            delay(boot_ms);
        }
    }

    /* Kernel probe — variant-specific */
    if (variant == 'D') {
        /* Variant D: probe fan-out. Try five probes 300 ms apart,
         * log which one (if any) gets a response. */
        Serial.println("T42READ: [D] probe fan-out — 5 subfunctions, 300 ms apart");
        /* Covers commercial-tool / custom protocols we might not know about. */
        struct { const char *name; uint8_t req[8]; uint8_t len; } probes[] = {
            { "$1A BB",          { 0x1A, 0xBB, 0, 0, 0, 0, 0, 0 }, 2 },
            { "$1A 21",          { 0x1A, 0x21, 0, 0, 0, 0, 0, 0 }, 2 },
            { "$1A 55",          { 0x1A, 0x55, 0, 0, 0, 0, 0, 0 }, 2 },
            { "$35 01 00 00 00", { 0x35, 0x01, 0x00, 0x00, 0x00, 0, 0, 0 }, 5 },
            { "$3E",             { 0x3E, 0, 0, 0, 0, 0, 0, 0 }, 1 },
        };
        bool got_any = false;
        for (unsigned p = 0; p < sizeof(probes)/sizeof(probes[0]); p++) {
            Serial.print("  probing "); Serial.print(probes[p].name); Serial.print(" ... ");
            int ret = uds_request(&g_isotp_link, probes[p].req, probes[p].len,
                                  &resp, 300);
            if (ret == UDS_OK) {
                Serial.print("GOT RESPONSE: svc=0x"); Serial.print(resp.service, HEX);
                Serial.print(" sub=0x"); Serial.println(resp.sub_function, HEX);
                got_any = true;
            } else {
                Serial.print("no reply (ret="); Serial.print(ret);
                if (ret == UDS_ERR_NEGATIVE) {
                    Serial.print(" NRC=0x"); Serial.print(resp.nrc, HEX);
                }
                Serial.println(")");
            }
            delay(50);
        }
        if (got_any) {
            led_connected();
            Serial.println("=== T42 KERNEL ALIVE — one of the probes got a reply ===");
            Serial.println("T42READ: power-cycle to restore OS");
        } else {
            led_error();
            Serial.println("T42READ: [D] all 5 probes silent — kernel not responding");
        }
        return;
    }

    /* Variants A/B/C/E: single probe using effective svc/pid.
     * If PROBE_SVC is 0 (either via kernel meta with probe.service=null, or
     * SET PROBE_SVC 0 override), skip the probe entirely — upload succeeded,
     * proof-of-life must come from an external capture. This lets reference
     * kernels that don't implement our FLSHY handshake still be bench-tested. */
    uint8_t probe_svc = _eff_probe_svc(kern);
    uint8_t probe_pid = _eff_probe_pid(kern);
    if (probe_svc == 0) {
        led_connected();
        Serial.println("T42READ: probe suppressed (PROBE_SVC=0) — upload complete, no handshake attempted");
        Serial.println("T42READ: verify kernel-alive externally (SavvyCAN), then power-cycle to restore OS");
        return;
    }

    Serial.print("T42READ: $");
    if (probe_svc < 0x10) Serial.print('0');
    Serial.print(probe_svc, HEX);
    Serial.print(' ');
    if (probe_pid < 0x10) Serial.print('0');
    Serial.print(probe_pid, HEX);
    Serial.println(" kernel probe");
    {
        uint8_t ping_req[2] = { probe_svc, probe_pid };
        int ret = uds_request(&g_isotp_link, ping_req, sizeof(ping_req), &resp,
                              2000);
        if (ret != UDS_OK) {
            led_error();
            Serial.print("ERR: probe no response ret="); Serial.println(ret);
            if (ret == UDS_ERR_NEGATIVE) {
                Serial.print("  NRC=0x"); Serial.println(resp.nrc, HEX);
            }
            Serial.println("T42READ: kernel did not respond. Possible causes:");
            Serial.println("  - wrong probe svc/pid for this kernel");
            Serial.println("    (try SET PROBE_SVC 0 to skip and verify via SavvyCAN)");
            Serial.println("  - load_addr wrong for this kernel variant");
            Serial.println("  - boot delay too short (try SET BOOT_DELAY_MS 2000)");
            Serial.println("  (power-cycle TCM to restore OS)");
            return;
        }

        /* resp.service = probe_svc|0x40, resp.sub_function = (echo), data = payload */
        Serial.print("  resp: svc=0x"); Serial.print(resp.service, HEX);
        Serial.print(" sub=0x"); Serial.print(resp.sub_function, HEX);
        Serial.print(" data=");
        for (uint16_t i = 0; i < resp.data_len && i < 8; i++) {
            if (resp.data[i] >= ' ' && resp.data[i] <= '~') {
                Serial.print((char)resp.data[i]);
            } else {
                if (resp.data[i] < 0x10) Serial.print('0');
                Serial.print(resp.data[i], HEX);
            }
            Serial.print(' ');
        }
        Serial.println();

        /* Any positive (svc|0x40) response = kernel alive. Signature check
         * comes from kernel meta (expected_sig); default clean-room kernel
         * uses "FLSHY". If meta has no sig, any positive response counts. */
        if (resp.service == (uint8_t)(probe_svc | 0x40)) {
            led_connected();
            Serial.println("=== T42 KERNEL ALIVE — positive response ===");
            const char *expect = kern ? kern->expected_sig : nullptr;
            if (expect && *expect) {
                size_t elen = strlen(expect);
                bool match = (resp.data_len >= elen);
                for (size_t i = 0; match && i < elen; i++) {
                    if ((char)resp.data[i] != expect[i]) match = false;
                }
                if (match) {
                    Serial.print("  signature confirmed: "); Serial.println(expect);
                } else {
                    Serial.print("  (signature '"); Serial.print(expect);
                    Serial.println("' not matched — inspect bytes above)");
                }
            } else {
                Serial.println("  (no expected_sig in kernel meta — inspect bytes above)");
            }
            Serial.println("T42READ MVP complete. Starting sequential flash dump:");

            /* === INLINE FLASH DUMP via repeat probes === */
            for (uint32_t k = 0; k < 20; k++) {
                uint8_t probe2[2] = { probe_svc, probe_pid };
                uds_msg_t resp2;
                int ret = uds_request(&g_isotp_link, probe2, 2, &resp2, 2000);
                if (ret != UDS_OK) {
                    Serial.print("  dump probe "); Serial.print(k);
                    Serial.print(" failed ret="); Serial.println(ret);
                    break;
                }
                Serial.print("  [0x");
                Serial.print(k * 5, HEX); Serial.print("]:");
                for (uint8_t j = 0; j < 5 && j < resp2.data_len; j++) {
                    Serial.print(' ');
                    if (resp2.data[j] < 0x10) Serial.print('0');
                    Serial.print(resp2.data[j], HEX);
                }
                Serial.println();
                delay(20);
            }

            Serial.println("T42READ: TCM currently running kernel — power-cycle to restore OS");
        } else {
            Serial.println("T42READ: unexpected response service");
        }
    }
}
#else
static void cmd_t42_read(const char * /*arg*/)
{
    print_err("T42READ unavailable — kernel_t42_private.h not present");
}
#endif /* KERNEL_T42_AVAILABLE */

/* === T42RA_READ — dump memory/flash via ref_a kernel's $A0 opcode ========
 * Kernel must already be running (run `KUSE t42_ref_a` + `T42READ A` first —
 * ref_a's auto-jump leaves the kernel resident). This command never uploads;
 * it only drives the running kernel.
 *
 * Wire protocol (bench-decoded 2026-04-24, see memory/t42_ref_a_protocol.md):
 *   TX (0x7E2, raw 8B, NO ISO-TP wrap):
 *     A0 00 <addr:4 big-endian> <size:2 big-endian>
 *   RX (0x7EA, raw 8B frames, NO ISO-TP PCI):
 *     ceil(size/8) frames, 8 bytes of memory each.
 *
 * Usage: T42RA_READ [start_hex [size_hex]]   defaults 0x00000000 0x100 (256 B)
 */
static int t42ra_recv_frame(uint8_t *out, uint32_t timeout_ms)
{
    /* Same idea as extkern_recv_any but with NO broadcast-TP side effects —
     * the reference (extracted) kernel re-TXs its ASCII tag on any 0x101
     * activity, which pollutes the bus during a read. Pure passive RX,
     * filter to g_ecu_id. */
    uint32_t deadline = millis() + timeout_ms;
    while ((int32_t)(millis() - deadline) < 0) {
        uint32_t rx_id;
        uint8_t  rx_data[8];
        uint8_t  rx_len;
        if (can_receive(&rx_id, rx_data, &rx_len) == 0) {
            if (rx_id == g_ecu_id) {
                memcpy(out, rx_data, rx_len);
                return rx_len;
            }
            /* ignore non-ECU bus chatter */
        }
    }
    return -1;
}

static void cmd_t42_ra_read(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }

    uint32_t start = 0x00000000;
    uint32_t size  = 0x00000100;   /* 256 bytes default */
    if (arg && *arg) {
        const char *p = arg;
        start = (uint32_t)strtoul(p, (char**)&p, 16);
        while (*p == ' ') p++;
        if (*p) size = (uint32_t)strtoul(p, NULL, 16);
    }
    /* Cap to 16 MB to avoid runaway args. T42 flash is 1 MB; this is
     * defensive, not a hard spec. */
    if (size > 0x01000000) size = 0x01000000;

    const uint32_t CHUNK = 256;   /* bytes per $A0 call — kernel accepts
                                     larger, but 256 keeps error recovery
                                     granular and fits the 16-bit size field */

    Serial.print("T42RA_READ: start=0x");
    Serial.print(start, HEX);
    Serial.print(" size=0x");
    Serial.print(size, HEX);
    Serial.print(" (");
    Serial.print(size); Serial.println(" bytes)");
    Serial.print("T42RA_READ: tester=0x"); Serial.print(g_tester_id, HEX);
    Serial.print(" ecu=0x"); Serial.println(g_ecu_id, HEX);

    /* Open SD output file under /Read/. Name encodes addr + size + timestamp.
     * mkdir is a no-op if the dir exists; only warn on a real failure. */
    char fname[80];
    fname[0] = '\0';
    File sd_file;
    bool sd_open = false;
    if (g_sd_ok) {
        if (!g_sd.exists("/Read") && !g_sd.mkdir("/Read")) {
            Serial.println("WARN: /Read/ create failed (write-protect or full?) — file save will fail");
        }
        char ts[20];
        format_timestamp(ts, sizeof(ts));
        snprintf(fname, sizeof(fname),
                 "/Read/T42RA_%08lX_%06lX_%s.bin",
                 (unsigned long)start, (unsigned long)size, ts);
        sd_file = g_sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
        if (sd_file) {
            sd_open = true;
            Serial.print("T42RA_READ: SD out = "); Serial.println(fname);
        } else {
            Serial.println("T42RA_READ: warn — SD open failed; Serial-only");
        }
    }

    g_reading_active = true;
    led_reading();
    uint32_t done = 0;
    uint32_t start_ms = millis();
    bool aborted = false;

    while (done < size) {
        uint32_t chunk = (size - done < CHUNK) ? (size - done) : CHUNK;
        uint32_t addr  = start + done;

        /* Build raw 8-byte $A0 request */
        uint8_t req[8];
        req[0] = 0xA0;
        req[1] = 0x00;
        req[2] = (uint8_t)(addr >> 24);
        req[3] = (uint8_t)(addr >> 16);
        req[4] = (uint8_t)(addr >>  8);
        req[5] = (uint8_t)(addr      );
        req[6] = (uint8_t)(chunk >> 8);
        req[7] = (uint8_t)(chunk     );

        /* Clear stale inbound frames (heartbeat/broadcast etc.) */
        extkern_drain_rx();

        if (can_send(g_tester_id, req, 8) != 0) {
            Serial.println("T42RA_READ: can_send failed");
            led_error();
            aborted = true;
            break;
        }

        /* Drain ceil(chunk/8) inbound frames */
        uint32_t frames_needed = (chunk + 7) / 8;
        uint8_t  chunk_buf[260]; /* 256 + slack */
        uint32_t received = 0;
        for (uint32_t f = 0; f < frames_needed; f++) {
            uint8_t rx[8];
            int rlen = t42ra_recv_frame(rx, 1500);
            if (rlen <= 0) {
                Serial.print("T42RA_READ: RX timeout at 0x");
                Serial.print(addr + received, HEX);
                Serial.print(" (got "); Serial.print(f);
                Serial.print("/"); Serial.print(frames_needed);
                Serial.println(" frames)");
                led_error();
                aborted = true;
                break;
            }
            uint32_t copy = (uint32_t)rlen;
            if (received + copy > chunk) copy = chunk - received;
            memcpy(chunk_buf + received, rx, copy);
            received += copy;
        }
        if (aborted) break;

        /* First-chunk visual check on Serial */
        if (done == 0) {
            Serial.print("T42RA_READ: first 16B @0x");
            Serial.print(addr, HEX); Serial.print(":");
            for (uint32_t i = 0; i < received && i < 16; i++) {
                Serial.print(' ');
                if (chunk_buf[i] < 0x10) Serial.print('0');
                Serial.print(chunk_buf[i], HEX);
            }
            Serial.println();
        }

        if (sd_open) {
            sd_file.write(chunk_buf, received);
        }

        done += received;

        /* Progress every 4 KB */
        if ((done & 0xFFF) == 0 || done == size) {
            uint32_t elapsed = millis() - start_ms;
            uint32_t bps = (elapsed > 0) ? (done * 1000UL / elapsed) : 0;
            Serial.print("T42RA_READ:PROGRESS ");
            Serial.print(done); Serial.print("/"); Serial.print(size);
            Serial.print(" B  "); Serial.print(bps); Serial.println(" B/s");
        }
    }

    if (sd_open) sd_file.close();
    g_reading_active = false;

    uint32_t total_ms = millis() - start_ms;
    Serial.print("T42RA_READ: ");
    Serial.print(aborted ? "ABORTED" : "done");
    Serial.print(" — "); Serial.print(done);
    Serial.print(" bytes in "); Serial.print(total_ms);
    Serial.print(" ms");
    if (total_ms > 0) {
        Serial.print("  ("); Serial.print(done * 1000UL / total_ms);
        Serial.print(" B/s)");
    }
    Serial.println();
    if (sd_open && done > 0) {
        Serial.print("T42RA_READ: file saved: "); Serial.println(fname);
    }
    if (aborted) { print_err("T42RA_READ failed"); }
    else         { led_connected(); print_ok(); }
}

/* === T42DUMP — loop $1A BB probes, collect 5 flash bytes per response ===
 * Kernel tracks read_addr and returns next 5 bytes on each probe. We send
 * N probes and log bytes to serial + save to SD (if filename given).
 *
 * Usage: T42DUMP [count]   (default 100 probes = 500 bytes)
 */
static void cmd_t42_dump(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }

    uint32_t count = 100;
    if (arg && *arg) {
        count = strtoul(arg, nullptr, 0);
        if (count == 0) count = 100;
        if (count > 200000) count = 200000;
    }

    Serial.print("T42DUMP: sending ");
    Serial.print(count); Serial.println(" probes, 5 bytes each");

    uint32_t got = 0;
    uds_msg_t resp;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t probe[2] = { 0x1A, 0xBB };
        int ret = uds_request(&g_isotp_link, probe, 2, &resp, 2000);
        if (ret != UDS_OK) {
            Serial.print("  probe "); Serial.print(i);
            Serial.print(" failed ret="); Serial.println(ret);
            break;
        }
        /* uds_msg_t stores payload AFTER SID+sub in data[], length in data_len.
         * So for reply `07 5A BB X Y Z W V`: service=5A, sub_function=BB,
         * data=[X,Y,Z,W,V], data_len=5 */
        if (resp.data_len < 5) continue;
        Serial.print("  [0x");
        Serial.print(i * 5, HEX); Serial.print("]: ");
        for (int k = 0; k < 5; k++) {
            if (resp.data[k] < 0x10) Serial.print("0");
            Serial.print(resp.data[k], HEX); Serial.print(" ");
        }
        Serial.println();
        got += 5;
        if ((i & 0x3) == 0) delay(10);  /* slow down a touch */
    }
    Serial.print("T42DUMP: collected ");
    Serial.print(got); Serial.println(" bytes");
}

/* === T42USB — Upload an extracted commercial-tool kernel using its protocol ===
 * Reference test: if the extracted kernel responds to its $87 / ASCII-tag
 * probe when uploaded via Flashy, our auth/upload pipeline is correct and
 * the only bug is in our clean-room kernel's TouCAN init.
 *
 * Captured commercial-tool flow (2026-04-24, Test-Day3 capture):
 *   1. $28 broadcast (disableNormalComm)
 *   2. $A2 broadcast (ProgrammingMode) — positive $E2 00
 *   3. $A5 01 broadcast
 *   4. $A5 03 broadcast
 *   5. $3E $3E heartbeats
 *   6. $34 00 <size4=0x5AC>      (size, not addr)
 *   7. $36 80 <addr4=0xFF8B40> <kernel data>
 *   8. Kernel auto-boots, emits ASCII tag at 0x7EA
 *   9. NO $37, NO $27 auth
 *
 * The extracted-kernel header is user-supplied and gitignored. When the
 * header is absent the command is omitted from the build. */
#if __has_include("kernel_t42_usbjtag.h")
#include "kernel_t42_usbjtag.h"
#define KERNEL_T42_USBJTAG_AVAILABLE 1
#else
#define KERNEL_T42_USBJTAG_AVAILABLE 0
#endif

#if KERNEL_T42_USBJTAG_AVAILABLE
static void cmd_t42_usb(const char * /*arg*/)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }

    led_writing();
    uds_msg_t resp;
    int ret;

    Serial.println("T42USB: uploading extracted commercial-tool kernel via its protocol");
    Serial.print("  Kernel size = ");
    Serial.print(KERNEL_T42_USBJTAG_SIZE);
    Serial.println(" bytes, load = 0xFF8B40");

    /* Step 1: $28 disableNormalCommunication broadcast */
    Serial.println("T42USB: $28 broadcast");
    { uint8_t b[8] = { 0xFE, 0x01, 0x28, 0, 0, 0, 0, 0 }; can_send(0x101, b, 8); }
    delay(100);

    /* Step 2: $A2 ProgrammingMode broadcast (captured tool sends this) */
    Serial.println("T42USB: $A2 broadcast (ProgrammingMode)");
    { uint8_t b[8] = { 0xFE, 0x01, 0xA2, 0, 0, 0, 0, 0 }; can_send(0x101, b, 8); }
    delay(100);

    /* $27 Security Access (Flashy needs it; captured trace was mid-session) */
    Serial.println("T42USB: $27 SecurityAccess");
    led_auth();
    if (!do_security_access()) {
        led_error();
        Serial.println("T42USB: security access denied");
        return;
    }
    led_writing();

    /* Step 3-4: $A5 01 + $A5 03 */
    Serial.println("T42USB: $A5 01 broadcast");
    { uint8_t b[8] = { 0xFE, 0x02, 0xA5, 0x01, 0, 0, 0, 0 }; can_send(0x101, b, 8); }
    delay(280);
    Serial.println("T42USB: $A5 03 broadcast");
    { uint8_t b[8] = { 0xFE, 0x02, 0xA5, 0x03, 0, 0, 0, 0 }; can_send(0x101, b, 8); }
    delay(100);

    /* Step 5: $3E heartbeats */
    Serial.println("T42USB: $3E x2");
    { uint8_t b[8] = { 0xFE, 0x01, 0x3E, 0, 0, 0, 0, 0 }; can_send(0x101, b, 8); }
    delay(100);
    { uint8_t b[8] = { 0xFE, 0x01, 0x3E, 0, 0, 0, 0, 0 }; can_send(0x101, b, 8); }
    delay(100);

    /* Step 6: $34 with 3-byte SIZE — captured format: 34 00 <sz3>
     * Trace: 05 34 00 00 05 AC (ISO-TP SF len=5, 5-byte payload) */
    Serial.println("T42USB: $34 RequestDownload (3-byte SIZE format)");
    {
        uint32_t sz = KERNEL_T42_USBJTAG_SIZE;
        uint8_t rd[5] = {
            0x34, 0x00,
            (uint8_t)(sz >> 16),
            (uint8_t)(sz >> 8),
            (uint8_t)(sz)
        };
        ret = uds_request(&g_isotp_link, rd, 5, &resp, 3000);
        if (ret == UDS_OK)          Serial.println("  $34 accepted");
        else if (ret == UDS_ERR_NEGATIVE) { Serial.print("  $34 NRC=0x"); Serial.println(resp.nrc, HEX); return; }
        else                        { Serial.print("  $34 no-ACK ret="); Serial.println(ret); return; }
    }

    /* Step 7: $36 with ADDRESS (0xFF8B40) + kernel payload */
    Serial.println("T42USB: $36 TransferData (ADDRESS format, 0xFF8B40)");
    {
        static uint8_t td_buf[6 + KERNEL_T42_USBJTAG_SIZE];
        td_buf[0] = 0x36;
        td_buf[1] = 0x80;
        td_buf[2] = 0x00;
        td_buf[3] = 0xFF;
        td_buf[4] = 0x8B;
        td_buf[5] = 0x40;
        memcpy(&td_buf[6], KERNEL_T42_USBJTAG_KERNEL, KERNEL_T42_USBJTAG_SIZE);
        uint16_t td_len = (uint16_t)(6 + KERNEL_T42_USBJTAG_SIZE);
        ret = uds_request(&g_isotp_link, td_buf, td_len, &resp, 3000);
        if (ret == UDS_OK)          Serial.println("  $36 accepted");
        else if (ret == UDS_ERR_NEGATIVE) { Serial.print("  $36 NRC=0x"); Serial.println(resp.nrc, HEX); }
        else                        { Serial.print("  $36 no-ACK ret="); Serial.print(ret); Serial.println(" — expected if auto-jump"); }
    }

    /* Step 8: Listen for the kernel's ASCII tag on 0x7EA (3s window).
     * The captured kernel emits a 7-char ASCII signature whose first
     * letter is 'U'. Match on that prefix only — broad enough to also
     * catch related variants. */
    Serial.println("T42USB: listening for ASCII signature (3s)...");
    uint32_t listen_end = millis() + 3000;
    bool saw_sig = false;
    while (millis() < listen_end) {
        uint32_t rx_id;
        uint8_t rx_data[8];
        uint8_t rx_dlc;
        if (can_receive(&rx_id, rx_data, &rx_dlc) == 0) {
            if (rx_id == g_ecu_id && rx_dlc == 8) {
                Serial.print("  RX 0x"); Serial.print(rx_id, HEX); Serial.print(":");
                for (int i = 0; i < 8; i++) { Serial.print(" "); if (rx_data[i]<0x10) Serial.print("0"); Serial.print(rx_data[i], HEX); }
                Serial.println();
                if (rx_data[0]=='U' && rx_data[1]=='S' && rx_data[2]=='B' && rx_data[3]=='J') {
                    saw_sig = true;
                    Serial.println("T42USB: extracted kernel ALIVE - pipeline confirmed working");
                    break;
                }
            }
        }
    }
    if (!saw_sig) Serial.println("T42USB: no ASCII signature in window");

    led_idle();
}
#endif /* KERNEL_T42_USBJTAG_AVAILABLE */

static void cmd_kernel(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (no_kernel_for_active_module()) return;

    led_writing();

    /* Select kernel params based on active module */
    const uint8_t *kernel_data;
    uint16_t       kernel_size;
    uint32_t       load_addr;
    uint8_t        rd_val[3];
    uint8_t        block_seq;
    bool           two_step;   /* T87: upload then execute separately; E38: single shot */

    /* Check if "write" argument was passed — selects flash tool write kernel */
    bool use_write_kernel = (arg && strncmp(arg, "write", 5) == 0);

    if (g_module == MODULE_E38N) {
        /* EXTKERN — read+write, no DRM, raw CAN post-kernel */
        kernel_data = E38N_KERNEL;
        kernel_size = E38N_KERNEL_SIZE;
        load_addr   = E38N_KERNEL_LOAD_ADDR;
        rd_val[0]   = E38N_RD_VALUE_0;
        rd_val[1]   = E38N_RD_VALUE_1;
        rd_val[2]   = E38N_RD_VALUE_2;
        block_seq   = E38N_BLOCK_SEQ;  /* 0x80 = downloadAndExecute */
        two_step    = false;
        Serial.println("KERNEL: E38 EXTKERN (single-shot, raw CAN)");
    } else if (g_module == MODULE_E38 && use_write_kernel) {
        /* flash tool E38a_v1.8C — supports read+write, two-step upload */
        kernel_data = E38W_KERNEL;
        kernel_size = E38W_KERNEL_SIZE;
        load_addr   = E38W_KERNEL_LOAD_ADDR;
        rd_val[0]   = E38W_RD_VALUE_0;
        rd_val[1]   = E38W_RD_VALUE_1;
        rd_val[2]   = E38W_RD_VALUE_2;
        block_seq   = E38W_BLOCK_SEQ;  /* 0x00 = upload only */
        two_step    = true;
        Serial.println("KERNEL: E38 flash tool write kernel (two-step)");
    } else if (g_module == MODULE_E67) {
        /* flash tool E67a_v1.4R — correct data, two-step kernel */
        kernel_data = E67_KERNEL;
        kernel_size = E67_KERNEL_SIZE;
        load_addr   = E67_KERNEL_LOAD_ADDR;
        rd_val[0]   = E67_RD_VALUE_0;
        rd_val[1]   = E67_RD_VALUE_1;
        rd_val[2]   = E67_RD_VALUE_2;
        block_seq   = E67_BLOCK_SEQ;
        two_step    = true;
        Serial.println("KERNEL: E67 flash tool read kernel (two-step)");
#if E92_KERNEL_AVAILABLE
    } else if (g_module == MODULE_E92) {
        /* E92 v2.0R read kernel — two-step ($36 00 upload, $36 80 execute),
         * declares 4 KB buffer in $34, loads at 0x40001000. */
        kernel_data = E92_KERNEL;
        kernel_size = E92_KERNEL_SIZE;
        load_addr   = E92_KERNEL_LOAD_ADDR;
        rd_val[0]   = E92_RD_VALUE_0;
        rd_val[1]   = E92_RD_VALUE_1;
        rd_val[2]   = E92_RD_VALUE_2;
        block_seq   = E92_BLOCK_SEQ;
        two_step    = true;
        Serial.println("KERNEL: E92 v2.0R read kernel (two-step)");
#endif
    } else if (g_module == MODULE_E38) {
        /* stock tool LS7 v0.43 — read-only kernel, single-shot */
        kernel_data = E38_KERNEL;
        kernel_size = E38_KERNEL_SIZE;
        load_addr   = E38_KERNEL_LOAD_ADDR;
        rd_val[0]   = E38_RD_VALUE_0;
        rd_val[1]   = E38_RD_VALUE_1;
        rd_val[2]   = E38_RD_VALUE_2;
        block_seq   = E38_BLOCK_SEQ;   /* 0x80 = downloadAndExecute */
        two_step    = false;
        Serial.println("KERNEL: E38 stock tool read kernel (single-shot)");
    } else if (is_t87_family() && use_write_kernel) {
        if (arg && strstr(arg, "full")) {
            kernel_data = T87W_FULL_KERNEL;
            kernel_size = T87W_FULL_KERNEL_SIZE;
            Serial.println(g_t87a_detected ? "KERNEL: T87A full-flash write" : "KERNEL: T87 flash tool full-flash write (two-step)");
        } else {
            kernel_data = T87W_CAL_KERNEL;
            kernel_size = T87W_CAL_KERNEL_SIZE;
            Serial.println(g_t87a_detected ? "KERNEL: T87A calflash write" : "KERNEL: T87 flash tool calflash write (two-step)");
        }
        if (g_t87a_detected) {
            load_addr   = 0x40028000UL;
            rd_val[0]   = 0x02;
            rd_val[1]   = 0x80;
            rd_val[2]   = 0x00;
            two_step    = false;
        } else {
            load_addr   = T87_KERNEL_LOAD_ADDR;  /* 0x4002B000 */
            rd_val[0]   = T87_RD_VALUE_0;
            rd_val[1]   = T87_RD_VALUE_1;
            rd_val[2]   = T87_RD_VALUE_2;
            two_step    = true;
        }
        block_seq   = T87_BLOCK_SEQ;         /* 0x00 — data-only upload */
    } else if (g_t87a_detected) {
#if KERNEL_T87A_AVAILABLE
        /* T87A: Flashy clean-room kernel (VLE, SPC564A80) */
        kernel_data = KERNEL_T87A_KERNEL;
        kernel_size = KERNEL_T87A_SIZE;
        load_addr   = KERNEL_T87A_KERNEL_LOAD_ADDR;  /* 0x40010000 */
        rd_val[0]   = 0x00;
        rd_val[1]   = (uint8_t)(kernel_size >> 8);
        rd_val[2]   = (uint8_t)(kernel_size & 0xFF);
        block_seq   = 0x80;
        two_step    = false;
        Serial.print("KERNEL: Flashy T87A (VLE, ");
        Serial.print(kernel_size); Serial.println(" bytes @ 0x40010000)");
#else
        /* Fallback: external kernel — raw CAN protocol, not UDS */
        kernel_data = T87A_EXTKERN_KERNEL;
        kernel_size = T87A_EXTKERN_KERNEL_SIZE;
        load_addr   = T87A_EXTKERN_LOAD_ADDR;
        rd_val[0]   = 0x00;
        rd_val[1]   = (uint8_t)(kernel_size >> 8);
        rd_val[2]   = (uint8_t)(kernel_size & 0xFF);
        block_seq   = 0x80;
        two_step    = false;
        Serial.println("KERNEL: T87A EXTKERN (0x40010000, $34 size-based, $36 80)");
#endif
    } else {
        /* Default: T87 read kernel — flash tool two-step */
        kernel_data = T87_KERNEL;
        kernel_size = T87_KERNEL_SIZE;
        load_addr   = T87_KERNEL_LOAD_ADDR;  /* 0x4002B000 */
        rd_val[0]   = T87_RD_VALUE_0;        /* 0x02 — address-based $34 */
        rd_val[1]   = T87_RD_VALUE_1;        /* 0xB0 */
        rd_val[2]   = T87_RD_VALUE_2;        /* 0x00 */
        block_seq   = T87_BLOCK_SEQ;         /* 0x00 — data-only upload */
        two_step    = true;
        Serial.println("KERNEL: T87 flash tool read (two-step)");
    }

    /* Runtime guard: public builds ship without proprietary kernels — the
     * stubs in gm_kernels.h / e67_kernel.h / t87a_kernel.h report size 0.
     * Detect this and bail with a clear message rather than uploading
     * zero bytes to the ECU. */
    if (kernel_size == 0) {
        led_error();
        Serial.println("ERR: no kernel embedded for this module");
        Serial.println("  This Flashy build doesn't include a kernel for the selected module.");
        Serial.println("  See CONTRIBUTING.md for how to supply your own kernel header,");
        Serial.println("  or pick a module that ships with a clean-room Kernel (e.g. E92).");
        return;
    }

    /* T87/T87A: enter programming mode (broadcast $10 02, $28, $A2, $A5 01, $A5 03)
     * Must be done AFTER SecurityAccess but BEFORE $34/$36 kernel upload.
     * The $A5 03 broadcast reboots the TCM into bootloader mode. */
    if (is_t87_family()) {
        t87_enter_programming_mode();
        delay(100);  /* minimal wait — bootloader exits fast (~2s window) */
    }

    /* Step 1: RequestDownload — GM format: 34 00 [3 bytes kernel size] */
    uint8_t rd_req[5] = { UDS_SID_REQUEST_DOWNLOAD, 0x00,
                          rd_val[0], rd_val[1], rd_val[2] };
    uds_msg_t resp;
    int ret = uds_request(&g_isotp_link, rd_req, sizeof(rd_req), &resp,
                          UDS_PENDING_TIMEOUT_MS);
    if (ret != UDS_OK) {
        led_error();
        Serial.print("ERR: RequestDownload failed ret=");
        Serial.println(ret);
        if (ret == UDS_ERR_NEGATIVE) {
            Serial.print("  NRC=0x");
            Serial.println(resp.nrc, HEX);
        }
        return;
    }
    Serial.println("RequestDownload accepted");

    /* Step 2: TransferData — 36 [block_seq] [load_addr:4] [kernel_data...] */
    /* Fixed 8 KB buffer accommodates any kernel we ship (largest is ~3 KB)
     * and stays valid even when public stubs report kernel_size = 0. */
    uint8_t td_buf[2 + 4 + 8192];
    td_buf[0] = UDS_SID_TRANSFER_DATA;
    td_buf[1] = block_seq;
    td_buf[2] = (uint8_t)(load_addr >> 24);
    td_buf[3] = (uint8_t)(load_addr >> 16);
    td_buf[4] = (uint8_t)(load_addr >> 8);
    td_buf[5] = (uint8_t)(load_addr);
    memcpy(&td_buf[6], kernel_data, kernel_size);

    uint16_t td_len = (uint16_t)(6 + kernel_size);

    /*
     * For E38 downloadAndExecute (block_seq 0x80), the kernel responds with
     * SID 0x99 (not the standard 0x76 positive response). uds_request() would
     * discard 0x99 as "unexpected SID" and timeout. So we send raw and accept
     * any response.
     */
    if (!two_step) {
        /* E38: raw send + wait for any response (0x99 = kernel ready) */
        ret = isotp_send(&g_isotp_link, td_buf, td_len);
        if (ret != ISOTP_RET_OK) {
            led_error();
            print_err("TransferData send failed");
            return;
        }
        Serial.print("Sending kernel (");
        Serial.print(kernel_size);
        Serial.println(" bytes)...");

        if (g_module == MODULE_E38N || g_t87a_detected) {
            /* EXTKERN or Flashy kernel on T87A / E38N:
             * isotp_send() is non-blocking for multi-frame.
             * Pump poll_can_rx() + isotp_poll() until ISO-TP transfer completes. */
            uint32_t send_deadline = millis() + 15000;
            while (g_isotp_link.send_status == ISOTP_SEND_STATUS_INPROGRESS &&
                   millis() < send_deadline) {
                poll_can_rx();
                isotp_poll(&g_isotp_link);
            }
            Serial.println("ISO-TP transfer complete");

#if KERNEL_T87A_AVAILABLE
            if (g_t87a_detected) {
                /* Flashy clean-room kernel: poll with $1A BB like E92. */
                delay(1000);
                Serial.println("  $1A BB poll...");
                uint8_t poll[8] = { 0x02, 0x1A, 0xBB, 0, 0, 0, 0, 0 };
                can_send(g_tester_id, poll, 8);
                if (kernel_wait_ack(3000)) {
                    Serial.println("=== KERNEL ALIVE (Flashy T87A) ===");
                } else {
                    Serial.println("WARN: no FLSHY ACK from T87A kernel");
                }
            } else
#endif
            {
                /* external kernel: switch to raw CAN mode, look for heartbeat */
                g_extkern_active = true;
                delay(1000);
                if (extkern_wait_heartbeat(5000)) {
                    Serial.println("Kernel response: kernel heartbeat");
                } else {
                    Serial.println("WARN: no kernel heartbeat (may still be running)");
                }
            }
        } else {
            /* stock tool/other E38 kernels: wait for 0x99 response */
            ret = uds_receive(&g_isotp_link, &resp, 10000);
            if (ret == UDS_OK) {
                Serial.print("Kernel response: 0x");
                Serial.println(resp.service, HEX);
            } else if (ret == UDS_ERR_TIMEOUT) {
                Serial.println("WARN: no kernel response (may still be running)");
            } else {
                led_error();
                Serial.print("ERR: kernel response error ret=");
                Serial.println(ret);
                if (ret == UDS_ERR_NEGATIVE) {
                    Serial.print("  NRC=0x");
                    Serial.println(resp.nrc, HEX);
                }
                return;
            }
        }
    } else {
        /* T87: standard uds_request, expects 0x76 response */
        ret = uds_request(&g_isotp_link, td_buf, td_len, &resp, UDS_PENDING_TIMEOUT_MS);
        if (ret != UDS_OK) {
            led_error();
            Serial.print("ERR: TransferData failed ret=");
            Serial.println(ret);
            if (ret == UDS_ERR_NEGATIVE) {
                Serial.print("  NRC=0x");
                Serial.println(resp.nrc, HEX);
            }
            return;
        }
    }

    Serial.print("Kernel data sent (");
    Serial.print(kernel_size);
    Serial.println(" bytes)");

    /* Step 3 (two-step kernels): Execute kernel */
    if (two_step) {
        uint8_t exec_req[6] = {
            UDS_SID_TRANSFER_DATA, 0x80,
            (uint8_t)(load_addr >> 24), (uint8_t)(load_addr >> 16),
            (uint8_t)(load_addr >> 8),  (uint8_t)(load_addr)
        };

        if (is_t87_family()) {
            /* T87/T87A/T93: Cascading execute strategy
             * Newer T87A/T93 bootloaders (5-byte security) may not support $36 80.
             * Try: $36 80 → $37 TransferExit → $31 01 FF 00 RoutineControl */
            bool execute_ok = false;

            /* Strategy 1: $36 80 (flash tool/EXTKERN standard) */
            Serial.println("Exec: trying $36 80...");
            ret = uds_request(&g_isotp_link, exec_req, 6, &resp, 3000);
            if (ret == UDS_OK) {
                Serial.println("$36 80 accepted");
                execute_ok = true;
            } else if (ret == UDS_ERR_TIMEOUT) {
                /* Timeout = kernel started running (no UDS response from running kernel) */
                Serial.println("$36 80 timeout (kernel may be running)");
                execute_ok = true;
            } else if (ret == UDS_ERR_NEGATIVE && resp.nrc == 0x12) {
                /* NRC 0x12 = subFunctionNotSupported — try next strategy */
                Serial.println("$36 80 NRC 0x12 (not supported)");

                /* Strategy 2: $37 TransferExit */
                Serial.println("Exec: trying $37 TransferExit...");
                uint8_t te_req[1] = { UDS_SID_TRANSFER_EXIT };
                ret = uds_request(&g_isotp_link, te_req, 1, &resp, 3000);
                if (ret == UDS_OK) {
                    Serial.println("$37 accepted — kernel should be running");
                    execute_ok = true;
                } else if (ret == UDS_ERR_TIMEOUT) {
                    Serial.println("$37 timeout (kernel may be running)");
                    execute_ok = true;
                } else {
                    if (ret == UDS_ERR_NEGATIVE) {
                        Serial.print("$37 NRC=0x");
                        Serial.println(resp.nrc, HEX);
                    }
                    /* Strategy 3: $31 01 FF 00 RoutineControl */
                    Serial.println("Exec: trying $31 01 FF 00...");
                    uint8_t rc_req[4] = { 0x31, 0x01, 0xFF, 0x00 };
                    ret = uds_request(&g_isotp_link, rc_req, 4, &resp, 3000);
                    if (ret == UDS_OK || ret == UDS_ERR_TIMEOUT) {
                        Serial.println("$31 01 FF 00 sent — kernel may be running");
                        execute_ok = true;
                    } else {
                        if (ret == UDS_ERR_NEGATIVE) {
                            Serial.print("$31 NRC=0x");
                            Serial.println(resp.nrc, HEX);
                        }
                    }
                }
            } else {
                Serial.print("$36 80 failed ret=");
                Serial.println(ret);
                if (ret == UDS_ERR_NEGATIVE) {
                    Serial.print("  NRC=0x");
                    Serial.println(resp.nrc, HEX);
                }
            }

            if (!execute_ok) {
                led_error();
                print_err("all kernel execute strategies failed");
                return;
            }
            Serial.println("Execute sent, waiting for kernel...");
        } else {
            /* E38: raw send + receive (kernel may respond with non-standard SID) */
            ret = isotp_send(&g_isotp_link, exec_req, sizeof(exec_req));
            if (ret != ISOTP_RET_OK) {
                led_error();
                print_err("Execute send failed");
                return;
            }
            Serial.println("Execute sent, waiting for kernel...");
            ret = uds_receive(&g_isotp_link, &resp, 10000);
            if (ret == UDS_OK) {
                Serial.print("Kernel response: 0x");
                Serial.println(resp.service, HEX);
            } else if (ret == UDS_ERR_TIMEOUT) {
                Serial.println("WARN: no execute response (kernel may be running)");
            } else {
                led_error();
                Serial.print("ERR: Execute kernel failed ret=");
                Serial.println(ret);
                if (ret == UDS_ERR_NEGATIVE) {
                    Serial.print("  NRC=0x");
                    Serial.println(resp.nrc, HEX);
                }
                return;
            }
        }
    }

    /* Auto-enable 0x0101 bus TP to keep nodes alive post-kernel */
    g_auto_broadcast = true;
    send_broadcast_tp();

    /* E38N: activate external-kernel raw CAN mode (stops poll_can_rx from eating frames) */
    if (g_module == MODULE_E38N) {
        g_extkern_active = true;
    }

    led_connected();
    Serial.println("Kernel running");
    Serial.println("BusTP (0x0101) auto-enabled");
    print_ok();
}

/* Forward declarations for functions defined later */
static int  hex_line_to_block(uint8_t *block_buf, uint32_t *addr, uint32_t timeout_ms);
static void print_addr_hex6(uint32_t addr);

/* Check if seed is all zeros (already unlocked) — works for 2-byte and 5-byte seeds */
static bool seed_is_all_zero(const uint8_t *seed, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        if (seed[i] != 0) return false;
    }
    return len > 0;
}

/* Perform full SecurityAccess: request seed, compute key, send key.
 * Handles 2-byte (algo 569) and all-zero seeds (any length).
 * Returns true on success, false on failure (prints error). */
static bool do_security_access(void) {
    uint8_t  seed[32];
    uint16_t seed_len = 0;
    int ret = uds_security_access_seed(&g_isotp_link, 0x01, seed, &seed_len);
    if (ret != UDS_OK) { print_err("seed request failed"); return false; }
    Serial.print("SEED:");
    for (uint16_t i = 0; i < seed_len; i++) {
        if (seed[i] < 0x10) Serial.print('0');
        Serial.print(seed[i], HEX);
    }
    Serial.println();

    /* 5-byte seed = T87A bootloader — except E92A LATE, which also uses
     * 5-byte but with algo 146 not 135. Don't flag T87A in that case. */
    if (seed_len == 5 && g_module != MODULE_E92) {
        g_t87a_detected = true;
    }
    if (seed_is_all_zero(seed, seed_len)) {
        Serial.println(g_t87a_detected ? "Security: already unlocked (T87A, seed=0)" : "Security: already unlocked (seed=0)");
        return true;
    }
    if (seed_len == 2) {
        uint16_t seed16 = (uint16_t)((seed[0] << 8) | seed[1]);
        uint16_t key16  = seedkey_compute(seed16);
        uint8_t key[2] = { (uint8_t)(key16 >> 8), (uint8_t)(key16 & 0xFF) };
        Serial.print("KEY:  ");
        if (key[0] < 0x10) Serial.print('0');
        Serial.print(key[0], HEX);
        if (key[1] < 0x10) Serial.print('0');
        Serial.println(key[1], HEX);
        ret = uds_security_access_key(&g_isotp_link, 0x02, key, 2);
        if (ret != UDS_OK) { print_err("key rejected"); return false; }
        Serial.println("Security unlocked");
        return true;
    }
    if (seed_len == 5) {
        uint8_t key5[5];
        bool ok;
        const char *who;
        if (g_module == MODULE_E92 && g_e92_variant == E92_VARIANT_LATE) {
            ok  = gm5byte_compute_key_e92a(seed, key5);  /* algo 146 */
            who = "E92A";
        } else {
            ok  = gm5byte_compute_key(seed, key5);       /* algo 135 */
            who = "T87A";
        }
        if (!ok) {
            print_err("5-byte seed out of range");
            return false;
        }
        Serial.print("KEY:  ");
        for (int i = 0; i < 5; i++) {
            if (key5[i] < 0x10) Serial.print('0');
            Serial.print(key5[i], HEX);
        }
        Serial.println();
        ret = uds_security_access_key(&g_isotp_link, 0x02, key5, 5);
        if (ret != UDS_OK) { print_err("5-byte key rejected"); return false; }
        Serial.print("Security unlocked ("); Serial.print(who); Serial.println(")");
        return true;
    }
    print_err("unsupported seed length");
    return false;
}
static void t87_broadcast_reset(void);
static void t87_broadcast_disable_comms(void);
static void t87_enter_programming_mode(void);
static void t87_return_to_normal(void);
static bool t87_flash_erase(uint32_t timeout_ms);
static bool t87_flash_write_block(uint32_t addr, const uint8_t *data);
static bool t87_flash_finalize(void);

/* ============================================================
 * external-kernel raw CAN protocol helpers
 *
 * After kernel upload, all communication uses raw 8-byte CAN
 * frames on g_tester_id / g_ecu_id (typically 0x7E0 / 0x7E8).
 * No UDS, no ISO-TP — just plain CAN frames.
 * ============================================================ */

/* Wait for a raw CAN response from ECU on g_ecu_id.
 * Skips kernel heartbeat frames (55 53 42 4A 54 41 47).
 * Returns number of data bytes (0-8), or -1 on timeout. */
static int extkern_recv(uint8_t *out, uint32_t timeout_ms)
{
    uint32_t deadline = millis() + timeout_ms;
    while (millis() < deadline) {
        /* Keep broadcast TP alive — prevents other modules from
         * resuming normal comms and flooding the CAN RX FIFO.
         * SKIP during active reading — TX during stream can disrupt kernel. */
        if (!g_reading_active) send_broadcast_tp_if_due();

        uint32_t rx_id;
        uint8_t  rx_data[8];
        uint8_t  rx_len;
        if (can_receive(&rx_id, rx_data, &rx_len) == 0) {
            if (rx_id == g_ecu_id) {
                /* Skip kernel ACK/heartbeat frames. Signature is "FLASHY "
                 * (46 4C 41 53 48 59 20) in the Flashy-branded Private Kernel.
                 * Legacy USBJTAG signature (55 53 42 4A 54 41 47) also accepted
                 * for cross-kernel compat. */
                if (rx_len == 7 &&
                    ((rx_data[0] == 0x46 && rx_data[1] == 0x4C &&
                      rx_data[2] == 0x41 && rx_data[3] == 0x53) ||
                     (rx_data[0] == 0x55 && rx_data[1] == 0x53 &&
                      rx_data[2] == 0x42 && rx_data[3] == 0x4A))) {
                    continue;  /* kernel ACK — keep waiting for real data */
                }
                memcpy(out, rx_data, rx_len);
                return rx_len;
            }
            /* Ignore non-ECU frames (bus traffic) */
        }
    }
    return -1;
}

/* Same as extkern_recv but ACCEPTS heartbeat/kernel frames.
 * Used by extkern_write_block — the $6C write ACK IS a kernel frame. */
static int extkern_recv_any(uint8_t *out, uint32_t timeout_ms)
{
    uint32_t deadline = millis() + timeout_ms;
    while (millis() < deadline) {
        send_broadcast_tp_if_due();

        uint32_t rx_id;
        uint8_t  rx_data[8];
        uint8_t  rx_len;
        if (can_receive(&rx_id, rx_data, &rx_len) == 0) {
            if (rx_id == g_ecu_id) {
                memcpy(out, rx_data, rx_len);
                return rx_len;
            }
        }
    }
    return -1;
}

/* Wait for kernel heartbeat — confirms kernel is alive.
 * Returns true if heartbeat received within timeout.
 * Debug: prints first 10 frames seen on any CAN ID. */
static bool extkern_wait_heartbeat(uint32_t timeout_ms)
{
    uint32_t deadline = millis() + timeout_ms;
    uint32_t frame_count = 0;
    while (millis() < deadline) {
        uint32_t rx_id;
        uint8_t  rx_data[8];
        uint8_t  rx_len;
        if (can_receive(&rx_id, rx_data, &rx_len) == 0) {
            frame_count++;
            if (frame_count <= 10) {
                Serial.print("  RX 0x");
                Serial.print(rx_id, HEX);
                Serial.print(" [");
                Serial.print(rx_len);
                Serial.print("] ");
                for (uint8_t i = 0; i < rx_len && i < 8; i++) {
                    if (rx_data[i] < 0x10) Serial.print('0');
                    Serial.print(rx_data[i], HEX);
                    Serial.print(' ');
                }
                Serial.println();
            }
            /* FLASHY kernel ACK = 46 4C 41 53 48 59 20 ("FLASHY ")
             * Legacy USBJTAG ACK = 55 53 42 4A 54 41 47 ("USBJTAG") — accepted too */
            if (rx_id == g_ecu_id && rx_len == 7 &&
                ((rx_data[0] == 0x46 && rx_data[1] == 0x4C &&
                  rx_data[2] == 0x41 && rx_data[3] == 0x53) ||
                 (rx_data[0] == 0x55 && rx_data[1] == 0x53 &&
                  rx_data[2] == 0x42 && rx_data[3] == 0x4A))) {
                Serial.print("  EXTKERN found after ");
                Serial.print(frame_count);
                Serial.println(" frames");
                return true;
            }
        }
    }
    Serial.print("  timeout: ");
    Serial.print(frame_count);
    Serial.println(" frames seen, no heartbeat");
    return false;
}

/* Drain any stale CAN frames from the hardware FIFO */
static void extkern_drain_rx(void)
{
    uint32_t rx_id;
    uint8_t  rx_data[8];
    uint8_t  rx_len;
    while (can_receive(&rx_id, rx_data, &rx_len) == 0) {
        /* discard */
    }
}

/* Send a raw 8-byte CAN frame to ECU and wait for response.
 * Returns response length, or -1 on timeout. */
static int extkern_cmd(const uint8_t *cmd, uint8_t cmd_len,
                       uint8_t *resp, uint32_t timeout_ms)
{
    uint8_t tx[8] = {0};
    memcpy(tx, cmd, (cmd_len > 8) ? 8 : cmd_len);
    if (can_send(g_tester_id, tx, 8) != 0) return -1;
    return extkern_recv(resp, timeout_ms);
}

/* $8A flash register write — two-step paired command.
 * Capture format:
 *   Step 1 (flag=00): 8A 00 00 [addr_hi] [addr_lo] 00 00 00  — set address
 *   Step 2 (flag=01): 8A 00 00 [value_hi] [value_lo] 00 00 01 — write value
 * Address and value are 16-bit, placed in bytes 3-4.
 * No ECU response expected between pair. */
static bool extkern_flash_reg(uint16_t addr, uint16_t value)
{
    uint8_t cmd[8];

    /* Step 1: set address */
    cmd[0] = 0x8A;
    cmd[1] = 0x00;
    cmd[2] = 0x00;
    cmd[3] = (uint8_t)(addr >> 8);
    cmd[4] = (uint8_t)(addr);
    cmd[5] = 0x00;
    cmd[6] = 0x00;
    cmd[7] = 0x00;
    if (can_send(g_tester_id, cmd, 8) != 0) return false;

    /* Step 2: write value at that address */
    cmd[3] = (uint8_t)(value >> 8);
    cmd[4] = (uint8_t)(value);
    cmd[7] = 0x01;
    if (can_send(g_tester_id, cmd, 8) != 0) return false;

    return true;
}

/* SPC564A80 $8A register write — full 32-bit address + value.
 * Format: 8A [addr:4] [val_hi] [val_lo] [flag]
 * The T87A external kernel uses paired $8A frames for flash register access. */
static bool t87a_reg_write(uint32_t addr, uint32_t value, uint8_t flag)
{
    uint8_t cmd[8];
    cmd[0] = 0x8A;
    cmd[1] = (uint8_t)(addr >> 24);
    cmd[2] = (uint8_t)(addr >> 16);
    cmd[3] = (uint8_t)(addr >> 8);
    cmd[4] = (uint8_t)(addr);
    cmd[5] = (uint8_t)(value >> 16);
    cmd[6] = (uint8_t)(value >> 8);
    cmd[7] = flag;
    return can_send(g_tester_id, cmd, 8) == 0;
}

/* Shorthand for paired $8A register writes (addr then value) */
static bool t87a_reg_pair(uint32_t addr, uint32_t value)
{
    if (!t87a_reg_write(addr, 0, 0x00)) return false;  /* set register addr */
    if (!t87a_reg_write(value, 0, 0x01)) return false;  /* write value */
    return true;
}

/* T87A SPC564A80 sector erase.
 * From bus capture: each erase needs $8A register setup + $6B command.
 * Module 0 base: C3F88000, Module 1 base: C3F8C000
 * MCR offset 0x00: Module Control Register
 * MCR offset 0x10: Low/Mid Select Register (LMSR)
 * MCR offset 0x14: High Select Register (HSR)
 */
static bool t87a_erase_sector(uint32_t addr, uint32_t sector_sel,
                                uint32_t module_base, bool use_hsr)
{
    uint32_t sel_reg = module_base + (use_hsr ? 0x14 : 0x10);

    /* Exact sequence from stock-tool HS-CAN write capture (analyzed bus protocol, not their code):
     * Frame 1: 8A [MCR]        00 00 00  — MCR clear (single frame, flag=0)
     * Frame 2: 8A [0x04]       00 00 01  — MCR = EHV (single frame, flag=1)
     * Frame 3: 8A [sel_reg]    00 00 00  — select register addr (flag=0)
     * Frame 4: 8A [sector_sel] 00 00 01  — sector select bits (flag=1)
     * Frame 5: 8A [sect_addr]  00 00 00  — dummy write (single frame, flag=0)
     * Frame 6: 8A FF FF FF FF  00 00 01  — interlock (single frame, flag=1)
     * Frame 7: 8A [MCR]        00 00 00  — MCR addr (flag=0)
     * Frame 8: 8A [0x05]       00 00 01  — MCR = ERS+EHV (flag=1)
     * Frame 9: 6B [addr:4] [size:3]      — erase command
     * Response: kernel ACK
     * Frame 10: 8A [MCR]       00 00 00  — MCR clear ERS (flag=0)
     * Frame 11: 8A [0x04]      00 00 01  — MCR = EHV only (flag=1)
     * Frame 12: 8A [MCR]       00 00 00  — MCR addr (flag=0)
     * Frame 13: 8A [0x00]      00 00 01  — MCR = clear all (flag=1) */

    /* Step 1-2: MCR clear then set EHV */
    t87a_reg_write(module_base, 0, 0x00);
    t87a_reg_write(0x00000004, 0, 0x01);
    /* Step 3-4: Select sector */
    t87a_reg_write(sel_reg, 0, 0x00);
    t87a_reg_write(sector_sel, 0, 0x01);
    /* Step 5: Dummy write to sector address */
    t87a_reg_write(addr, 0, 0x00);
    /* Step 6: Interlock */
    t87a_reg_write(0xFFFFFFFF, 0, 0x01);
    /* Step 7-8: MCR = ERS+EHV (start erase) */
    t87a_reg_write(module_base, 0, 0x00);
    t87a_reg_write(0x00000005, 0, 0x01);

    /* Step 9: Send $6B erase command */
    uint8_t cmd[8] = {0};
    cmd[0] = 0x6B;
    cmd[1] = (uint8_t)(addr >> 24);
    cmd[2] = (uint8_t)(addr >> 16);
    cmd[3] = (uint8_t)(addr >> 8);
    cmd[4] = (uint8_t)(addr);
    /* bytes 5-7: sector size from bus capture — varies by sector */
    if (addr < 0x020000) {
        cmd[5] = 0x00; cmd[6] = 0x40; cmd[7] = 0x00;  /* 0x004000 = 16KB */
    } else if (addr < 0x080000) {
        cmd[5] = 0x02; cmd[6] = 0x00; cmd[7] = 0x00;  /* 0x020000 = 128KB */
    } else if (addr < 0x100000) {
        cmd[5] = 0x04; cmd[6] = 0x00; cmd[7] = 0x00;  /* 0x040000 = 256KB */
    } else {
        cmd[5] = 0x08; cmd[6] = 0x00; cmd[7] = 0x00;  /* 0x080000 = 512KB */
    }

    /* Send $6B and wait for kernel ACK.
     * Must use extkern_recv_any — the ACK IS a kernel frame (55 53 42...)
     * that extkern_recv would filter out as a heartbeat. */
    if (can_send(g_tester_id, cmd, 8) != 0) return false;
    uint8_t resp[8];
    int rlen = extkern_recv_any(resp, 30000);
    if (rlen < 7) return false;
    /* Accept FLASHY ("FLA" prefix = 46 4C 41) or legacy USBJTAG ("USB" = 55 53 42) */
    bool is_flashy = (resp[0] == 0x46 && resp[1] == 0x4C && resp[2] == 0x41);
    bool is_usbj   = (resp[0] == 0x55 && resp[1] == 0x53 && resp[2] == 0x42);
    if (!is_flashy && !is_usbj) {
        return false;
    }

    /* Step 10-13: MCR cleanup — clear ERS, then clear all */
    t87a_reg_write(module_base, 0, 0x00);
    t87a_reg_write(0x00000004, 0, 0x01);
    t87a_reg_write(module_base, 0, 0x00);
    t87a_reg_write(0x00000000, 0, 0x01);

    return true;
}

/* T87A flash unlock — unlock all sectors on both modules.
 * Uses $8A register writes with SPC564A80 flash passwords.
 * From bus capture: SIU init FIRST, then flash passwords. */
static bool t87a_flash_unlock(void)
{
    const uint32_t MOD0 = 0xC3F88000;
    const uint32_t MOD1 = 0xC3F8C000;

    /* SIU initialization (from capture frames 3053-3058) */
    t87a_reg_pair(0xFFF38010, 0x0000C520);  /* SIU system reset */
    t87a_reg_pair(0xFFF38010, 0x0000D928);  /* SIU system reset */
    t87a_reg_pair(0xFFF38000, 0xFF00000A);  /* SIU MIDR */

    /* Module 0: LMLR + password A1A11111
     * bus capture: reg_addr(flag=0) → password(flag=1) → reg_addr(flag=0) → unlock_bits(flag=1) */
    t87a_reg_pair(MOD0 + 0x04, 0xA1A11111);  /* LMLR addr + password */
    t87a_reg_pair(MOD0 + 0x04, 0x00100000);  /* LMLR addr + unlock bits */
    /* Module 0: HLR + password B2B22222 */
    t87a_reg_pair(MOD0 + 0x08, 0xB2B22222);
    t87a_reg_pair(MOD0 + 0x08, 0x00000000);  /* HLR unlock = 0x00000000 */
    /* Module 0: SLMLR + password C3C33333 */
    t87a_reg_pair(MOD0 + 0x0C, 0xC3C33333);
    t87a_reg_pair(MOD0 + 0x0C, 0x00100000);

    /* Module 1: same pattern */
    t87a_reg_pair(MOD1 + 0x04, 0xA1A11111);
    t87a_reg_pair(MOD1 + 0x04, 0x00100000);
    t87a_reg_pair(MOD1 + 0x08, 0xB2B22222);
    t87a_reg_pair(MOD1 + 0x08, 0x00100000);  /* Module 1 HLR = 0x00100000 */
    t87a_reg_pair(MOD1 + 0x0C, 0xC3C33333);
    t87a_reg_pair(MOD1 + 0x0C, 0x00100000);

    return true;
}

/* T87A sector table — addresses, select bits, module assignment.
 * From stock-tool HS-CAN write capture (analyzed bus protocol, not their code). Boot sector (0x020000-0x03FFFF) excluded. */
struct t87a_sector {
    uint32_t addr;
    uint32_t sel;       /* sector select bit for LMSR/HSR */
    uint32_t mod_base;  /* C3F88000 or C3F8C000 */
    bool     use_hsr;   /* true = HSR (0x14), false = LMSR (0x10) */
    bool     dual;      /* true = needs both modules erased */
};

/* Sector table — exact match to stock-tool HS-CAN write capture (analyzed bus protocol, not their code).
 * Each entry is ONE $6B erase command. Dual-module sectors have TWO entries.
 * size field = bytes 5-7 of the $6B command. */
static const t87a_sector T87A_SECTORS[] = {
    /* Low blocks — Module 0, LMSR, 16KB each */
    { 0x000000, 0x00000001, 0xC3F88000, false, false },  /* $6B size=0x004000 */
    { 0x004000, 0x00000002, 0xC3F88000, false, false },
    { 0x008000, 0x00000004, 0xC3F88000, false, false },
    { 0x00C000, 0x00000008, 0xC3F88000, false, false },
    { 0x010000, 0x00000010, 0xC3F88000, false, false },
    { 0x014000, 0x00000020, 0xC3F88000, false, false },
    { 0x018000, 0x00000040, 0xC3F88000, false, false },
    { 0x01C000, 0x00000080, 0xC3F88000, false, false },
    /* SKIP 0x020000-0x03FFFF — boot sector, protected by kernel */
    /* Mid blocks — Module 0, LMSR, 128KB each */
    { 0x040000, 0x00010000, 0xC3F88000, false, false },  /* $6B size=0x020000 */
    { 0x060000, 0x00020000, 0xC3F88000, false, false },
    /* Module 1 blocks — M1_LMSR, 256KB each */
    { 0x080000, 0x00000001, 0xC3F8C000, false, false },  /* $6B size=0x040000 */
    { 0x0C0000, 0x00010000, 0xC3F8C000, false, false },
    /* Large blocks — HSR, DUAL: M0 then M1, 512KB each */
    { 0x100000, 0x00000001, 0xC3F88000, true,  false },  /* $6B size=0x080000, M0 */
    { 0x100000, 0x00000001, 0xC3F8C000, true,  false },  /* $6B size=0x080000, M1 */
    { 0x180000, 0x00000002, 0xC3F88000, true,  false },
    { 0x180000, 0x00000002, 0xC3F8C000, true,  false },
    { 0x200000, 0x00000004, 0xC3F88000, true,  false },
    { 0x200000, 0x00000004, 0xC3F8C000, true,  false },
    { 0x280000, 0x00000008, 0xC3F88000, true,  false },
    { 0x280000, 0x00000008, 0xC3F8C000, true,  false },
    { 0x300000, 0x00000010, 0xC3F88000, true,  false },
    { 0x300000, 0x00000010, 0xC3F8C000, true,  false },
    { 0x380000, 0x00000020, 0xC3F88000, true,  false },
    { 0x380000, 0x00000020, 0xC3F8C000, true,  false },
};
#define T87A_NUM_SECTORS (sizeof(T87A_SECTORS) / sizeof(T87A_SECTORS[0]))

/* AMD/Spansion flash unlock sequence (word-addressed for 32-bit bus):
 * 0x1554 <- 0xAA, 0x0AA8 <- 0x55 */
static bool extkern_flash_unlock(void)
{
    if (!extkern_flash_reg(0x1554, 0x00AA)) return false;
    if (!extkern_flash_reg(0x0AA8, 0x0055)) return false;
    return true;
}

/* Flash reset — put flash chip back to read mode.
 * Sends both Intel (0xFF) and AMD (0xF0) reset to addr 0x0000.
 * Must be called before $A0 read commands. */
static bool extkern_flash_reset(void)
{
    if (!extkern_flash_reg(0x0000, 0xFFFF)) return false;
    if (!extkern_flash_reg(0x0000, 0xF0F0)) return false;
    return true;
}

/* $6B sector erase — erases one flash sector.
 * Format: 6B [addr:4] [pad:3]
 * Response: addr echo + "USB" (55 53 42) within timeout. */
static bool extkern_erase_sector(uint32_t addr, uint32_t timeout_ms)
{
    /* Full AMD erase sequence:
     * Unlock: 0x1554<-AA, 0x0AA8<-55
     * Setup:  0x1554<-80
     * Unlock: 0x1554<-AA, 0x0AA8<-55
     * Sector: [sector_addr]<-30 */
    if (!extkern_flash_unlock()) return false;
    if (!extkern_flash_reg(0x1554, 0x0080)) return false;
    if (!extkern_flash_unlock()) return false;

    /* Now send $6B erase command */
    uint8_t cmd[8] = {0};
    cmd[0] = 0x6B;
    cmd[1] = (uint8_t)(addr >> 24);
    cmd[2] = (uint8_t)(addr >> 16);
    cmd[3] = (uint8_t)(addr >> 8);
    cmd[4] = (uint8_t)(addr);

    uint8_t resp[8];
    int rlen = extkern_cmd(cmd, 8, resp, timeout_ms);
    if (rlen < 7) return false;

    /* Verify response contains "USB" (0x55 0x53 0x42) */
    if (resp[4] != 0x55 || resp[5] != 0x53 || resp[6] != 0x42) {
        Serial.print("ERASE: unexpected response at 0x");
        Serial.println(addr, HEX);
        return false;
    }
    return true;
}

/* $6C write block — programs 4096 bytes (0x1000) at given address.
 * Format: 6C [addr:4] [size:3=00 10 00]
 * Then send 512 raw CAN frames (512 x 8 = 4096 bytes).
 * Kernel responds with "EXTKERN" (7 bytes) when done. */
static bool extkern_write_block(uint32_t addr, const uint8_t *data,
                                uint32_t data_len, uint32_t timeout_ms)
{
    /* AMD program setup: unlock + 0x1554<-A0.
     * Skip for T87A — SPC564A80 kernel handles programming internally. */
    if (!g_t87a_detected) {
        if (!extkern_flash_unlock()) return false;
        if (!extkern_flash_reg(0x1554, 0x00A0)) return false;
    }

    /* Send $6C command */
    uint8_t cmd[8] = {0};
    cmd[0] = 0x6C;
    cmd[1] = (uint8_t)(addr >> 24);
    cmd[2] = (uint8_t)(addr >> 16);
    cmd[3] = (uint8_t)(addr >> 8);
    cmd[4] = (uint8_t)(addr);
    /* Size field on T87A is NOT a byte count — it encodes the target
     * flash module + sector type. Decoded from bench NT-Link Trans Write
     * capture 2026-04-22 (T87A-NTLink-Trans-Write.csv):
     *   0x011000 — Module 0 LMSR   (addr < 0x080000, low + mid blocks)
     *   0x021000 — Module 1 LMSR   (0x080000 <= addr < 0x100000)
     *   0x031000 — HSR dual-module (addr >= 0x100000)
     * Flashy previously hardcoded 0x011000 for ALL addresses, which is
     * the WRONG module selector for cal-region writes (0x080000+). The
     * kernel silently ACKs but programs the wrong physical sector,
     * leaving post-write flash in an inconsistent state → brick on
     * reboot. Three confirmed bricks this way before this fix. See
     * memory/t87a_cal_write_unsolved.md.
     * E38 EXTKERN still uses raw byte count (different protocol). */
    if (g_t87a_detected) {
        if (addr < 0x080000) {
            cmd[5] = 0x01; cmd[6] = 0x10; cmd[7] = 0x00;
        } else if (addr < 0x100000) {
            cmd[5] = 0x02; cmd[6] = 0x10; cmd[7] = 0x00;
        } else {
            cmd[5] = 0x03; cmd[6] = 0x10; cmd[7] = 0x00;
        }
    } else {
        cmd[5] = (uint8_t)(data_len >> 16);
        cmd[6] = (uint8_t)(data_len >> 8);
        cmd[7] = (uint8_t)(data_len);
    }

    if (can_send(g_tester_id, cmd, 8) != 0) return false;

    /* Stream 512 raw CAN frames (512 x 8 = 4096 bytes) */
    uint32_t frames = data_len / 8;
    for (uint32_t f = 0; f < frames; f++) {
        if (can_send(g_tester_id, &data[f * 8], 8) != 0) return false;
        /* Pace: tiny yield every 64 frames to avoid TX buffer overflow */
        if ((f & 0x3F) == 0x3F) delayMicroseconds(100);
    }

    /* Wait for "EXTKERN" ACK (55 53 42 4A 54 41 47).
     * Must use extkern_recv_any — the ACK IS a kernel frame that
     * extkern_recv would filter out as a "heartbeat". */
    uint8_t resp[8];
    int rlen = extkern_recv_any(resp, timeout_ms);
    if (rlen < 7) {
        Serial.print("WRITE: no EXTKERN ack at 0x");
        Serial.println(addr, HEX);
        return false;
    }
    /* Accept FLASHY ACK ("FLASHY " = 46 4C 41 53 48 59 20)
     * or legacy USBJTAG ACK ("USBJTAG" = 55 53 42 4A 54 41 47).
     * Same frame shape, different branding — both are valid kernel completes. */
    bool ack_flashy = (resp[0] == 0x46 && resp[1] == 0x4C && resp[2] == 0x41 &&
                       resp[3] == 0x53 && resp[4] == 0x48 && resp[5] == 0x59 &&
                       resp[6] == 0x20);
    bool ack_usbj   = (resp[0] == 0x55 && resp[1] == 0x53 && resp[2] == 0x42 &&
                       resp[3] == 0x4A && resp[4] == 0x54 && resp[5] == 0x41 &&
                       resp[6] == 0x47);
    if (!ack_flashy && !ack_usbj) {
        Serial.print("WRITE: bad ack at 0x");
        Serial.println(addr, HEX);
        return false;
    }
    return true;
}

/* Send $A0 read command — starts a streaming read from kernel.
 * Format: A0 [addr:4] [size:3]   (4-byte address, 3-byte size)
 * The kernel then streams ALL data as consecutive raw CAN frames.
 * Returns 0 on success (command sent), -1 on failure. */
static int extkern_start_read(uint32_t addr, uint32_t size)
{
    uint8_t cmd[8] = {0};
    cmd[0] = 0xA0;
    cmd[1] = (uint8_t)(addr >> 24);
    cmd[2] = (uint8_t)(addr >> 16);
    cmd[3] = (uint8_t)(addr >> 8);
    cmd[4] = (uint8_t)(addr);
    cmd[5] = (uint8_t)(size >> 16);
    cmd[6] = (uint8_t)(size >> 8);
    cmd[7] = (uint8_t)(size);

    return can_send(g_tester_id, cmd, 8);
}

/* Receive a buffer of raw CAN data frames from the kernel stream.
 * Used after extkern_start_read() to collect the data as it arrives.
 * Returns number of bytes received, or -1 on timeout. */
static int extkern_recv_chunk(uint8_t *out, uint32_t size, uint32_t timeout_ms)
{
    uint32_t received = 0;
    uint32_t frames_needed = (size + 7) / 8;
    for (uint32_t f = 0; f < frames_needed; f++) {
        uint8_t resp[8];
        int rlen = extkern_recv(resp, timeout_ms);
        if (rlen <= 0) return (received > 0) ? (int)received : -1;
        uint32_t copy = (size - received < (uint32_t)rlen)
                        ? (size - received) : (uint32_t)rlen;
        memcpy(out + received, resp, copy);
        received += copy;
    }
    return (int)received;
}

/* EXTKERN full write: erase all sectors then program 4KB blocks.
 * This performs the complete AMD/Spansion erase → program sequence
 * using raw CAN commands to the external kernel. */
static void cmd_extkern_fullwrite(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }

    Serial.println("EXTKERN_FULLWRITE: starting external-kernel raw CAN write");
    Serial.println("EXTKERN_FULLWRITE:WARNING *** DO NOT POWER DOWN UNTIL COMPLETE ***");

    /* Step 1: Verify kernel is alive — wait for kernel heartbeat */
    Serial.println("EXTKERN: waiting for kernel heartbeat...");
    extkern_drain_rx();
    if (!extkern_wait_heartbeat(3000)) {
        led_error();
        print_err("no kernel heartbeat — kernel not running?");
        return;
    }
    Serial.println("EXTKERN: kernel alive");

    /* Step 2: Erase — iterate all sectors from capture sequence.
     * bus capture shows erase addresses going 0x000000 through 0x1FF000.
     * The kernel handles the sector geometry internally with the $6B command;
     * we just send the sector base addresses. From the capture, 54 erase
     * commands were sent covering the full 2MB. */

    /* Build sector list from capture: 8x8KB + 38x64KB + 8x8KB */
    uint32_t erase_addrs[54];
    int nsec = 0;
    /* 8 x 8KB bottom (0x000000 - 0x00E000) */
    for (uint32_t a = 0x000000; a < 0x010000; a += 0x002000)
        erase_addrs[nsec++] = a;
    /* 38 x 64KB main (0x010000 - 0x1EFFFF) — actually 30 sectors */
    for (uint32_t a = 0x010000; a < 0x1F0000; a += 0x010000)
        erase_addrs[nsec++] = a;
    /* 8 x 8KB top (0x1F0000 - 0x1FE000) */
    for (uint32_t a = 0x1F0000; a < 0x200000; a += 0x002000)
        erase_addrs[nsec++] = a;

    Serial.print("EXTKERN: erasing ");
    Serial.print(nsec);
    Serial.println(" sectors...");
    led_erasing();

    for (int s = 0; s < nsec; s++) {
        Serial.print("ERASE:0x");
        Serial.print(erase_addrs[s], HEX);

        if (!extkern_erase_sector(erase_addrs[s], 10000)) {
            Serial.println(" FAIL");
            led_error();
            Serial.println("CRITICAL: ERASE FAILED - DO NOT POWER DOWN");
            return;
        }
        Serial.println(" OK");
        send_broadcast_tp_if_due();
    }

    Serial.println("EXTKERN: erase complete");

    /* Step 3: Program — receive WDATA blocks from PC and write via $6C.
     * EXTKERN uses 4KB (0x1000) blocks = 512 write blocks for 2MB.
     * We'll signal WRITE:READY so capture_write.py sends WDATA lines. */
    uint32_t block_size = 0x1000;  /* 4096 bytes per EXTKERN write block */
    uint32_t total_blocks = 0x200000 / block_size;  /* 512 blocks */

    led_writing();
    Serial.print("WRITE:READY 0 ");
    Serial.println(total_blocks);

    uint32_t addr = 0x000000;
    uint32_t good_blocks = 0;
    uint8_t  block_buf[0x1000];  /* 4KB buffer */

    for (uint32_t blk = 0; blk < total_blocks; blk++) {
        /* Receive WDATA line from PC — we need to handle larger blocks.
         * The existing hex_line_to_block expects 0x800 blocks. For EXTKERN
         * we need 0x1000. We'll receive two WDATA lines per block or
         * adapt the protocol.
         *
         * Actually, let's keep it simple: receive two 0x800 WDATA lines
         * per 0x1000 block, since capture_write.py already uses 0x800. */
        uint32_t wdata_addr = 0;

        /* First half (0x800 bytes) */
        int data_len = hex_line_to_block(block_buf, &wdata_addr, WRITE_WDATA_TIMEOUT_MS);
        if (data_len <= 0) {
            Serial.print("EXTKERN:TIMEOUT block=");
            Serial.println(blk);
            Serial.println("CRITICAL: WRITE INCOMPLETE - DO NOT POWER DOWN");
            return;
        }
        while (data_len < 0x800) block_buf[data_len++] = 0xFF;

        /* Second half (0x800 bytes) */
        data_len = hex_line_to_block(block_buf + 0x800, &wdata_addr, WRITE_WDATA_TIMEOUT_MS);
        if (data_len <= 0) {
            Serial.print("EXTKERN:TIMEOUT block=");
            Serial.println(blk);
            Serial.println("CRITICAL: WRITE INCOMPLETE - DO NOT POWER DOWN");
            return;
        }
        while (data_len < 0x800) block_buf[0x800 + data_len++] = 0xFF;

        /* Write 4KB block via EXTKERN $6C protocol */
        if (!extkern_write_block(addr, block_buf, block_size, 10000)) {
            Serial.print("WDATA:NAK:");
            print_addr_hex6(addr);
            Serial.println();
            Serial.println("CRITICAL: WRITE FAILED - DO NOT POWER DOWN");
            return;
        }

        good_blocks++;
        Serial.print("WDATA:ACK:");
        print_addr_hex6(addr);
        Serial.println();

        addr += block_size;
        send_broadcast_tp_if_due();

        if ((blk & 0x1F) == 0x1F) {
            Serial.print("WRITE:PROGRESS ");
            Serial.print(blk + 1);
            Serial.print("/");
            Serial.println(total_blocks);
        }
    }

    led_success();
    Serial.print("WRITE:DONE blocks=");
    Serial.print(good_blocks);
    Serial.print(" bytes=");
    Serial.println(good_blocks * block_size);
    print_ok();
}

/* ---------- TESTWRITE: self-contained single-sector write test ----------
 *
 * Targets the LAST 8KB boot sector (0x1FE000-0x1FFFFF) which is safe
 * to test — it's the last sector of the calibration region.
 *
 * Flow: entry sequence → read into RAM → erase → verify erased →
 *       write from RAM → verify written. No PC data transfer needed.
 *       Emergency restore on any failure.
 */
static void cmd_testwrite(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (g_module != MODULE_E38N) { print_err("TESTWRITE requires ALGO E38N"); return; }

    const uint32_t SECTOR_ADDR = 0x1FE000;
    const uint32_t SECTOR_SIZE = 0x2000;   /* 8KB */
    const uint32_t BLOCK_SIZE  = 0x1000;   /* 4KB EXTKERN write block */
    const uint32_t CAN_FRAMES  = SECTOR_SIZE / 8;  /* 1024 */

    static uint8_t sector_buf[0x2000];  /* 8KB — static to avoid stack pressure */

    Serial.println("TESTWRITE: incremental write test — sector 0x1FE000 (8KB)");
    Serial.println("TESTWRITE:WARNING *** DO NOT POWER DOWN UNTIL COMPLETE ***");

    /* ---- Entry sequence (mirrors FULLREAD E38N) ---- */
    uds_msg_t resp;

    Serial.println("TESTWRITE: disableDTCSetting...");
    {
        uint8_t a2_req[1] = { 0xA2 };
        uds_request(&g_isotp_link, a2_req, 1, &resp, UDS_DEFAULT_TIMEOUT_MS);
    }

    Serial.println("TESTWRITE: SecurityAccess...");
    led_auth();
    if (!do_security_access()) { led_error(); return; }

    e38_enter_programming_mode();

    Serial.println("TESTWRITE: uploading external kernel...");
    cmd_kernel(NULL);
    if (!g_auto_broadcast) {
        led_error();
        print_err("kernel upload failed");
        return;
    }

    /* ---- Phase 1: Read original sector into RAM ---- */
    Serial.println("TESTWRITE: Phase 1 — reading sector 0x1FE000...");
    led_reading();

    /* Follow same flow as proven FULLREAD: drain, flash_reset, filter, read */
    extkern_drain_rx();
    Serial.println("TESTWRITE: flash reset...");
    if (!extkern_flash_reset()) {
        led_error(); print_err("flash reset failed"); goto cleanup;
    }
    delay(50);

    can_set_filter(g_ecu_id, 0x7FF);
    extkern_drain_rx();

    if (extkern_start_read(SECTOR_ADDR, SECTOR_SIZE) != 0) {
        led_error(); print_err("$A0 read send failed"); goto cleanup;
    }

    uint32_t checksum_orig;
    checksum_orig = 0;
    for (uint32_t f = 0; f < CAN_FRAMES; f++) {
        uint8_t frame[8];
        int rlen = extkern_recv(frame, 10000);
        if (rlen <= 0) {
            led_error();
            Serial.print("ERR: read stream lost frame=");
            Serial.print(f);
            Serial.print("/");
            Serial.println(CAN_FRAMES);
            goto cleanup;
        }
        memcpy(sector_buf + f * 8, frame, 8);
        for (int i = 0; i < 8; i++) checksum_orig += frame[i];
    }

    Serial.print("TESTWRITE:CHECKSUM_ORIG 0x");
    Serial.println(checksum_orig, HEX);

    /* ---- Phase 2: Erase sector ---- */
    Serial.println("TESTWRITE: Phase 2 — erasing sector 0x1FE000...");
    led_erasing();

    extkern_drain_rx();
    delay(200);

    if (!extkern_erase_sector(SECTOR_ADDR, 30000)) {
        led_error();
        Serial.println("CRITICAL: ERASE FAILED — attempting emergency restore");
        goto emergency_write;
    }
    Serial.println("TESTWRITE: erase OK");

    /* ---- Phase 3: Verify erased ---- */
    {
        Serial.println("TESTWRITE: Phase 3 — verifying erased...");
        led_reading();

        extkern_drain_rx();
        delay(200);

        if (!extkern_flash_reset()) {
            Serial.println("WARN: flash reset failed, trying read anyway");
        }
        delay(10);
        extkern_drain_rx();

        if (extkern_start_read(SECTOR_ADDR, SECTOR_SIZE) != 0) {
            led_error();
            Serial.println("ERR: verify-erase read failed — emergency restore");
            goto emergency_write;
        }

        uint32_t checksum_erased = 0;
        bool all_ff = true;
        for (uint32_t f = 0; f < CAN_FRAMES; f++) {
            uint8_t frame[8];
            int rlen = extkern_recv(frame, 10000);
            if (rlen <= 0) {
                led_error();
                Serial.print("ERR: verify-erase stream lost frame=");
                Serial.println(f);
                goto emergency_write;
            }
            for (int i = 0; i < 8; i++) {
                checksum_erased += frame[i];
                if (frame[i] != 0xFF) all_ff = false;
            }
        }

        Serial.print("TESTWRITE:CHECKSUM_ERASED 0x");
        Serial.println(checksum_erased, HEX);

        if (!all_ff) {
            Serial.println("WARN: not fully erased — proceeding anyway");
        } else {
            Serial.println("TESTWRITE: erase verified — all 0xFF");
        }
    }

    /* ---- Phase 4: Write original data back ---- */
    {
        Serial.println("TESTWRITE: Phase 4 — writing sector back...");
        led_writing();

        /* Flash reset before write to ensure clean state */
        extkern_drain_rx();
        if (!extkern_flash_reset()) {
            Serial.println("WARN: flash reset before write failed");
        }
        delay(200);
        extkern_drain_rx();

        /* Write first 4KB block */
        if (!extkern_write_block(SECTOR_ADDR, sector_buf, BLOCK_SIZE, 30000)) {
            led_error();
            Serial.println("CRITICAL: write block 1 (0x1FE000) FAILED");
            goto emergency_write;
        }
        Serial.println("TESTWRITE: block 0x1FE000 written OK");

        extkern_drain_rx();
        delay(100);

        /* Write second 4KB block */
        if (!extkern_write_block(SECTOR_ADDR + BLOCK_SIZE,
                                sector_buf + BLOCK_SIZE,
                                BLOCK_SIZE, 30000)) {
            led_error();
            Serial.println("CRITICAL: write block 2 (0x1FF000) FAILED");
            goto emergency_write;
        }
        Serial.println("TESTWRITE: block 0x1FF000 written OK");
    }

    /* ---- Phase 5: Verify written data ---- */
    {
        Serial.println("TESTWRITE: Phase 5 — verifying written data...");
        led_reading();

        extkern_drain_rx();
        delay(200);

        if (!extkern_flash_reset()) {
            Serial.println("WARN: flash reset failed, trying read anyway");
        }
        delay(10);
        extkern_drain_rx();

        if (extkern_start_read(SECTOR_ADDR, SECTOR_SIZE) != 0) {
            led_error();
            print_err("verify-write read failed");
            goto cleanup;
        }

        uint32_t checksum_final = 0;
        for (uint32_t f = 0; f < CAN_FRAMES; f++) {
            uint8_t frame[8];
            int rlen = extkern_recv(frame, 10000);
            if (rlen <= 0) {
                led_error();
                Serial.print("ERR: verify-write stream lost frame=");
                Serial.println(f);
                goto cleanup;
            }
            for (int i = 0; i < 8; i++) checksum_final += frame[i];
        }

        Serial.print("TESTWRITE:CHECKSUM_FINAL 0x");
        Serial.println(checksum_final, HEX);

        if (checksum_final == checksum_orig) {
            Serial.println("TESTWRITE: *** VERIFICATION PASSED ***");
            led_celebrate();
        } else {
            Serial.println("TESTWRITE: *** VERIFICATION FAILED ***");
            Serial.print("  expected 0x"); Serial.print(checksum_orig, HEX);
            Serial.print("  got 0x"); Serial.println(checksum_final, HEX);
            led_error();
        }
    }

    goto cleanup;

emergency_write:
    /* Emergency: try to restore sector from RAM buffer */
    Serial.println("TESTWRITE: EMERGENCY — restoring sector from RAM...");
    led_writing();
    extkern_drain_rx();
    delay(500);
    {
        bool ok1 = extkern_write_block(SECTOR_ADDR, sector_buf, BLOCK_SIZE, 30000);
        bool ok2 = extkern_write_block(SECTOR_ADDR + BLOCK_SIZE,
                                      sector_buf + BLOCK_SIZE, BLOCK_SIZE, 30000);
        if (ok1 && ok2) {
            Serial.println("TESTWRITE: emergency restore completed");
        } else {
            Serial.println("TESTWRITE: EMERGENCY RESTORE FAILED");
            Serial.println("Use backup file: reads/2G1FK1EJ0A9118489_86AAMFK10055W6E9.bin");
        }
    }

cleanup:
    can_set_filter(0, 0);  /* restore accept-all */
    e38_return_to_normal();
    g_auto_broadcast = false;
    g_extkern_active  = false;
    led_connected();
    Serial.println("TESTWRITE:DONE");
    print_ok();
}

/* EXTKERN read — read flash via raw CAN $A0 command.
 * The external kernel streams ALL data as consecutive CAN frames after
 * a single $A0 command. We collect frames into 0x800 (2KB) blocks and
 * output each as a DATA: line for compatibility with capture_read.py. */
static void cmd_extkern_read(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }

    char buf[64];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *astr = strtok(buf, " ");
    char *sstr = strtok(NULL, " ");
    if (!astr || !sstr) {
        print_err("usage: READ <start_addr> <num_blocks>");
        return;
    }

    uint32_t start_addr = parse_hex(astr);
    uint32_t num_blocks = parse_dec_or_hex(sstr);
    if (num_blocks == 0 || num_blocks > 0xFFFF) {
        print_err("invalid block count");
        return;
    }

    uint32_t total_bytes = num_blocks * 0x800UL;

    /* Step 1: Verify kernel is alive — wait for kernel heartbeat.
     * Skip if g_extkern_active already set (kernel confirmed in cmd_kernel). */
    extkern_drain_rx();  /* flush stale frames */
    if (!g_extkern_active) {
        Serial.println("EXTKERN: waiting for kernel heartbeat...");
        if (!extkern_wait_heartbeat(3000)) {
            led_error();
            print_err("no kernel heartbeat — kernel not running?");
            return;
        }
        Serial.println("EXTKERN: kernel alive");
        g_extkern_active = true;
    } else {
        Serial.println("EXTKERN: kernel already confirmed alive");
    }

    /* Step 2: Flash reset — put flash in read mode.
     * T87A SPC564A80 doesn't need AMD flash reset — skip for T87A. */
    if (!g_t87a_detected) {
        Serial.println("EXTKERN: flash reset...");
        if (!extkern_flash_reset()) {
            led_error();
            print_err("flash reset failed");
            return;
        }
        delay(10);
    } else {
        Serial.println("EXTKERN: T87A — skipping flash reset (SPC564A80)");
    }

    led_reading();
    g_reading_active = true;  /* suppress NeoPixel during read — pixel.show() disables IRQs */
    Serial.print("READ:START addr=0x");
    Serial.print(start_addr, HEX);
    Serial.print(" blocks=");
    Serial.print(num_blocks);
    Serial.print(" bytes=");
    Serial.println(total_bytes);

    /* Step 3: Set hardware CAN filter to only accept ECU ID.
     * This prevents bus chatter from overflowing the RX FIFO. */
    can_set_filter(g_ecu_id, 0x7FF);  /* exact match */

    /* Disable broadcast TP during streaming read — sending CAN frames
     * on 0x0101 can disrupt the kernel's read stream */
    g_auto_broadcast = false;

    /* Step 4: Open SD file */
    uint32_t addr = start_addr;
    uint32_t good_blocks = 0;
    uint8_t  read_buf[0x800];
    FsFile   sd_file;
    bool     use_sd = false;

    if (g_sd_ok) {
        char fname[128];  /* fits "/Read/HSREAD_<VIN17>_<OSID16>_<ts13>.bin" */
        if (g_sd_filename[0]) {
            strncpy(fname, g_sd_filename, sizeof(fname) - 1);
            fname[sizeof(fname) - 1] = '\0';
        } else {
            snprintf(fname, sizeof(fname), "read_%06lX.bin", (unsigned long)start_addr);
        }
        sd_file = g_sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
        if (sd_file) {
            if (!sd_file.preAllocate(total_bytes)) {
                Serial.println("WARN: SD pre-allocate failed");
            }
            use_sd = true;
            Serial.print("SD: writing to ");
            Serial.println(fname);
        } else {
            Serial.println("WARN: SD file open failed, using serial only");
        }
    }

    /* Step 5: Read in chunks — multiple $A0 commands with flash reset
     * between each. Avoids kernel stream-length issues. */
    uint32_t checksum = 0;

    #define CHUNK_BLOCKS 256  /* 512 KB per $A0 command */
    uint32_t chunk_start = 0;

    while (chunk_start < num_blocks) {
        uint32_t chunk_blks = num_blocks - chunk_start;
        if (chunk_blks > CHUNK_BLOCKS) chunk_blks = CHUNK_BLOCKS;
        uint32_t chunk_addr = start_addr + chunk_start * 0x800UL;
        uint32_t chunk_bytes = chunk_blks * 0x800UL;

        /* Flash reset + drain before each chunk (skip for T87A SPC564A80) */
        if (!g_t87a_detected) {
            extkern_flash_reset();
            delay(5);
        }
        extkern_drain_rx();

        if (extkern_start_read(chunk_addr, chunk_bytes) != 0) {
            led_error();
            Serial.print("ERR: $A0 failed at 0x");
            Serial.println(chunk_addr, HEX);
            if (use_sd) { sd_file.close(); }
            can_set_filter(0, 0);
            g_auto_broadcast = true;
            return;
        }

        for (uint32_t i = 0; i < chunk_blks; i++) {
            uint32_t blk = chunk_start + i;
            uint32_t rx_pos = 0;
            for (uint32_t f = 0; f < 256; f++) {
                uint8_t resp[8];
                int rlen = extkern_recv(resp, 10000);
                if (rlen <= 0) {
                    led_error();
                    Serial.print("ERR: stream lost at 0x");
                    Serial.print(addr, HEX);
                    Serial.print(" frame=");
                    Serial.print(f);
                    Serial.print("/256 blk=");
                    Serial.print(blk);
                    Serial.print("/");
                    Serial.println(num_blocks);
                    if (use_sd) { sd_file.close(); }
                    can_set_filter(0, 0);
                    g_auto_broadcast = true;
                    return;
                }
                uint32_t copy = (0x800 - rx_pos < (uint32_t)rlen)
                                ? (0x800 - rx_pos) : (uint32_t)rlen;
                memcpy(read_buf + rx_pos, resp, copy);
                rx_pos += copy;
            }

            if (use_sd) {
                sd_file.write(read_buf, 0x800);
            }
            for (uint16_t j = 0; j < 0x800; j++) {
                checksum += read_buf[j];
            }

            good_blocks++;
            addr += 0x0800;

            if ((blk & 0x3F) == 0x3F) {
                Serial.print("READ:PROGRESS ");
                Serial.print(blk + 1);
                Serial.print("/");
                Serial.println(num_blocks);
            }
        }

        Serial.print("READ:CHUNK 0x");
        Serial.print(chunk_addr, HEX);
        Serial.print(" (");
        Serial.print(good_blocks);
        Serial.print("/");
        Serial.print(num_blocks);
        Serial.println(")");

        chunk_start += chunk_blks;
    }

    if (use_sd) {
        sd_file.flush();
        Serial.print("SD: file size ");
        Serial.print(sd_file.size());
        Serial.println(" bytes");
        sd_file.close();
    }

    can_set_filter(0, 0);  /* restore accept-all */
    g_auto_broadcast = true;  /* re-enable broadcast TP */
    g_reading_active = false;  /* re-enable NeoPixel */
    Serial.print("READ:DONE blocks=");
    Serial.print(good_blocks);
    Serial.print(" bytes=");
    Serial.print(good_blocks * 0x800UL);
    Serial.print(" checksum=0x");
    Serial.println(checksum, HEX);
    led_connected();
    print_ok();
}

static void cmd_read(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }

    /* E38N uses external-kernel raw CAN read protocol */
    if (g_module == MODULE_E38N) {
        cmd_extkern_read(arg);
        return;
    }

    /* T87A with external kernel active — redirect to EXTKERN read */
    if (g_t87a_detected && g_extkern_active) {
        cmd_extkern_read(arg);
        return;
    }

    char buf[64];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *astr = strtok(buf, " ");
    char *sstr = strtok(NULL, " ");
    if (!astr || !sstr) {
        print_err("usage: READ <start_addr> <num_blocks>");
        return;
    }

    uint32_t start_addr = parse_hex(astr);
    uint32_t num_blocks = parse_dec_or_hex(sstr);

    if (num_blocks == 0 || num_blocks > 0xFFFF) {
        print_err("invalid block count");
        return;
    }

    uint32_t total_bytes = num_blocks * 0x800UL;

    led_reading();
    Serial.print("READ:START addr=0x");
    Serial.print(start_addr, HEX);
    Serial.print(" blocks=");
    Serial.print(num_blocks);
    Serial.print(" bytes=");
    Serial.println(total_bytes);

    uint32_t addr = start_addr;
    uint32_t good_blocks = 0;
    uint32_t checksum = 0;   /* additive checksum of all read data */

    /* Open SD file for writing */
    FsFile   sd_file;
    bool     use_sd = false;
    if (g_sd_ok) {
        char fname[128];  /* fits "/Read/HSREAD_<VIN17>_<OSID16>_<ts13>.bin" */
        if (g_sd_filename[0]) {
            strncpy(fname, g_sd_filename, sizeof(fname) - 1);
            fname[sizeof(fname) - 1] = '\0';
        } else {
            snprintf(fname, sizeof(fname), "read_%06lX.bin", (unsigned long)start_addr);
        }
        sd_file = g_sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
        if (sd_file) {
            if (!sd_file.preAllocate(total_bytes)) {
                Serial.println("WARN: SD pre-allocate failed");
            }
            use_sd = true;
            Serial.print("SD: writing to ");
            Serial.println(fname);
        } else {
            Serial.println("WARN: SD file open failed, serial only");
        }
    }

    /* Select RequestUpload format based on module */
    uint16_t hdr_skip = (g_module == MODULE_E38 || g_module == MODULE_E67) ? E38_ADDR_ECHO_SKIP
                                                  : T87_ADDR_ECHO_SKIP;

    for (uint32_t blk = 0; blk < num_blocks; blk++) {
        uds_msg_t resp;
        int ret;

        if (g_module == MODULE_E38 || g_module == MODULE_E67) {
            /*
             * E38 read: send RequestUpload (0x35 00 08 [addr:4]).
             * The kernel may respond with just 0x36 data directly
             * (no separate 0x75 SF), or 0x75 then 0x36. Handle both.
             */
            uint8_t req[7];
            req[0] = UDS_SID_REQUEST_UPLOAD;
            req[1] = 0x00;
            req[2] = 0x08;
            req[3] = (uint8_t)(addr >> 24);
            req[4] = (uint8_t)(addr >> 16);
            req[5] = (uint8_t)(addr >> 8);
            req[6] = (uint8_t)(addr);

            ret = isotp_send(&g_isotp_link, req, sizeof(req));
            if (ret != ISOTP_RET_OK) {
                led_error();
                if (use_sd) sd_file.close();
                print_err("RequestUpload send failed");
                return;
            }

            /* Accept first response — could be 0x75 or 0x36 */
            ret = uds_receive(&g_isotp_link, &resp, UDS_PENDING_TIMEOUT_MS);
            if (ret != UDS_OK) {
                led_error();
                if (use_sd) sd_file.close();
                Serial.print("ERR: RequestUpload at 0x");
                Serial.print(addr, HEX);
                Serial.print(" ret=");
                Serial.println(ret);
                return;
            }

            /* If we got 0x75 (RequestUpload positive), need another receive for 0x36 */
            if (resp.service == 0x75) {
                ret = uds_receive(&g_isotp_link, &resp, UDS_PENDING_TIMEOUT_MS);
            }
            /* If resp.service == 0x36, we already have the data — fall through */
        } else {
            /* T87: standard request/response */
            ret = uds_gm_request_upload(&g_isotp_link, addr, &resp);
            if (ret != UDS_OK) {
                led_error();
                if (use_sd) sd_file.close();
                Serial.print("ERR: RequestUpload at 0x");
                Serial.print(addr, HEX);
                Serial.print(" ret=");
                Serial.println(ret);
                if (ret == UDS_ERR_NEGATIVE) {
                    Serial.print("  NRC=0x");
                    Serial.println(resp.nrc, HEX);
                }
                return;
            }
            ret = uds_receive(&g_isotp_link, &resp, UDS_PENDING_TIMEOUT_MS);
        }
        if (ret != UDS_OK) {
            if (use_sd) sd_file.close();
            Serial.print("ERR: TransferData receive at 0x");
            Serial.print(addr, HEX);
            Serial.print(" ret=");
            Serial.println(ret);
            return;
        }

        /*
         * Response: service=0x36, sub_function varies
         * T87: data[0..3] = 4-byte address echo, then 0x800 data
         * E38: data[0..2] = 3-byte address echo, then 0x800 data
         */
        uint16_t data_len = (resp.data_len > hdr_skip)
                            ? (resp.data_len - hdr_skip) : 0;
        const uint8_t *flash_data = resp.data + hdr_skip;

        /* Write to SD card (primary data store) */
        if (use_sd && data_len > 0) {
            sd_file.write(flash_data, data_len);
        }

        /* Checksum from actual data buffer */
        for (uint16_t i = 0; i < data_len; i++) {
            checksum += flash_data[i];
        }

        /* Serial: address only (full hex dumps overflow USB CDC buffer) */
        Serial.print("DATA:");
        if (addr < 0x100000) Serial.print('0');
        if (addr < 0x10000) Serial.print('0');
        if (addr < 0x1000) Serial.print('0');
        if (addr < 0x100) Serial.print('0');
        if (addr < 0x10) Serial.print('0');
        Serial.println(addr, HEX);

        good_blocks++;
        addr += 0x0800;

        /* Keep bus alive during long reads */
        send_broadcast_tp_if_due();

        /* Progress every 64 blocks */
        if ((blk & 0x3F) == 0x3F) {
            Serial.print("READ:PROGRESS ");
            Serial.print(blk + 1);
            Serial.print("/");
            Serial.println(num_blocks);
        }
    }

    if (use_sd) {
        sd_file.flush();
        Serial.print("SD: file size ");
        Serial.print(sd_file.size());
        Serial.println(" bytes");
        sd_file.close();
    }

    led_connected();
    Serial.print("READ:DONE blocks=");
    Serial.print(good_blocks);
    Serial.print(" bytes=");
    Serial.print(good_blocks * 0x800UL);
    Serial.print(" checksum=0x");
    Serial.println(checksum, HEX);
    print_ok();
}

/* ---------- BAM (Boot Assist Module) read for T87A ---------- */

/* BAM entry heartbeat: flood 0x7E2 with 27 FB 17 0A 57 every 2ms,
 * wait for 0x7EA response, then send "RAMEXEC" to enter BAM mode.
 * After entry, read flash via cmd 0x04 on CAN IDs 0x026/0x027. */

#define BAM_TX_ID       0x027
#define BAM_RX_ID       0x026
#define BAM_HBEAT_ID    0x7E2
#define BAM_HBEAT_RSP   0x7EA

static bool bam_enter(void)
{
    /* Phase 1: PURPLE LED — "power on TCM now" */
    led_set(128, 0, 200);  /* purple */
    Serial.println("BAM: Purple LED — power on TCM now...");

    /* Flood heartbeat while waiting for ECU response.
     * Last byte is module-specific: 0x57 for T87/T87A, 0x5D for T93.
     *
     * CRITICAL: With the TCM powered off, no node ACKs our frames.
     * The CAN controller's TEC (Transmit Error Counter) increments by 8
     * per failed frame. At TEC > 255, the controller goes bus-off and
     * stops transmitting. At 2ms/frame, bus-off hits in ~64ms.
     * Fix: reinit CAN every 50ms to reset error counters, keeping the
     * heartbeat alive on the wire until the TCM powers on and ACKs. */
    const uint8_t hbeat_algo = (g_module == MODULE_T93) ? 0x5D : 0x57;
    const uint8_t hbeat[] = { 0x27, 0xFB, 0x17, 0x0A, hbeat_algo };
    uint32_t start = millis();
    uint32_t last_tx = 0;
    uint32_t last_reinit = millis();
    bool got_response = false;

    /* Accept all CAN frames during entry */
    can_set_filter(0, 0);

    while (millis() - start < 10000) {  /* 10-second window to power on */
        /* Reinit CAN every 50ms to reset error counters before bus-off */
        if (millis() - last_reinit >= 50) {
            can_init(500000);
            can_set_filter(0, 0);
            last_reinit = millis();
        }

        /* Send heartbeat every 2ms */
        uint32_t now = millis();
        if (now - last_tx >= 2) {
            can_send(BAM_HBEAT_ID, hbeat, 5);
            last_tx = now;
        }

        /* Check for response on 0x7EA */
        uint32_t rx_id;
        uint8_t  rx_data[8];
        uint8_t  rx_len;
        if (can_receive(&rx_id, rx_data, &rx_len) == 0) {
            if (rx_id == BAM_HBEAT_RSP && rx_len >= 5 &&
                rx_data[0] == 0x67 && rx_data[1] == 0xFB) {
                Serial.print("BAM: ECU responded: ");
                for (int i = 0; i < rx_len; i++) {
                    if (rx_data[i] < 0x10) Serial.print('0');
                    Serial.print(rx_data[i], HEX);
                    Serial.print(' ');
                }
                Serial.println();
                got_response = true;
                break;
            }
        }
    }

    if (!got_response) {
        led_set_blink(200, 0, 0, 200);  /* red flash */
        Serial.println("BAM: No ECU response — timeout");
        return false;
    }

    /* Phase 2: Send "RAMEXEC" on 0x7E2 */
    const uint8_t ramexec[] = { 0x52, 0x41, 0x4D, 0x45, 0x58, 0x45, 0x43 };
    can_send(BAM_HBEAT_ID, ramexec, 7);
    Serial.println("BAM: Sent RAMEXEC");

    /* Wait ~150ms for BAM to initialize */
    delay(150);

    /* Phase 3: Check for BAM init frame on 0x026 */
    uint32_t wait_start = millis();
    bool bam_alive = false;
    while (millis() - wait_start < 2000) {
        uint32_t rx_id;
        uint8_t  rx_data[8];
        uint8_t  rx_len;
        if (can_receive(&rx_id, rx_data, &rx_len) == 0) {
            if (rx_id == BAM_RX_ID) {
                Serial.print("BAM: Init frame: ");
                for (int i = 0; i < rx_len; i++) {
                    if (rx_data[i] < 0x10) Serial.print('0');
                    Serial.print(rx_data[i], HEX);
                    Serial.print(' ');
                }
                Serial.println();
                bam_alive = true;
                break;
            }
        }
    }

    if (!bam_alive) {
        led_set_blink(200, 0, 0, 200);  /* red flash */
        Serial.println("BAM: No BAM init frame — failed");
        return false;
    }

    /* Phase 4: Poll + CHIPID sequence */
    uint8_t tx[8], rx[8];
    uint8_t rx_len;
    uint32_t rx_id;
    uint8_t seq = 0;

    /* POLL reg 0x00 */
    memset(tx, 0, 8);
    tx[0] = 0x01; tx[1] = seq++;
    can_send(BAM_TX_ID, tx, 8);
    delay(1);
    can_receive(&rx_id, rx, &rx_len);

    /* CHIPID */
    memset(tx, 0, 8);
    tx[0] = 0x1B; tx[1] = seq++;
    can_send(BAM_TX_ID, tx, 8);
    delay(1);
    if (can_receive(&rx_id, rx, &rx_len) == 0 && rx_id == BAM_RX_ID) {
        Serial.print("BAM: ChipID version: ");
        Serial.print(rx[3], HEX); Serial.print('.'); Serial.println(rx[4], HEX);
    }

    /* Remaining polls (regs 0x02 through 0x24, ~500ms apart like capture) */
    for (int reg = 2; reg <= 0x24; reg++) {
        memset(tx, 0, 8);
        tx[0] = 0x01; tx[1] = seq++;
        can_send(BAM_TX_ID, tx, 8);
        /* Small delay between polls — not 500ms like EXTKERN, faster is fine */
        delay(5);
        can_receive(&rx_id, rx, &rx_len);  /* drain response */
    }

    /* INIT command */
    memset(tx, 0, 8);
    tx[0] = 0x02; tx[1] = seq++;
    tx[5] = 0x02;  /* flags=0x02 from capture */
    can_send(BAM_TX_ID, tx, 8);
    delay(5);
    can_receive(&rx_id, rx, &rx_len);

    led_set(0, 200, 0);  /* green — connected */
    Serial.println("BAM: Connected — ready to read");
    return true;
}

static int bam_send_recv(uint8_t *tx, uint8_t *rx, uint16_t timeout_ms)
{
    can_send(BAM_TX_ID, tx, 8);
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        uint32_t rx_id;
        uint8_t  rx_len;
        if (can_receive(&rx_id, rx, &rx_len) == 0 && rx_id == BAM_RX_ID) {
            return rx_len;
        }
    }
    return -1;
}

/* ---------- BAM sector table for SPC564A80 ---------- */
/* Sector start offsets and UNLOCK masks derived from capture analysis.
 * BAM addresses start at flash 0x020000 (OS region). */
struct bam_sector_t {
    uint32_t offset;      /* byte offset from BAM base (= flash 0x020000) */
    uint32_t size;        /* sector size in bytes */
    uint32_t unlock_mask; /* UNLOCK byte[3:5] value */
    uint32_t init_addr;   /* INIT bytes[4:8] base address */
};

/* Full 4MB flash = 0x400000 total, BAM sees 0x020000-0x3FFFFF = 0x3E0000 (3.875 MB)
 * Sector map reconstructed from BAM_Mode_Write_1.csv capture */
static const bam_sector_t BAM_SECTORS[] = {
    { 0x000000,  0x4000, 0x00004000, 0x00000000 },  /*  0: 16KB  */
    { 0x004000,  0x4000, 0x00004000, 0x00004000 },  /*  1: 16KB  */
    { 0x008000,  0x4000, 0x00004000, 0x00008000 },  /*  2: 16KB  */
    { 0x00C000,  0x4000, 0x00004000, 0x0000C000 },  /*  3: 16KB  */
    { 0x010000,  0x4000, 0x00004000, 0x00010000 },  /*  4: 16KB  */
    { 0x014000,  0x4000, 0x00004000, 0x00014000 },  /*  5: 16KB  */
    { 0x018000, 0x10000, 0x00010000, 0x00018000 },  /*  6: 64KB  */
    { 0x028000, 0x20000, 0x00020000, 0x00028000 },  /*  7: 128KB */
    { 0x048000, 0x20000, 0x00020000, 0x00048000 },  /*  8: 128KB */
    { 0x068000, 0x40000, 0x00040000, 0x00068000 },  /*  9: 256KB */
    { 0x0A8000, 0x40000, 0x00040000, 0x000A8000 },  /* 10: 256KB */
    { 0x0E8000, 0x40000, 0x00040000, 0x000E8000 },  /* 11: 256KB */
    { 0x128000, 0x40000, 0x00040000, 0x00128000 },  /* 12: 256KB */
    { 0x168000, 0x40000, 0x00040000, 0x00168000 },  /* 13: 256KB */
    { 0x1A8000, 0x40000, 0x00040000, 0x001A8000 },  /* 14: 256KB */
    { 0x1E8000, 0x40000, 0x00040000, 0x001E8000 },  /* 15: 256KB */
    { 0x228000, 0x40000, 0x00040000, 0x00228000 },  /* 16: 256KB */
    { 0x268000, 0x40000, 0x00040000, 0x00268000 },  /* 17: 256KB */
    { 0x2A8000, 0x40000, 0x00040000, 0x002A8000 },  /* 18: 256KB */
    { 0x2E8000, 0x40000, 0x00040000, 0x002E8000 },  /* 19: 256KB */
    { 0x328000, 0x40000, 0x00040000, 0x00328000 },  /* 20: 256KB */
    { 0x368000, 0x40000, 0x00040000, 0x00368000 },  /* 21: 256KB */
    { 0x3A8000, 0x40000, 0x00040000, 0x003A8000 },  /* 22: 256KB */
};
#define BAM_NUM_SECTORS (sizeof(BAM_SECTORS) / sizeof(BAM_SECTORS[0]))
#define BAM_TOTAL_SIZE  0x3E8000UL  /* 3.875 MB = total BAM-accessible flash */

static bool bam_write_sector(const uint8_t *data, uint32_t size,
                             uint32_t unlock_mask, uint32_t init_addr,
                             uint8_t &seq)
{
    uint8_t tx[8], rx[8];

    /* INIT (set current address) */
    memset(tx, 0, 8);
    tx[0] = 0x02; tx[1] = seq++;
    tx[4] = (init_addr >> 24) & 0xFF;
    tx[5] = (init_addr >> 16) & 0xFF;
    tx[6] = (init_addr >> 8) & 0xFF;
    tx[7] = init_addr & 0xFF;
    if (bam_send_recv(tx, rx, 500) < 0) return false;

    /* UNLOCK */
    memset(tx, 0, 8);
    tx[0] = 0x0E; tx[1] = seq++;
    tx[3] = (unlock_mask >> 16) & 0xFF;
    tx[4] = (unlock_mask >> 8) & 0xFF;
    tx[5] = unlock_mask & 0xFF;
    if (bam_send_recv(tx, rx, 30000) < 0) return false;  /* erase can take time */
    delay(5);

    /* INIT (clear) */
    memset(tx, 0, 8);
    tx[0] = 0x02; tx[1] = seq++;
    if (bam_send_recv(tx, rx, 500) < 0) return false;

    /* Write data: 10 frames of 6 bytes = 60 bytes per block, then VERIFY */
    uint32_t pos = 0;
    while (pos < size) {
        /* Write 10 frames (60 bytes) */
        for (int f = 0; f < 10 && pos < size; f++) {
            memset(tx, 0, 8);
            tx[0] = 0x23;
            tx[1] = seq++;
            uint8_t remaining = (size - pos > 6) ? 6 : (uint8_t)(size - pos);
            for (uint8_t b = 0; b < remaining; b++) {
                tx[2 + b] = data[pos++];
            }
            if (bam_send_recv(tx, rx, 500) < 0) return false;
        }

        /* VERIFY after each 60-byte block */
        memset(tx, 0, 8);
        tx[0] = 0x03;
        tx[1] = seq++;
        tx[2] = 0x04;
        /* Copy last few data bytes into verify for comparison */
        if (bam_send_recv(tx, rx, 500) < 0) return false;
    }

    /* CMD18 — sector commit/finalize */
    memset(tx, 0, 8);
    tx[0] = 0x18;
    tx[1] = seq++;
    /* byte[2] varies by sector size: 0x03 for 16KB, 0x02 for 64KB, 0x06 for 128KB, 0x08 for 256KB */
    if (size <= 0x4000)       tx[2] = 0x03;
    else if (size <= 0x10000) tx[2] = 0x02;
    else if (size <= 0x20000) tx[2] = 0x06;
    else                      tx[2] = 0x08;
    if (bam_send_recv(tx, rx, 500) < 0) return false;

    return true;
}

static bool bam_read_flash(uint8_t *buf, uint32_t size, uint8_t &seq)
{
    uint8_t tx[8], rx[8];
    uint32_t pos = 0;

    while (pos < size) {
        uint8_t count;
        /* 12 frames of 5 bytes + 1 frame of 4 bytes = 64 bytes per group */
        uint32_t block_pos = pos % 64;
        if (block_pos + 5 >= 64)
            count = (uint8_t)(64 - block_pos);
        else
            count = 5;
        if (pos + count > size)
            count = (uint8_t)(size - pos);

        memset(tx, 0, 8);
        tx[0] = 0x04;
        tx[1] = seq++;
        tx[2] = count;

        int rlen = bam_send_recv(tx, rx, 500);
        if (rlen < 0) return false;

        for (uint8_t i = 0; i < count && pos < size; i++) {
            buf[pos++] = rx[3 + i];
        }
    }
    return true;
}

/* ---------- T87A HS-CAN Write ----------
 * Uses external kernel (must be running) to erase + write flash.
 * Reads source data from SD card file.
 * Boot sector (0x020000-0x03FFFF) is protected — not erased or written. */
/* ---------- T87A CAL Write — HS-CAN cal-only write via Rollin Smoke kernel
 *
 * Writes the 1 MB cal region (0x080000-0x17FFFF) to T87A flash. Strict
 * subset of HSWRITE — never touches boot / NVM / boot-block. Accepts
 * either a 1 MB cal-only source file (byte 0 = flash 0x080000) or a
 * 4 MB full-flash source (byte 0x080000 = flash 0x080000).
 *
 * Safety: erase + write only touch the 3 cal sectors
 *   0x080000 (256 KB M1 LMSR)
 *   0x0C0000 (256 KB M1 LMSR)
 *   0x100000 (512 KB HSR dual M0+M1)
 * Boot sector (0x000000-0x03FFFF) and OS region (0x180000-0x3FFFFF)
 * are untouched. Worst case rollback = BAMWRITE the prior HSREAD image. */
static void cmd_t87a_calwrite(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (!g_sd_ok) { print_err("no SD card"); return; }

    const char *fname = (arg && arg[0]) ? arg :
                        (g_sd_filename[0] ? g_sd_filename : NULL);
    if (!fname) {
        print_err("usage: CALWRITE <path> — try /Read/CALREAD_<osid>_<ts>.bin");
        return;
    }

    FsFile src = g_sd.open(fname, O_RDONLY);
    if (!src) {
        Serial.print("ERR: cannot open "); Serial.println(fname);
        return;
    }
    uint32_t file_size = src.size();

    /* Accept 1 MB (cal-only) or 4 MB (full-flash); everything else rejected */
    uint32_t src_cal_offset = 0;
    if (file_size == 0x100000UL) {
        src_cal_offset = 0;
    } else if (file_size == 0x400000UL) {
        src_cal_offset = 0x080000;
    } else {
        Serial.print("ERR: file size must be 1 MB (cal-only) or 4 MB (full-flash), got ");
        Serial.println(file_size);
        src.close();
        return;
    }

    Serial.print("CALWRITE: source ");
    Serial.print(fname);
    Serial.print(" (");
    Serial.print(file_size);
    Serial.print(" B, cal offset=0x");
    Serial.print(src_cal_offset, HEX);
    Serial.println(")");

    /* Bring up the T87A kernel if not already running (same flow CALREAD uses). */
    if (!g_extkern_active) {
        Serial.println("CALWRITE: bringing up T87A kernel...");
        g_tester_id = 0x7E2;
        g_ecu_id    = 0x7EA;
        seedkey_set_algo(SEEDKEY_T87);
        isotp_init_link(&g_isotp_link, g_tester_id,
                        g_isotp_send_buf, sizeof(g_isotp_send_buf),
                        g_isotp_recv_buf, sizeof(g_isotp_recv_buf));
        g_module = MODULE_T87;

        t87_broadcast_reset();

        bool have_vin  = read_vin_from_ecu();
        bool have_osid = read_osid_from_ecu();
        if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
        if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin);  }

        t87_broadcast_disable_comms();

        if (!do_security_access()) {
            print_err("CALWRITE: security access denied");
            src.close();
            return;
        }
        if (!g_t87a_detected) {
            print_err("CALWRITE: not a T87A — aborting");
            src.close();
            return;
        }

        t87_enter_programming_mode();
        delay(100);

        if (!hsread_upload_rollin_smoke()) {
            print_err("CALWRITE: kernel upload failed");
            src.close();
            return;
        }
        g_extkern_active = true;
        delay(200);
    }

    Serial.println("CALWRITE:WARNING *** DO NOT POWER DOWN UNTIL COMPLETE ***");

    /* Unlock flash (required before erase/write) */
    Serial.println("CALWRITE: unlocking flash...");
    t87a_flash_unlock();

    /* Let kernel settle — HSWRITE capture showed ~2 s of heartbeats */
    delay(2000);
    { uint32_t rid; uint8_t rd[8]; uint8_t rl;
      while (can_receive(&rid, rd, &rl) == 0) {} }

    led_reading();  /* blue during erase */
    g_reading_active = true;

    /* Erase cal sectors only: addr in [0x080000, 0x180000) */
    Serial.println("CALWRITE: erasing cal sectors (0x080000-0x17FFFF)...");
    uint32_t erase_start = millis();
    int erased = 0;
    for (uint32_t s = 0; s < T87A_NUM_SECTORS; s++) {
        const t87a_sector &sec = T87A_SECTORS[s];
        if (sec.addr < 0x080000 || sec.addr >= 0x180000) continue;

        Serial.print("CALWRITE:ERASE 0x"); Serial.print(sec.addr, HEX);
        Serial.print(" mod=0x"); Serial.print(sec.mod_base, HEX);
        if (!t87a_erase_sector(sec.addr, sec.sel, sec.mod_base, sec.use_hsr)) {
            led_error();
            Serial.println(" FAILED");
            src.close();
            g_reading_active = false;
            return;
        }
        Serial.println(" OK");
        erased++;
    }
    uint32_t erase_ms = millis() - erase_start;
    Serial.print("CALWRITE: erase complete — ");
    Serial.print(erased);
    Serial.print(" sectors in ");
    Serial.print(erase_ms / 1000);
    Serial.println("s");

    /* Write 4 KB blocks. Address range 0x080000 to 0x17FFFF (256 blocks). */
    led_writing();
    Serial.println("CALWRITE: writing flash...");
    uint8_t block_buf[0x1000];
    uint32_t blocks_written = 0;
    uint32_t blocks_skipped = 0;
    uint32_t write_start = millis();
    uint32_t addr     = 0x080000;
    uint32_t end_addr = 0x180000;
    uint32_t total_blocks = (end_addr - addr) / 0x1000;

    src.seekSet(src_cal_offset);

    while (addr < end_addr) {
        int rd = src.read(block_buf, 0x1000);
        if (rd < 0x1000) {
            memset(block_buf + rd, 0xFF, 0x1000 - rd);
        }

        /* Skip all-0xFF blocks — already erased, no write needed */
        bool all_ff = true;
        for (int i = 0; i < 0x1000; i++) {
            if (block_buf[i] != 0xFF) { all_ff = false; break; }
        }
        if (all_ff) {
            addr += 0x1000;
            blocks_skipped++;
            continue;
        }

        if (!extkern_write_block(addr, block_buf, 0x1000, 10000)) {
            led_error();
            Serial.print("CALWRITE:ERR write failed at 0x");
            Serial.println(addr, HEX);
            src.close();
            g_reading_active = false;
            return;
        }

        addr += 0x1000;
        blocks_written++;

        if (((blocks_written + blocks_skipped) % 32) == 0) {
            Serial.print("CALWRITE:PROGRESS ");
            Serial.print(blocks_written + blocks_skipped);
            Serial.print("/");
            Serial.print(total_blocks);
            Serial.print(" (wrote=");
            Serial.print(blocks_written);
            Serial.print(" skipped_ff=");
            Serial.print(blocks_skipped);
            Serial.println(")");
        }
    }

    src.close();
    uint32_t write_ms = millis() - write_start;
    Serial.print("CALWRITE:DONE wrote=");
    Serial.print(blocks_written);
    Serial.print(" skipped_ff=");
    Serial.print(blocks_skipped);
    Serial.print(" time=");
    Serial.print(write_ms / 1000);
    Serial.println("s");

    g_reading_active = false;
    led_celebrate();
    led_connected();

    /* IMPORTANT: do NOT run KEXIT after a write. Bench test 2026-04-22 showed
     * the Rollin Smoke kernel enters a fundamentally different state after a
     * successful flash write session:
     *   - kernel bounds-checks $A0 reads to flash size (4 MB) post-write
     *   - 16 MB kill stream plateaus at 4 MB instead of overshooting
     *   - kernel never fires its blr exit path, stays silent-unresponsive
     *   - further poking CAN corrupt flash and brick the TCM
     * Writes end by telling the user to power-cycle. Matches every proprietary
     * flash tool's behavior. See memory/t87a_kernel_exit_followup.md. */
    g_auto_broadcast = false;
    g_extkern_active = false;
    Serial.println();
    Serial.println("==========================================================");
    Serial.println("CALWRITE:DONE — POWER-CYCLE the TCM now to restore the OS.");
    Serial.println("  Do NOT attempt Reset / KEXIT — post-write kernel state is");
    Serial.println("  unstable and further commands can brick the TCM.");
    Serial.println("==========================================================");
    print_ok();
}

static void cmd_t87a_hswrite(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (!g_sd_ok) { print_err("no SD card"); return; }

    /* Open source file from SD */
    const char *fname = (arg && arg[0]) ? arg :
                        (g_sd_filename[0] ? g_sd_filename : NULL);
    if (!fname) {
        print_err("usage: HSWRITE <path> — e.g. /Write/T87A_OS-XXXXXXXX_Unlock_Patched.bin");
        return;
    }

    FsFile src = g_sd.open(fname, O_RDONLY);
    if (!src) {
        Serial.print("ERR: cannot open ");
        Serial.println(fname);
        return;
    }
    uint32_t file_size = src.size();
    Serial.print("HSWRITE: source file ");
    Serial.print(fname);
    Serial.print(" (");
    Serial.print(file_size);
    Serial.println(" bytes)");

    if (file_size < 0x100000) {
        print_err("file too small for T87A flash");
        src.close();
        return;
    }

    /* Read source-file OS PN from 0x014638 (GM T87A layout — same offset
     * tools/t87a_patch.py uses) for the pre-write cross-OS safety check.
     * Only meaningful for full 4 MB images. */
    uint32_t src_os_pn = 0;
    if (file_size >= 0x01463C) {
        src.seekSet(0x014638);
        uint8_t pn_bytes[4];
        if (src.read(pn_bytes, 4) == 4) {
            src_os_pn = ((uint32_t)pn_bytes[0] << 24) |
                        ((uint32_t)pn_bytes[1] << 16) |
                        ((uint32_t)pn_bytes[2] << 8)  |
                         (uint32_t)pn_bytes[3];
        }
        src.seekSet(0);  /* rewind for the write loop below */
    }

    /* PRE-BRING-UP cross-OS safety check: read TCM OSID via a plain UDS
     * $09 04 request (no auth, no kernel needed) and compare against
     * source bin's OS PN. If mismatch, abort BEFORE loading the kernel
     * so the TCM stays untouched — no residual kernel, no power-cycle
     * needed. If the pre-check can't read OSID (e.g., kernel already
     * resident from a prior session), we fall through and do the
     * post-bring-up check later. */
    if (src_os_pn != 0 && !g_extkern_active) {
        /* Temporarily set T87A IDs so the UDS call goes to the right ECU */
        g_tester_id = 0x7E2;
        g_ecu_id    = 0x7EA;
        isotp_init_link(&g_isotp_link, g_tester_id,
                        g_isotp_send_buf, sizeof(g_isotp_send_buf),
                        g_isotp_recv_buf, sizeof(g_isotp_recv_buf));
        Serial.println("HSWRITE: pre-checking TCM OSID (no kernel upload yet)...");
        bool pre_have_osid = read_osid_from_ecu();
        if (pre_have_osid && g_osid[0]) {
            uint32_t pre_tcm_os_pn = (uint32_t)strtoul(g_osid, NULL, 10);
            if (pre_tcm_os_pn != 0 && src_os_pn != pre_tcm_os_pn) {
                Serial.println();
                Serial.println("====================================================================");
                Serial.println("HSWRITE: CROSS-OS MISMATCH (pre-check) — ABORTING");
                Serial.print("  Source bin OS PN (offset 0x014638): ");
                Serial.println(src_os_pn);
                Serial.print("  Current TCM OSID (via Mode 9):      ");
                Serial.println(g_osid);
                Serial.println();
                Serial.println("  No kernel loaded, no flash touched — TCM untouched.");
                Serial.println("  Flashy HSWRITE is for SAME-OS writes (cal edits).");
                Serial.println("  For cross-OS swaps, use JTAG full-flash.");
                Serial.println("====================================================================");
                src.close();
                return;
            }
            /* Pre-check passed — fall through to bring-up + write */
        }
    }

    /* Bring up the T87A kernel if not already running (mirrors CALWRITE). */
    if (!g_extkern_active) {
        Serial.println("HSWRITE: bringing up T87A kernel...");
        g_tester_id = 0x7E2;
        g_ecu_id    = 0x7EA;
        seedkey_set_algo(SEEDKEY_T87);
        isotp_init_link(&g_isotp_link, g_tester_id,
                        g_isotp_send_buf, sizeof(g_isotp_send_buf),
                        g_isotp_recv_buf, sizeof(g_isotp_recv_buf));
        g_module = MODULE_T87;

        t87_broadcast_reset();

        bool have_vin  = read_vin_from_ecu();
        bool have_osid = read_osid_from_ecu();
        if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
        if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin);  }

        t87_broadcast_disable_comms();

        if (!do_security_access()) {
            print_err("HSWRITE: security access denied");
            src.close();
            return;
        }
        if (!g_t87a_detected) {
            print_err("HSWRITE: not a T87A — aborting");
            src.close();
            return;
        }

        t87_enter_programming_mode();
        delay(100);

        if (!hsread_upload_rollin_smoke()) {
            print_err("HSWRITE: kernel upload failed");
            src.close();
            return;
        }
        g_extkern_active = true;
        delay(200);
    }

    /* CROSS-OS SAFETY GATE — refuse to write if source OS PN doesn't match
     * the currently-reported TCM OSID. Writing a different OS over the
     * existing boot block causes a boot-OS mismatch brick (proven 2026-04-22).
     * Same-OS writes pass this check and proceed; cross-OS swaps must use
     * JTAG full-flash instead (which writes matching boot + OS together).
     * Only checked when source is a full 4 MB image — cal-only bins skip. */
    if (src_os_pn != 0 && g_osid[0]) {
        uint32_t tcm_os_pn = (uint32_t)strtoul(g_osid, NULL, 10);
        if (tcm_os_pn != 0 && src_os_pn != tcm_os_pn) {
            Serial.println();
            Serial.println("====================================================================");
            Serial.println("HSWRITE: CROSS-OS MISMATCH — ABORTING");
            Serial.print("  Source bin OS PN (offset 0x014638): ");
            Serial.println(src_os_pn);
            Serial.print("  Current TCM OSID (via Mode 9):      ");
            Serial.println(g_osid);
            Serial.println();
            Serial.println("  Writing a DIFFERENT OS over this TCM preserves the old boot");
            Serial.println("  block, which has OS-specific signature/CRC anchors — boot will");
            Serial.println("  reject the new OS and the TCM will BRICK on reboot.");
            Serial.println();
            Serial.println("  Flashy HSWRITE is for SAME-OS writes (cal edits).");
            Serial.println("  For cross-OS swaps, use JTAG full-flash.");
            Serial.println("====================================================================");
            src.close();
            return;
        }
        if (tcm_os_pn != 0) {
            Serial.print("HSWRITE: OS PN match confirmed (");
            Serial.print(tcm_os_pn);
            Serial.println(") — safe to proceed");
        }
    } else if (src_os_pn == 0) {
        Serial.println("HSWRITE:WARN source OS PN unreadable — skipping cross-OS check");
    }

    Serial.println("HSWRITE:WARNING *** DO NOT POWER DOWN UNTIL COMPLETE ***");

    /* Step 1: Flash unlock */
    Serial.println("HSWRITE: unlocking flash...");
    t87a_flash_unlock();

    /* Step 1b: MCU ID reads (from capture frames 3083-3090) */
    {
        uint8_t rd[8] = {0x87, 0xC3, 0xF9, 0x00, 0x04, 0x00, 0x00, 0x04};
        can_send(g_tester_id, rd, 8); delay(5);
        rd[4] = 0x00; can_send(g_tester_id, rd, 8); delay(5);
        rd[1] = 0xC3; rd[2] = 0xF8; rd[3] = 0x80; rd[4] = 0x00;
        can_send(g_tester_id, rd, 8); delay(5);
        rd[3] = 0xC0; can_send(g_tester_id, rd, 8); delay(5);
    }

    /* Step 1c: Wait for kernel to settle — capture shows ~10s of heartbeats */
    Serial.println("HSWRITE: waiting for kernel settle...");
    delay(2000);
    /* Drain any queued heartbeat frames */
    { uint32_t rid; uint8_t rd[8]; uint8_t rl;
      while (can_receive(&rid, rd, &rl) == 0) {} }

    /* Step 2: Erase sectors — skip 0x000000-0x03FFFF (boot + NVM + boot block) */
    Serial.println("HSWRITE: erasing sectors...");
    led_reading();  /* blue during erase */
    g_reading_active = true;

    uint32_t start_ms = millis();
    for (uint32_t s = 0; s < T87A_NUM_SECTORS; s++) {
        const t87a_sector &sec = T87A_SECTORS[s];

        /* Skip boot sector + NVM + boot block (0x000000-0x03FFFF) */
        if (sec.addr < 0x040000) continue;

        Serial.print("HSWRITE:ERASE 0x");
        Serial.print(sec.addr, HEX);

        /* Erase sector — dual-module sectors are separate entries in table */
        if (!t87a_erase_sector(sec.addr, sec.sel, sec.mod_base, sec.use_hsr)) {
            led_error();
            Serial.print(" FAILED");
            Serial.println();
            src.close();
            g_reading_active = false;
            return;
        }
        Serial.println(" OK");
    }

    uint32_t erase_ms = millis() - start_ms;
    Serial.print("HSWRITE: erase complete (");
    Serial.print(erase_ms / 1000);
    Serial.println("s)");

    /* Step 3: Write blocks (4096 bytes each via $6C) */
    Serial.println("HSWRITE: writing flash...");
    uint8_t block_buf[0x1000];
    uint32_t blocks_written = 0;
    uint32_t write_start = millis();
    uint32_t addr = 0;

    src.seekSet(0);

    /* T87A: skip lower blocks (boot sector + NVM) and boot block region.
     * Start writing at 0x040000. Source file is full flash format. */
    if (g_t87a_detected && addr < 0x040000) {
        src.seekSet(0x040000);
        addr = 0x040000;
    }

    while (addr < file_size && addr < 0x400000) {
        /* Skip boot sector + NVM + boot block region */
        if (addr < 0x040000) {
            src.seekSet(0x040000);
            addr = 0x040000;
            continue;
        }

        int rd = src.read(block_buf, 0x1000);
        if (rd < 0x1000) {
            /* Pad with 0xFF if short read */
            memset(block_buf + rd, 0xFF, 0x1000 - rd);
        }

        /* Skip all-0xFF blocks (already erased) */
        bool all_ff = true;
        for (int i = 0; i < 0x1000; i++) {
            if (block_buf[i] != 0xFF) { all_ff = false; break; }
        }
        if (all_ff) {
            addr += 0x1000;
            blocks_written++;
            continue;
        }

        if (!extkern_write_block(addr, block_buf, 0x1000, 10000)) {
            led_error();
            Serial.print("HSWRITE:ERR write failed at 0x");
            Serial.println(addr, HEX);
            src.close();
            g_reading_active = false;
            return;
        }

        blocks_written++;
        addr += 0x1000;

        if ((blocks_written & 0x3F) == 0) {
            uint32_t elapsed = (millis() - write_start) / 1000;
            uint32_t kb = blocks_written * 4;
            Serial.print("HSWRITE:PROGRESS ");
            Serial.print(kb);
            Serial.print("/");
            Serial.print(file_size / 1024);
            Serial.print(" KB  ");
            Serial.print(elapsed);
            Serial.println("s");
        }
    }

    src.close();
    g_reading_active = false;

    uint32_t total_s = (millis() - start_ms) / 1000;
    Serial.print("HSWRITE:DONE blocks=");
    Serial.print(blocks_written);
    Serial.print(" time=");
    Serial.print(total_s);
    Serial.println("s");

    /* Do NOT run KEXIT after write — post-write kernel state is unstable.
     * Same rationale as CALWRITE. User must power-cycle to restore OS. */
    g_auto_broadcast = false;
    g_extkern_active = false;

    led_celebrate();
    Serial.println();
    Serial.println("==========================================================");
    Serial.println("HSWRITE:DONE — POWER-CYCLE the TCM now to restore the OS.");
    Serial.println("  Do NOT attempt Reset / KEXIT — post-write kernel state is");
    Serial.println("  unstable and further commands can brick the TCM.");
    Serial.println("==========================================================");
    print_ok();
}

static void cmd_bamwrite(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (!g_sd_ok) { print_err("no SD card"); return; }

    /* Default write file — user can change g_sd_filename before calling */
    const char *fname = g_sd_filename[0] ? g_sd_filename : "BAM_T87A_read.bin";

    FsFile src = g_sd.open(fname, O_RDONLY);
    if (!src) {
        Serial.print("ERR: cannot open ");
        Serial.println(fname);
        return;
    }
    uint32_t file_size = src.size();
    Serial.print("BAMWRITE: source file ");
    Serial.print(fname);
    Serial.print(" (");
    Serial.print(file_size);
    Serial.println(" bytes)");

    if (file_size > BAM_TOTAL_SIZE) file_size = BAM_TOTAL_SIZE;

    /* Step 1: Enter BAM mode */
    Serial.println("BAMWRITE: Entering BAM mode...");
    if (!bam_enter()) {
        src.close();
        print_err("BAM entry failed");
        return;
    }

    can_set_filter(BAM_RX_ID, 0x7FF);

    bool write_ok = false;
    int attempt = 0;
    const int MAX_ATTEMPTS = 3;

    while (!write_ok && attempt < MAX_ATTEMPTS) {
        attempt++;
        if (attempt > 1) {
            Serial.print("BAMWRITE: Retry attempt ");
            Serial.println(attempt);
            /* Yellow flash for 5 seconds before retry */
            led_set_blink(200, 200, 0, 300);
            delay(5000);
        }

        led_set_blink(255, 100, 0, 250);  /* orange flash = writing */
        Serial.println("BAMWRITE: Writing flash...");

        uint8_t seq = 0x25;  /* starting sequence (matches captures) */
        uint32_t total_written = 0;
        uint32_t start_ms = millis();
        bool sector_fail = false;

        /* Write each sector */
        for (uint32_t si = 0; si < BAM_NUM_SECTORS && !sector_fail; si++) {
            const bam_sector_t &sec = BAM_SECTORS[si];
            if (sec.offset >= file_size) break;

            uint32_t write_size = sec.size;
            if (sec.offset + write_size > file_size)
                write_size = file_size - sec.offset;

            /* Read sector data from SD file */
            uint8_t chunk[256];  /* read in small chunks to save RAM */
            src.seek(sec.offset);

            /* INIT + UNLOCK + INIT */
            uint8_t tx[8], rx[8];

            /* INIT (set address) */
            memset(tx, 0, 8);
            tx[0] = 0x02; tx[1] = seq++;
            tx[4] = (sec.init_addr >> 24) & 0xFF;
            tx[5] = (sec.init_addr >> 16) & 0xFF;
            tx[6] = (sec.init_addr >> 8) & 0xFF;
            tx[7] = sec.init_addr & 0xFF;
            if (bam_send_recv(tx, rx, 500) < 0) { sector_fail = true; break; }

            /* UNLOCK */
            memset(tx, 0, 8);
            tx[0] = 0x0E; tx[1] = seq++;
            tx[3] = (sec.unlock_mask >> 16) & 0xFF;
            tx[4] = (sec.unlock_mask >> 8) & 0xFF;
            tx[5] = sec.unlock_mask & 0xFF;
            if (bam_send_recv(tx, rx, 30000) < 0) { sector_fail = true; break; }

            /* INIT (clear) */
            memset(tx, 0, 8);
            tx[0] = 0x02; tx[1] = seq++;
            if (bam_send_recv(tx, rx, 500) < 0) { sector_fail = true; break; }

            /* Write data in 60-byte blocks */
            uint32_t sec_pos = 0;
            while (sec_pos < write_size) {
                /* Read up to 60 bytes from SD */
                uint32_t chunk_len = write_size - sec_pos;
                if (chunk_len > 60) chunk_len = 60;
                src.read(chunk, chunk_len);

                /* Write 10 frames × 6 bytes */
                uint32_t cpos = 0;
                for (int f = 0; f < 10 && cpos < chunk_len; f++) {
                    memset(tx, 0, 8);
                    tx[0] = 0x23;
                    tx[1] = seq++;
                    uint8_t nbytes = (chunk_len - cpos > 6) ? 6 : (uint8_t)(chunk_len - cpos);
                    memcpy(&tx[2], &chunk[cpos], nbytes);
                    cpos += nbytes;
                    if (bam_send_recv(tx, rx, 500) < 0) { sector_fail = true; break; }
                }
                if (sector_fail) break;

                /* VERIFY */
                memset(tx, 0, 8);
                tx[0] = 0x03;
                tx[1] = seq++;
                tx[2] = 0x04;
                if (bam_send_recv(tx, rx, 500) < 0) { sector_fail = true; break; }

                sec_pos += chunk_len;
                total_written += chunk_len;
            }
            if (sector_fail) break;

            /* CMD18 — sector finalize */
            memset(tx, 0, 8);
            tx[0] = 0x18; tx[1] = seq++;
            if (sec.size <= 0x4000)       tx[2] = 0x03;
            else if (sec.size <= 0x10000) tx[2] = 0x02;
            else if (sec.size <= 0x20000) tx[2] = 0x06;
            else                          tx[2] = 0x08;
            if (bam_send_recv(tx, rx, 500) < 0) { sector_fail = true; break; }

            /* Progress */
            uint32_t elapsed = (millis() - start_ms) / 1000;
            Serial.print("BAMWRITE:SECTOR ");
            Serial.print(si);
            Serial.print(" done (");
            Serial.print(total_written / 1024);
            Serial.print("/");
            Serial.print(file_size / 1024);
            Serial.print(" KB) ");
            Serial.print(elapsed);
            Serial.println("s");

            led_update();
        }

        if (sector_fail) {
            Serial.println("BAMWRITE: Write failed — sector error");
            continue;  /* retry */
        }

        uint32_t write_elapsed = (millis() - start_ms) / 1000;
        Serial.print("BAMWRITE: Write complete (");
        Serial.print(total_written / 1024);
        Serial.print(" KB in ");
        Serial.print(write_elapsed);
        Serial.println("s)");

        /* Step 3: Readback verify */
        Serial.println("BAMWRITE: Readback verify...");
        led_set_blink(0, 100, 200, 250);  /* cyan flash = reading */

        /* Transition to read mode — matches bus capture:
         * POLL, then INIT with byte[5]=0x02 (read mode flag) */
        uint8_t tx2[8], rx2[8];
        memset(tx2, 0, 8);
        tx2[0] = 0x01; tx2[1] = seq++;
        bam_send_recv(tx2, rx2, 500);

        memset(tx2, 0, 8);
        tx2[0] = 0x02; tx2[1] = seq++;
        tx2[5] = 0x02;  /* read mode flag */
        bam_send_recv(tx2, rx2, 500);

        uint32_t verify_errors = 0;
        uint32_t verify_pos = 0;
        uint8_t  verify_buf[64];
        uint8_t  source_buf[64];

        src.seek(0);

        /* BAM reads AND writes both start at flash 0x000000 (confirmed via
         * NT-Link capture 2026-04-19). Source files are full-flash 4 MB,
         * so source byte 0 == flash byte 0 — no skip needed. The earlier
         * 0x020000 skip caused every readback-verify to fail with
         * ~all-bytes-mismatched, tripping the 3-attempt retry loop. */

        while (verify_pos < file_size) {
            uint32_t chunk_len = file_size - verify_pos;
            if (chunk_len > 64) chunk_len = 64;

            /* Read 64 bytes from flash via BAM */
            uint32_t vpos = 0;
            while (vpos < chunk_len) {
                uint8_t count;
                if (vpos + 5 >= 64)
                    count = (uint8_t)(64 - vpos);
                else
                    count = 5;
                if (vpos + count > chunk_len)
                    count = (uint8_t)(chunk_len - vpos);

                memset(tx2, 0, 8);
                tx2[0] = 0x04; tx2[1] = seq++;  tx2[2] = count;
                int rlen = bam_send_recv(tx2, rx2, 500);
                if (rlen < 0) {
                    Serial.println("BAMWRITE: Verify read timeout");
                    verify_errors = 0xFFFFFFFF;
                    break;
                }
                for (uint8_t b = 0; b < count; b++)
                    verify_buf[vpos++] = rx2[3 + b];
            }
            if (verify_errors == 0xFFFFFFFF) break;

            /* Compare against source file */
            src.read(source_buf, chunk_len);
            for (uint32_t b = 0; b < chunk_len; b++) {
                if (verify_buf[b] != source_buf[b]) {
                    verify_errors++;
                    if (verify_errors <= 5) {
                        Serial.print("BAMWRITE:MISMATCH at 0x");
                        Serial.print(verify_pos + b, HEX);
                        Serial.print(" wrote=0x");
                        Serial.print(source_buf[b], HEX);
                        Serial.print(" read=0x");
                        Serial.println(verify_buf[b], HEX);
                    }
                }
            }
            verify_pos += chunk_len;

            if ((verify_pos & 0xFFFF) == 0) {
                Serial.print("BAMWRITE:VERIFY ");
                Serial.print(verify_pos / 1024);
                Serial.print("/");
                Serial.print(file_size / 1024);
                Serial.println(" KB");
                led_update();
            }
        }

        if (verify_errors == 0) {
            write_ok = true;
            Serial.println("BAMWRITE: Verify PASSED — zero errors");
        } else {
            Serial.print("BAMWRITE: Verify FAILED — ");
            Serial.print(verify_errors);
            Serial.println(" errors");
        }
    }

    src.close();
    can_set_filter(0, 0);

    if (write_ok) {
        led_celebrate();
        Serial.println("BAMWRITE:DONE — write verified OK");
        led_set(0, 255, 0);
    } else {
        led_set(200, 0, 0);  /* solid red */
        Serial.println("BAMWRITE:FAILED — max retries exceeded");
    }
    print_ok();
}

static void cmd_bamread(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }

    Serial.println("BAMREAD: BAM (Boot Assist Module) full read — SPC564A80");

    /* Step 1: Enter BAM mode */
    if (!bam_enter()) {
        print_err("BAM entry failed");
        return;
    }

    /* Step 2: Open SD file. TCM is in BAM boot mode here, so UDS $09 04
     * can't be queried up-front for the OS PN — we write to a timestamped
     * placeholder name and rename with the OS PN after the read completes
     * (the 4 MB dump carries it at flash offset 0x014638). */
    const uint32_t BAM_FLASH_SIZE = 4UL * 1024 * 1024;  /* 4 MB */
    FsFile sd_file;
    bool use_sd = false;
    char fname[96];
    char bamread_ts[20] = {0};
    bool bamread_autoname = false;

    if (g_sd_filename[0]) {
        strncpy(fname, g_sd_filename, sizeof(fname) - 1);
        fname[sizeof(fname) - 1] = '\0';
    } else {
        if (g_sd_ok && !g_sd.exists("/Read") && !g_sd.mkdir("/Read")) {
            Serial.println("WARN: /Read/ create failed (write-protect or full?) — file save will fail");
        }
        format_timestamp(bamread_ts, sizeof(bamread_ts));
        snprintf(fname, sizeof(fname),
                 "/Read/T87A_BAMREAD_%s.bin", bamread_ts);
        bamread_autoname = true;
    }

    if (g_sd_ok) {
        sd_file = g_sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
        if (sd_file) {
            sd_file.preAllocate(BAM_FLASH_SIZE);
            use_sd = true;
            Serial.print("SD: writing to ");
            Serial.println(fname);
        } else {
            Serial.println("WARN: SD file open failed");
        }
    }

    /* Step 3: Read flash via BAM cmd 0x04
     * TX: 04 [SEQ] [COUNT] 00 00 00 00 00
     *   COUNT = 5 normally, 4 for last frame of 64-byte block
     * RX: FF 00 [SEQ] [D0] [D1] [D2] [D3] [D4]
     *   Data bytes = RX[3..3+COUNT-1]
     */
    led_set_blink(0, 100, 200, 250);  /* blue/cyan flash = reading */
    Serial.println("BAMREAD: Starting 4MB flash read...");

    uint8_t  seq = 0x25;  /* capture shows reads start around seq 0x25 */
    uint32_t total_read = 0;
    uint32_t checksum = 0;
    uint8_t  block_buf[64];
    uint32_t block_pos = 0;
    uint32_t errors = 0;
    uint32_t start_ms = millis();

    uint8_t tx[8], rx[8];

    /* Filter to BAM RX ID for speed */
    can_set_filter(BAM_RX_ID, 0x7FF);

    while (total_read < BAM_FLASH_SIZE) {
        /* Determine byte count for this frame:
         * 12 frames of 5 bytes + 1 frame of 4 bytes = 64 bytes per group */
        uint8_t count;
        if (block_pos + 5 >= 64) {
            count = (uint8_t)(64 - block_pos);  /* last frame: 4 bytes */
        } else {
            count = 5;
        }

        memset(tx, 0, 8);
        tx[0] = 0x04;
        tx[1] = seq++;
        tx[2] = count;

        int rlen = bam_send_recv(tx, rx, 500);
        if (rlen < 0) {
            errors++;
            if (errors > 10) {
                led_set(200, 0, 0);
                Serial.println("BAMREAD: Too many timeouts — aborting");
                break;
            }
            continue;
        }
        errors = 0;

        /* Extract data bytes from response */
        for (uint8_t i = 0; i < count && (total_read + i) < BAM_FLASH_SIZE; i++) {
            uint8_t b = rx[3 + i];
            block_buf[block_pos++] = b;
            checksum += b;
        }
        total_read += count;

        /* Flush 64-byte block to SD */
        if (block_pos >= 64) {
            if (use_sd) {
                sd_file.write(block_buf, 64);
            }
            block_pos = 0;
        }

        /* Progress every 64KB */
        if ((total_read & 0xFFFF) == 0) {
            uint32_t elapsed = (millis() - start_ms) / 1000;
            float speed = (elapsed > 0) ? (float)total_read / elapsed / 1024.0f : 0;
            Serial.print("BAMREAD:PROGRESS ");
            Serial.print(total_read / 1024);
            Serial.print("/");
            Serial.print(BAM_FLASH_SIZE / 1024);
            Serial.print(" KB  ");
            Serial.print(speed, 1);
            Serial.print(" KB/s  ");
            Serial.print(elapsed);
            Serial.println("s");

            /* Flush SD periodically */
            if (use_sd) sd_file.flush();

            led_update();
        }
    }

    /* Flush any remaining bytes */
    if (block_pos > 0 && use_sd) {
        sd_file.write(block_buf, block_pos);
    }

    if (use_sd) {
        sd_file.flush();
        Serial.print("SD: file size ");
        Serial.print(sd_file.size());
        Serial.println(" bytes");
        sd_file.close();
    }

    can_set_filter(0, 0);  /* restore accept-all */

    uint32_t elapsed = (millis() - start_ms) / 1000;
    Serial.print("BAMREAD:DONE bytes=");
    Serial.print(total_read);
    Serial.print(" checksum=0x");
    Serial.print(checksum, HEX);
    Serial.print(" time=");
    Serial.print(elapsed);
    Serial.println("s");

    if (total_read >= BAM_FLASH_SIZE) {
        led_celebrate();
        led_set(0, 255, 0);
    } else {
        led_set(200, 0, 0);
    }

    /* Rename placeholder to include OS PN. T87A stores it at flash offset
     * 0x014638 as a 4-byte big-endian uint32 (same offset t87a_patch.py
     * reads from .bin files). Only rename when we wrote the full 4 MB and
     * the PN parses into the GM PN range (20000000..30000000). */
    if (bamread_autoname && use_sd && total_read >= BAM_FLASH_SIZE && g_sd_ok) {
        FsFile rf = g_sd.open(fname, O_RDONLY);
        uint32_t os_pn = 0;
        if (rf) {
            rf.seekSet(0x014638);
            uint8_t pn[4];
            if (rf.read(pn, 4) == 4) {
                os_pn = ((uint32_t)pn[0] << 24) | ((uint32_t)pn[1] << 16) |
                        ((uint32_t)pn[2] << 8)  |  (uint32_t)pn[3];
            }
            rf.close();
        }
        if (os_pn >= 20000000UL && os_pn <= 30000000UL) {
            char newname[96];
            snprintf(newname, sizeof(newname),
                     "/Read/T87A_%lu_BAMREAD_%s.bin",
                     (unsigned long)os_pn, bamread_ts);
            if (g_sd.rename(fname, newname)) {
                Serial.print("BAMREAD: renamed to ");
                Serial.println(newname);
            } else {
                Serial.print("BAMREAD: rename failed, kept ");
                Serial.println(fname);
            }
        } else {
            Serial.print("BAMREAD: OS PN unreadable at 0x014638, kept ");
            Serial.println(fname);
        }
    }

    print_ok();
}

/* ---------- VIN / OSID state (cached for FULLREAD filename) ---------- */
/* non-static so earlier forward decls can reference via extern */
char g_vin[20]  = {0};   /* 17-char VIN + null */
char g_osid[20] = {0};   /* up to 16-char cal ID + null */

static bool read_vin_from_ecu(void)
{
    /* GM ReadDataByLocalIdentifier: 1A 90 (VIN) */
    uint8_t req[2] = { 0x1A, 0x90 };
    uds_msg_t resp;
    int ret = uds_request(&g_isotp_link, req, 2, &resp, UDS_DEFAULT_TIMEOUT_MS);
    if (ret != UDS_OK) {
        /* Fallback: standard UDS ReadByIdentifier DID F190 */
        uint8_t req2[3] = { 0x22, 0xF1, 0x90 };
        ret = uds_request(&g_isotp_link, req2, 3, &resp, UDS_DEFAULT_TIMEOUT_MS);
        if (ret == UDS_OK) {
            /* 22 F190 response: sub_function=F1, data[0]=90, data[1..]=VIN */
            uint16_t vin_off = 1;  /* skip DID low byte */
            uint16_t vin_len = (resp.data_len > vin_off) ? (resp.data_len - vin_off) : 0;
            if (vin_len > 17) vin_len = 17;
            uint16_t j = 0;
            for (uint16_t i = 0; i < vin_len; i++) {
                uint8_t c = resp.data[vin_off + i];
                if (c >= 0x20 && c <= 0x7E) g_vin[j++] = (char)c;
            }
            g_vin[j] = '\0';
            return (j > 0);
        }
        return false;
    }
    /* 1A 90 response: sub_function=90, data[0..]=VIN */
    uint16_t vin_len = resp.data_len;
    if (vin_len > 17) vin_len = 17;
    uint16_t j = 0;
    for (uint16_t i = 0; i < vin_len; i++) {
        uint8_t c = resp.data[i];
        if (c >= 0x20 && c <= 0x7E) g_vin[j++] = (char)c;
    }
    g_vin[j] = '\0';
    return (j > 0);
}

static bool read_osid_from_ecu(void)
{
    /* OBD-II Mode 9 PID 04 (Calibration ID) — returns decimal 8-digit PN in
     * ASCII (e.g. "24293216"), matching our .bin filename convention.
     * Response: 49 04 <num_ids> <id0:16> [id1:16] ... — take only the first. */
    uint8_t req[2] = { 0x09, 0x04 };
    uds_msg_t resp;
    int ret = uds_request(&g_isotp_link, req, 2, &resp, UDS_DEFAULT_TIMEOUT_MS);
    if (ret == UDS_OK && resp.data_len >= 2) {
        /* resp.sub_function=04, data[0]=num_ids, data[1..16]=first cal ID */
        uint16_t off = 1;              /* skip num_ids byte */
        uint16_t end = off + 16;       /* first cal ID is 16 bytes */
        if (end > resp.data_len) end = resp.data_len;
        uint16_t j = 0;
        for (uint16_t i = off; i < end && j < sizeof(g_osid) - 1; i++) {
            uint8_t c = resp.data[i];
            if (c == 0x00) break;      /* null pad marks end of ASCII PN */
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z')) {
                g_osid[j++] = (char)c;
            }
        }
        g_osid[j] = '\0';
        if (j > 0) return true;
    }

    /* Fallback: GM ReadDataByLocalIdentifier 1A B4 (alphanumeric cal ID) */
    uint8_t req2[2] = { 0x1A, 0xB4 };
    ret = uds_request(&g_isotp_link, req2, 2, &resp, UDS_DEFAULT_TIMEOUT_MS);
    if (ret != UDS_OK) return false;

    /* Response: sub_function=B4, data[0..]=cal ID (ASCII) */
    uint16_t len = resp.data_len;
    if (len > 16) len = 16;
    uint16_t j = 0;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t c = resp.data[i];
        if (c >= 0x20 && c <= 0x7E) g_osid[j++] = (char)c;
    }
    g_osid[j] = '\0';
    return (j > 0);
}

static void cmd_vin(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (read_vin_from_ecu()) {
        Serial.print("VIN:"); Serial.println(g_vin);
        print_ok();
    } else {
        print_err("VIN read failed");
    }
}

static void cmd_osid(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (read_osid_from_ecu()) {
        Serial.print("OSID:"); Serial.println(g_osid);
        print_ok();
    } else {
        print_err("OSID read failed");
    }
}

static void cmd_vinwrite(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (!arg || strlen(arg) != 17) {
        print_err("usage: VINWRITE <17-char VIN>");
        return;
    }

    /* Pre-check: GM TCMs silently ignore $3B 90 if the new VIN matches
     * what's already stored (no response → timeout). Read first, skip
     * if identical. */
    if (g_vin[0] && strncmp(g_vin, arg, 17) == 0) {
        Serial.print("VIN already matches: "); Serial.println(arg);
        print_ok();
        return;
    }

    /* E92 short-circuit: bench-verified 2026-04-24 (capture
     * E92-VIN-Write-Testing2.csv) that VIN on E92 is not live-writable via
     * $3B 90 or $2E F190 in any session (default/03/02), with or without
     * AUTH. ECU flow-controls the multi-frame then silently drops. VIN is
     * stored in the calibration block and must be changed via cal reflash
     * with the VIN bytes patched into the bin. See memory:e92_variants.md. */
    if (g_module == MODULE_E92) {
        Serial.println("VINWRITE not supported on E92:");
        Serial.println("  VIN lives in the calibration block, not a live-writable DID.");
        Serial.println("  $3B 90 and $2E F190 are silent-dropped in every session on this OS.");
        Serial.println("  To change VIN: FULLREAD -> patch VIN bytes in the bin -> FULLWRITE.");
        print_err("VINWRITE not supported on E92 (requires cal reflash)");
        return;
    }

    led_vin_write();   /* orange while writing VIN */
    Serial.print("Writing VIN: ");
    Serial.println(arg);

    /* Try GM WriteDataByLocalIdentifier first: $3B 90 [17 bytes VIN].
     * Works on E38/T87/T87A in the default session. */
    uint8_t req_gm[19];
    req_gm[0] = 0x3B;
    req_gm[1] = 0x90;
    memcpy(&req_gm[2], arg, 17);

    uds_msg_t resp;
    int ret = uds_request(&g_isotp_link, req_gm, sizeof(req_gm), &resp,
                          UDS_PENDING_TIMEOUT_MS);
    if (ret == UDS_OK) {
        memcpy(g_vin, arg, 17);
        g_vin[17] = '\0';
        led_vin_ok();
        Serial.println("VIN written successfully ($3B 90)");
        print_ok();
        return;
    }

    /* $3B 90 failed. Log what happened, then try standard UDS
     * WriteDataByIdentifier $2E F190 — newer stacks (E92/MPC5674F) often
     * only accept the DID form and will return an NRC telling us which
     * gate is blocking (session, security, etc.). */
    Serial.print("  $3B 90 failed: ");
    print_uds_error(ret, &resp);

    uint8_t req_uds[20];
    req_uds[0] = 0x2E;
    req_uds[1] = 0xF1;
    req_uds[2] = 0x90;
    memcpy(&req_uds[3], arg, 17);

    Serial.println("  trying $2E F190 (UDS WriteByIdentifier)...");
    ret = uds_request(&g_isotp_link, req_uds, sizeof(req_uds), &resp,
                      UDS_PENDING_TIMEOUT_MS);
    if (ret == UDS_OK) {
        memcpy(g_vin, arg, 17);
        g_vin[17] = '\0';
        led_vin_ok();
        Serial.println("VIN written successfully ($2E F190)");
        print_ok();
        return;
    }

    led_error();
    Serial.print("  $2E F190 failed: ");
    print_uds_error(ret, &resp);
    if (ret == UDS_ERR_NEGATIVE) {
        Serial.print("  hint: NRC 0x"); Serial.print(resp.nrc, HEX);
        Serial.println(" — may need DIAG 03 (extended session) and/or AUTH (algo 513 for E92)");
    }
}

/* ---------- HSREAD: T87A HS-CAN 4 MB read via FLASHY kernel ----------
 *
 * Uploads the FLASHY T87A private kernel (1980 B, load 0x40010000, Flashy
 * project's own clean-room authorship), then streams flash back via the
 * kernel's $A0 command. Target: 4 MB in ~130-150 s, 3-4× faster than BAMREAD.
 *
 * Prerequisite: TCM must be dual-unlocked (cal with all 5 recipe patches +
 * valid CS1/CS2). If locked, SecurityAccess fails with NRC $33 or the kernel
 * silently refuses to start and $A0 never responds.
 *
 * Protocol mirrors the NT-Link HS-CAN capture (captures/T87A-HS-READ-FUL-take2L.csv):
 *   1. $20 broadcast  — returnToNormal (reset prior session)
 *   2. VIN + OSID reads (for output filename)
 *   3. $28 broadcast  — disableNormalCommunication
 *   4. $27 physical   — SecurityAccess (5-byte GM algo 135)
 *   5. $10 02 / $A2 / $A5 01 / $A5 03 — enter programming mode
 *   6. $34 00 00 08 FC — RequestDownload (size-based, 2300 B)
 *   7. $36 80 40 02 80 00 <kernel...> — TransferData w/ downloadAndExecute
 *   8. $A0 <addr:4> <size:3>  — streaming read (delegated to cmd_extkern_read)
 *   9. t87_return_to_normal
 *
 * Output: SD file HSREAD_<VIN>_<OSID>.bin (falls back to HSREAD_T87A.bin).
 */
/* Use the proven 1980-byte T87A EXTKERN at 0x40010000 as Rollin Smoke for now.
 * The 2300-byte USBJTAG kernel extracted from the NT-Link HS-CAN capture is
 * kept in kernel_t87a_rollin_smoke_private.h but not driven yet — we don't
 * have a successful NT-Link capture showing its $A0 command format. The EXTKERN
 * at 0x40010000 is known-good against cmd_extkern_read's $A0 protocol. */
#if 1  /* HSREAD always available when T87A EXTKERN is embedded */
static bool hsread_upload_rollin_smoke(void)
{
    const uint8_t *kd  = T87A_EXTKERN_KERNEL;
    const uint32_t ks  = T87A_EXTKERN_KERNEL_SIZE;
    const uint32_t la  = T87A_EXTKERN_LOAD_ADDR;

    Serial.print("HSREAD: uploading FLASHY T87A kernel (");
    Serial.print(ks);
    Serial.print(" bytes @ 0x");
    Serial.print(la, HEX);
    Serial.println(")");

    /* $34 RequestDownload — GM size-based: 34 00 00 <size_hi> <size_lo> */
    uint8_t rd_req[5] = {
        UDS_SID_REQUEST_DOWNLOAD, 0x00, 0x00,
        (uint8_t)(ks >> 8), (uint8_t)(ks & 0xFF)
    };
    uds_msg_t resp;
    int ret = uds_request(&g_isotp_link, rd_req, sizeof(rd_req), &resp,
                          UDS_PENDING_TIMEOUT_MS);
    if (ret != UDS_OK) {
        Serial.print("HSREAD: $34 RequestDownload failed ret=");
        Serial.println(ret);
        if (ret == UDS_ERR_NEGATIVE) {
            Serial.print("  NRC=0x");
            Serial.println(resp.nrc, HEX);
        }
        return false;
    }
    Serial.println("HSREAD: $34 accepted");

    /* $36 TransferData: 36 80 [addr:4] [kernel] — block_seq 0x80 = download+execute */
    static uint8_t td_buf[6 + T87A_EXTKERN_KERNEL_SIZE];
    td_buf[0] = UDS_SID_TRANSFER_DATA;
    td_buf[1] = 0x80;
    td_buf[2] = (uint8_t)(la >> 24);
    td_buf[3] = (uint8_t)(la >> 16);
    td_buf[4] = (uint8_t)(la >> 8);
    td_buf[5] = (uint8_t)(la);
    memcpy(td_buf + 6, kd, ks);

    int sret = isotp_send(&g_isotp_link, td_buf, (uint16_t)(6 + ks));
    if (sret != ISOTP_RET_OK) {
        Serial.print("HSREAD: isotp_send failed ret=");
        Serial.println(sret);
        return false;
    }

    uint32_t deadline = millis() + 15000;
    while (g_isotp_link.send_status == ISOTP_SEND_STATUS_INPROGRESS &&
           millis() < deadline) {
        poll_can_rx();
        isotp_poll(&g_isotp_link);
    }
    if (g_isotp_link.send_status == ISOTP_SEND_STATUS_INPROGRESS) {
        Serial.println("HSREAD: ISO-TP send timeout");
        return false;
    }
    Serial.println("HSREAD: kernel upload complete");

    /* Give the kernel a moment to start executing at 0x40028000 */
    delay(1000);
    g_extkern_active = true;
    return true;
}
#endif  /* HSREAD available */

static void cmd_hsread(const char *arg)
{
    (void)arg;
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (g_module != MODULE_T87) { print_err("HSREAD requires ALGO T87"); return; }
    if (!g_sd_ok)           { print_err("HSREAD requires SD card"); return; }

#if 0
    print_err("HSREAD: FLASHY T87A kernel not embedded in this build");
    return;
#else
    Serial.println("HSREAD: T87A HS-CAN 4 MB read (Rollin Smoke kernel)");
    Serial.println("HSREAD: prerequisite — TCM must be dual-unlocked (5-patch cal)");

    /* Force T87A HS-CAN slot 2 (0x7E2 tx / 0x7EA rx) + T87 algo.
     * `ALGO T87` only sets g_module/algo, not the CAN IDs — without this the
     * session requests go to the ECM slot (0x7E0) and silent-timeout. */
    g_tester_id = 0x7E2;
    g_ecu_id    = 0x7EA;
    seedkey_set_algo(SEEDKEY_T87);
    isotp_init_link(&g_isotp_link, g_tester_id,
                    g_isotp_send_buf, sizeof(g_isotp_send_buf),
                    g_isotp_recv_buf, sizeof(g_isotp_recv_buf));

    /* 1. returnToNormal — clear any prior diagnostic session */
    Serial.println("HSREAD: returnToNormal (broadcast)...");
    t87_broadcast_reset();

    /* 2. Read VIN + OSID — drives output filename */
    bool have_vin  = read_vin_from_ecu();
    bool have_osid = read_osid_from_ecu();
    if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin); }
    else           { Serial.println("VIN: (not available)"); }
    if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
    else           { Serial.println("OSID: (not available)"); }

    /* 3. $28 disableNormalCommunication */
    Serial.println("HSREAD: disableNormalComm (broadcast)...");
    t87_broadcast_disable_comms();

    /* 4. SecurityAccess $27 */
    Serial.println("HSREAD: SecurityAccess...");
    led_auth();
    if (!do_security_access()) {
        led_error();
        print_err("HSREAD: security access denied — TCM may not be dual-unlocked");
        return;
    }

    /* Post-auth guard: Rollin Smoke is T87A-specific (load 0x40028000).
     * do_security_access() flips g_t87a_detected when it sees a 5-byte seed. */
    if (!g_t87a_detected) {
        led_error();
        print_err("HSREAD: module is T87, not T87A — Rollin Smoke kernel target mismatch");
        return;
    }

    /* 5. Enter programming mode ($10 02 / $A2 / $A5 01 / $A5 03) */
    t87_enter_programming_mode();
    delay(100);

    /* 6-7. Upload Rollin Smoke kernel */
    if (!hsread_upload_rollin_smoke()) {
        led_error();
        print_err("HSREAD: kernel upload failed");
        g_extkern_active = false;
        return;
    }

    /* 8. Build output filename + stream 4 MB via $A0 (2048 × 0x800 blocks).
     * Timestamped so successive reads don't overwrite each other.
     * Output lands in /Read/ on the SD card (auto-created if missing), the
     * mirror of the /Write/ source directory. */
    if (g_sd_ok && !g_sd.exists("/Read") && !g_sd.mkdir("/Read")) {
        Serial.println("WARN: /Read/ create failed (write-protect or full?) — file save will fail");
    }
    char ts[20];
    format_timestamp(ts, sizeof(ts));
    {
        const char *mod = module_name_for_filename();
        if (have_vin && have_osid) {
            snprintf(g_sd_filename, sizeof(g_sd_filename),
                     "/Read/%s_%s_%s_HSREAD_%s.bin", mod, g_vin, g_osid, ts);
        } else if (have_osid) {
            snprintf(g_sd_filename, sizeof(g_sd_filename),
                     "/Read/%s_%s_HSREAD_%s.bin", mod, g_osid, ts);
        } else if (have_vin) {
            snprintf(g_sd_filename, sizeof(g_sd_filename),
                     "/Read/%s_%s_HSREAD_%s.bin", mod, g_vin, ts);
        } else {
            snprintf(g_sd_filename, sizeof(g_sd_filename),
                     "/Read/%s_HSREAD_%s.bin", mod, ts);
        }
    }
    Serial.print("HSREAD: streaming 4 MB to SD file ");
    Serial.println(g_sd_filename);

    cmd_extkern_read("0 2048");
    g_sd_filename[0] = '\0';

    /* 9. Teardown:
     *    - stop the 0x101 broadcast TesterPresent flood (cmd_extkern_read
     *      re-enables g_auto_broadcast on its normal exit; we want the bus
     *      quiet once HSREAD is done)
     *    - $20 returnToNormal + $14 clearDTCs broadcast (harmless to send;
     *      the kernel ignores them but other modules re-sync)
     *    - No $11 01 ECU reset: the USBJTAG kernel at 0x40010000 has taken
     *      over the TCM's CAN handling and doesn't route UDS services to
     *      the OS. A clean reset requires power-cycle (same as NT-Link). */
    /* Stop the 0x0101 TesterPresent flood so the kernel sees a silent tester.
     * Important: do NOT call t87_return_to_normal() yet when AUTOKEXIT is on
     * — earlier bench test showed the $20 + $14 broadcast pair is what
     * pushes the kernel into its silent-zombie state. Let KEXIT try the
     * exit while the kernel may still be responsive. */
    g_auto_broadcast = false;
    g_extkern_active = false;

    Serial.println("HSREAD:DONE");
    bool kernel_exited = false;
    if (g_auto_kexit) {
        kernel_exited = kexit_run_trailer_and_observe();
    }
    if (!kernel_exited) {
        /* Fallback: original returnToNormal sequence. */
        t87_return_to_normal();
        if (g_auto_kexit) {
            Serial.println("HSREAD: kernel still resident — power-cycle the TCM to clear");
        } else {
            Serial.println("HSREAD: kernel still resident on TCM — power-cycle the TCM to clear");
            Serial.println("HSREAD: (try AUTOKEXIT on to attempt inline kernel exit)");
        }
    }
    print_ok();
#endif
}

/* ---------- T87A kernel-exit trailer ----------
 *
 * Mined from commercial-tool HS-CAN write captures. After the bulk transfer,
 * the proprietary tools send a series of failing / stop-session frames;
 * the kernel's state machine recognizes "transfer complete" and hands
 * control back to stock TCM OS. Captures show the 0x101 broadcast flips
 * from FE 01 3E (kernel mode) to FE 01 20 (normal), and stock runtime
 * frames (0x189, 0x197, 0x1A6, 0x1AF, 0x1F5, 0x3F5) reappear.
 *
 * Candidate sequence — try each, because the captures are RX-only and
 * we can't see the exact tester command behind each response:
 *   $34 00 00 08 FC   — RequestDownload (expected NRC 7F 34 78)
 *   $36 FF 00 00 00 00 — TransferData   (expected NRC 7F 36 78)
 *   $37               — TransferExit
 *   $A5 03            — GM stopProgrammingMode
 *   $31 01 FF 00      — RoutineControl CheckProgrammingDependencies
 *   $11 01            — ECUReset hardReset
 *
 * Runs inline — caller must still have g_isotp_link set up to 0x7E2/0x7EA.
 * Returns true if bus observation shows kernel-mode exited. */
/* (g_auto_kexit is declared near the top of this file as a forward decl) */

/* Send one UDS request as part of the exit trailer; log NRC / timeout. */
static void kexit_send(const char *label, const uint8_t *req, uint16_t len)
{
    uds_msg_t resp;
    Serial.print("  TX "); Serial.println(label);
    int ret = uds_request(&g_isotp_link, req, len, &resp, 400);
    if (ret == UDS_ERR_NEGATIVE) {
        Serial.print("    NRC 7F "); Serial.print(req[0], HEX);
        Serial.print(" "); if (resp.nrc < 0x10) Serial.print('0');
        Serial.println(resp.nrc, HEX);
    } else if (ret == UDS_OK) {
        Serial.println("    positive response (unexpected — kernel may be exiting)");
    } else {
        Serial.print("    timeout/err ret="); Serial.println(ret);
    }
}

/* Listen N ms, report whether we saw 0x101 FE 01 20 or stock OS runtime.
 * Returns true if the OS appears to be back on the bus. */
static bool kexit_observe(uint32_t duration_ms, const char *label)
{
    can_set_filter(0, 0);
    { uint32_t _id; uint8_t _d[8]; uint8_t _l;
      while (can_receive(&_id, _d, &_l) == 0) {} }

    const uint32_t STOCK_OS_IDS[] = {
        0x0C1, 0x0C7, 0x0F1, 0x0F9, 0x189, 0x197, 0x19D, 0x1A6,
        0x1AF, 0x1F5, 0x1F8, 0x3F5, 0x4C9
    };
    const int NUM_STOCK_IDS = sizeof(STOCK_OS_IDS) / sizeof(STOCK_OS_IDS[0]);

    bool saw_bcast_3E = false, saw_bcast_20 = false, saw_stock = false;
    uint32_t first_stock = 0, frame_count = 0;

    uint32_t start = millis();
    while ((millis() - start) < duration_ms) {
        uint32_t id; uint8_t data[8]; uint8_t len;
        if (can_receive(&id, data, &len) != 0) continue;
        frame_count++;
        if (id == 0x101 && len >= 3 && data[0] == 0xFE && data[1] == 0x01) {
            if (data[2] == 0x3E) saw_bcast_3E = true;
            if (data[2] == 0x20) saw_bcast_20 = true;
        }
        if (!saw_stock) {
            for (int i = 0; i < NUM_STOCK_IDS; i++) {
                if (id == STOCK_OS_IDS[i]) {
                    saw_stock = true; first_stock = id; break;
                }
            }
        }
    }

    Serial.print("KEXIT["); Serial.print(label); Serial.print("] frames=");
    Serial.print(frame_count);
    Serial.print(" 3E="); Serial.print(saw_bcast_3E ? "Y" : ".");
    Serial.print(" 20="); Serial.print(saw_bcast_20 ? "Y" : ".");
    Serial.print(" stock="); Serial.print(saw_stock ? "Y" : ".");
    if (saw_stock) { Serial.print(" id=0x"); Serial.print(first_stock, HEX); }
    Serial.println();
    return saw_bcast_20 || saw_stock;
}

/* Liveness probe: send a minimal raw $A0 (read 8 bytes from 0x0), wait up
 * to 300 ms for any frame on 0x7EA. Bench TCMs don't always emit OS runtime
 * frames (IGN not strapped), so kernel-alive check is our best exit signal. */
static bool kexit_kernel_alive(void)
{
    can_set_filter(0, 0);
    { uint32_t _id; uint8_t _d[8]; uint8_t _l;
      while (can_receive(&_id, _d, &_l) == 0) {} }

    /* Raw $A0: [opcode][addr:4][size:3] — 8 bytes from 0x00000000 */
    uint8_t tx[8] = { 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08 };
    can_send(0x7E2, tx, 8);

    uint32_t start = millis();
    while ((millis() - start) < 300) {
        uint32_t id; uint8_t data[8]; uint8_t len;
        if (can_receive(&id, data, &len) != 0) continue;
        if (id == 0x7EA) return true;   /* kernel responded on its channel */
    }
    return false;
}

/* Send a raw 8-byte CAN frame (no ISO-TP framing) to 0x7E2, log it. */
static void kexit_send_raw8(const char *label, const uint8_t *frame)
{
    Serial.print("  RAW TX "); Serial.print(label); Serial.print(" = ");
    for (int i = 0; i < 8; i++) {
        if (frame[i] < 0x10) Serial.print('0');
        Serial.print(frame[i], HEX);
        Serial.print(' ');
    }
    Serial.println();
    can_send(0x7E2, (uint8_t *)frame, 8);
}

/* Multi-strategy kernel exit attempt. Returns true if kernel has gone
 * (liveness probe fails). */
static bool kexit_run_trailer_and_observe(void)
{
    g_auto_broadcast = false;
    g_extkern_active = false;
    can_set_filter(0, 0);

    Serial.println("KEXIT: checking kernel liveness via raw $A0...");
    bool kernel_was_alive = kexit_kernel_alive();
    if (kernel_was_alive) {
        Serial.println("KEXIT: kernel confirmed alive (responded to $A0)");
    } else {
        Serial.println("KEXIT: kernel did not respond to $A0 — may be stuck/crashed");
        Serial.println("KEXIT: proceeding with kill stream anyway (it's safe)");
    }

    /* --- Strategy 1: silent wait (3 s). Success == stock OS heartbeat seen.
     * (We cannot use "kernel non-responsive" as success — that's the stuck
     * state, not an exit. True exit signal is the OS returning to the bus.) */
    Serial.println("KEXIT: strategy 1 — silent wait (3 s)...");
    if (kexit_observe(3000, "silent")) {
        Serial.println("KEXIT: *** OS HEARTBEAT during silent wait — kernel exited on its own ***");
        led_celebrate();
        return true;
    }

    /* --- Strategy 2: THE KILL COMMAND ------------------------------------
     * Proven in T87A-CAL-HSREAD-Exit-Test.csv capture: sending a single
     * $A0 with a 16 MB size (4x flash) causes the kernel to stream 0xFF
     * past flash bounds for ~150 s, ends with PowerPC `blr` (4E 80 00 20),
     * and the MCU resets. TCM cold-boots the OS; 0x0C7 heartbeat resumes
     * ~27 s after the stream stops. Total: ~3 minutes.
     *
     * We wait patiently, watching for the first 0x0C7 stock OS frame
     * (unambiguous "OS is alive" signal). Progress ticks every ~10 s so
     * the user knows Flashy isn't frozen. */
    Serial.println("KEXIT: strategy 2 — 16 MB kill stream + OS-heartbeat wait");
    Serial.println("KEXIT: this takes ~3 minutes. Don't interrupt.");
    {
        uint8_t f[8] = { 0xA0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF };
        kexit_send_raw8("$A0 16MB kill", f);
    }

    /* Wait up to 4 minutes for stock OS (0x0C7) to resume. */
    const uint32_t WAIT_MS      = 240000UL;  /* 4 min ceiling */
    const uint32_t TICK_MS      = 10000UL;   /* progress every 10 s */
    uint32_t start         = millis();
    uint32_t last_tick     = start;
    uint32_t stream_frames = 0;
    uint32_t last_7EA_ms   = 0;
    bool     got_os        = false;

    while ((millis() - start) < WAIT_MS) {
        uint32_t id; uint8_t data[8]; uint8_t len;
        if (can_receive(&id, data, &len) == 0) {
            if (id == 0x7EA) { stream_frames++; last_7EA_ms = millis(); }
            if (id == 0x0C7) { got_os = true; break; }
        }
        if ((millis() - last_tick) >= TICK_MS) {
            uint32_t elapsed_s = (millis() - start) / 1000UL;
            uint32_t quiet_s   = last_7EA_ms ? (millis() - last_7EA_ms) / 1000UL : 0;
            Serial.print("KEXIT: waiting for OS... t+");
            Serial.print(elapsed_s);
            Serial.print("s, stream_frames=");
            Serial.print(stream_frames);
            Serial.print(", stream_quiet=");
            Serial.print(quiet_s);
            Serial.println("s");
            last_tick = millis();
        }
    }

    if (got_os) {
        uint32_t total_s = (millis() - start) / 1000UL;
        Serial.print("KEXIT: *** STOCK OS HEARTBEAT DETECTED after ");
        Serial.print(total_s);
        Serial.println("s — TCM is back ***");
        led_celebrate();
        return true;
    }

    Serial.print("KEXIT: timed out waiting for OS (");
    Serial.print(WAIT_MS / 1000UL);
    Serial.print("s, stream_frames=");
    Serial.print(stream_frames);
    Serial.println(")");
    Serial.println("KEXIT: kernel may still be resident — try power-cycle");
    return false;
}

/* OPSCAN: brute-force try every 1-byte opcode 0x00..0xFF on 0x7E2 (raw 8B)
 * and report which ones get ANY response on 0x7EA. Kernel accepts $A0 and
 * maybe $1A/B/C/D — we already tried most. This finds unknown opcodes.
 * Safe: kernel either ignores unknown opcodes silently, or responds.
 * Call after HSREAD when kernel is running. */
static void cmd_opscan(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (g_module != MODULE_T87) {
        print_err("OPSCAN: T87A-only (run after HSREAD while kernel is alive)");
        return;
    }

    Serial.println("OPSCAN: probing all 256 opcodes on 0x7E2 (raw 8B)");
    Serial.println("OPSCAN: reports only opcodes that produce a 0x7EA response");

    g_auto_broadcast = false;
    g_extkern_active = false;
    can_set_filter(0, 0);

    /* Skip opcodes we already know trigger massive streams */
    const uint8_t SKIP_OPCODES[] = { 0xA0 };
    const int NUM_SKIP = sizeof(SKIP_OPCODES) / sizeof(SKIP_OPCODES[0]);

    int responders = 0;
    for (int op = 0x00; op <= 0xFF; op++) {
        bool skip = false;
        for (int i = 0; i < NUM_SKIP; i++) if (op == SKIP_OPCODES[i]) skip = true;
        if (skip) continue;

        /* Drain any stale frames */
        { uint32_t _id; uint8_t _d[8]; uint8_t _l;
          while (can_receive(&_id, _d, &_l) == 0) {} }

        uint8_t tx[8] = { (uint8_t)op, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        can_send(0x7E2, tx, 8);

        /* Watch for response on 0x7EA within 80 ms */
        uint32_t start = millis();
        bool got = false;
        uint8_t resp[8] = {0};
        uint8_t resp_len = 0;
        while ((millis() - start) < 80) {
            uint32_t id; uint8_t data[8]; uint8_t len;
            if (can_receive(&id, data, &len) != 0) continue;
            if (id == 0x7EA) {
                memcpy(resp, data, len);
                resp_len = len;
                got = true;
                break;
            }
        }

        if (got) {
            responders++;
            Serial.print("OPSCAN: $");
            if (op < 0x10) Serial.print('0');
            Serial.print(op, HEX);
            Serial.print(" -> 0x7EA [");
            Serial.print(resp_len);
            Serial.print("] ");
            for (int i = 0; i < resp_len; i++) {
                if (resp[i] < 0x10) Serial.print('0');
                Serial.print(resp[i], HEX);
                Serial.print(' ');
            }
            Serial.println();

            /* If kernel went silent after this opcode, liveness will fail */
            if (!kexit_kernel_alive()) {
                Serial.print("OPSCAN: *** kernel DIED after $");
                if (op < 0x10) Serial.print('0');
                Serial.print(op, HEX);
                Serial.println(" — candidate exit! ***");
                led_celebrate();
                return;
            }
        }
    }

    Serial.print("OPSCAN: done, ");
    Serial.print(responders);
    Serial.println(" opcodes responded");
    print_ok();
}

static void cmd_kexit(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (g_module != MODULE_T87) {
        print_err("KEXIT: T87A-only (set ALGO T87 first)");
        return;
    }
    (void)kexit_run_trailer_and_observe();
    print_ok();
}

static void cmd_autokexit(const char *arg)
{
    if (!arg || !*arg) {
        Serial.print("AUTOKEXIT: "); Serial.println(g_auto_kexit ? "on" : "off");
        print_ok();
        return;
    }
    if (strcasecmp(arg, "on") == 0 || strcmp(arg, "1") == 0) {
        g_auto_kexit = true;
        Serial.println("AUTOKEXIT: on — HSREAD/CALREAD will run exit trailer inline");
    } else if (strcasecmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
        g_auto_kexit = false;
        Serial.println("AUTOKEXIT: off");
    } else {
        print_err("usage: AUTOKEXIT [on|off]");
        return;
    }
    print_ok();
}

static void cmd_fullread(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (no_kernel_for_active_module()) return;

    uint32_t block_count;
    if (g_module == MODULE_E38 || g_module == MODULE_E38N || g_module == MODULE_E67) {
        block_count = E38_BLOCK_COUNT;
    } else {
        block_count = T87_BLOCK_COUNT;
    }

    /* E38N uses same entry sequence as E38, but with external kernel + raw CAN read */
    if (g_module == MODULE_E38N) {
        Serial.println("FULLREAD: E38 external-kernel mode");

        uds_msg_t resp;

        /* Step 1: disableDTCSetting */
        Serial.println("FULLREAD: disableDTCSetting...");
        {
            uint8_t a2_req[2] = { 0xA2, 0x00 };
            uds_request(&g_isotp_link, a2_req, 1, &resp, UDS_DEFAULT_TIMEOUT_MS);
        }

        /* Step 2: Read VIN + OSID */
        bool have_vin  = read_vin_from_ecu();
        bool have_osid = read_osid_from_ecu();
        if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin); }
        else           { Serial.println("VIN: (not available)"); }
        if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
        else           { Serial.println("OSID: (not available)"); }
        Serial.print("FILE:");
        Serial.print(have_vin ? g_vin : "UNKNOWN");
        Serial.print("_");
        Serial.print(have_osid ? g_osid : "NOOSID");
        Serial.println(".bin");

        /* Step 3: SecurityAccess (default session) */
        Serial.println("FULLREAD: SecurityAccess (default session)...");
        led_auth();
        if (!do_security_access()) { led_error(); return; }

        /* Step 4: Enter programming mode (same broadcast as E38) */
        e38_enter_programming_mode();

        /* Step 5: Upload external kernel */
        Serial.println("FULLREAD: uploading external kernel...");
        cmd_kernel(NULL);
        if (!g_auto_broadcast) {
            led_error();
            print_err("kernel upload failed");
            return;
        }

        /* Step 6: Read all blocks via external-kernel raw CAN */
        Serial.println("FULLREAD: reading flash via EXTKERN...");
        /* Set SD filename from VIN/OSID if available */
        if (g_vin[0] && g_osid[0]) {
            snprintf(g_sd_filename, sizeof(g_sd_filename), "%s_%s.bin", g_vin, g_osid);
        } else if (g_vin[0]) {
            snprintf(g_sd_filename, sizeof(g_sd_filename), "%s.bin", g_vin);
        } else {
            g_sd_filename[0] = '\0';
        }
        char read_arg[32];
        snprintf(read_arg, sizeof(read_arg), "0 %lu", (unsigned long)block_count);
        cmd_extkern_read(read_arg);
        g_sd_filename[0] = '\0';  /* reset */

        /* Step 7: Return to normal */
        e38_return_to_normal();
        g_auto_broadcast = false;
        g_extkern_active  = false;

        led_connected();
        Serial.println("FULLREAD:DONE");
        print_ok();
        return;
    }

    Serial.println("FULLREAD: starting automated read sequence");

    uds_msg_t resp;

    if (g_module == MODULE_E67) {
        /*
         * E67 sequence (from flash tool capture):
         *   1. Read VIN + OSID via 0x7E0 (default session)
         *   2. BC $28 (commControl disable)
         *   3. SecurityAccess $27 (default session)
         *   4. BC $A5 01 (requestProgrammingMode)
         *   5. BC $A5 03 (enableProgrammingMode)
         *   6. Kernel upload ($34/$36 00 + $36 80)
         *   7. $35 01 streaming read
         *   8. returnToNormalMode
         * NOTE: No $10 02, no $A2 broadcast — E67 bootloader rejects kernel
         *       execute if those are sent.
         */

        /* Step 1: Read VIN + OSID (default session) */
        bool have_vin  = read_vin_from_ecu();
        bool have_osid = read_osid_from_ecu();

        if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin); }
        else           { Serial.println("VIN: (not available)"); }
        if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
        else           { Serial.println("OSID: (not available)"); }

        Serial.print("FILE:");
        Serial.print(have_vin ? g_vin : "UNKNOWN");
        Serial.print("_");
        Serial.print(have_osid ? g_osid : "NOOSID");
        Serial.println(".bin");

        /* Step 2: BC $28 (commControl disable) */
        Serial.println("E67: commControl disable (broadcast)...");
        { uint8_t bc[1] = { 0x28 }; send_broadcast_cmd(bc, 1); delay(110); }

        /* Step 3: SecurityAccess (default session) */
        Serial.println("FULLREAD: SecurityAccess (default session)...");
        led_auth();
        if (!do_security_access()) { led_error(); return; }

        /* Step 4-5: BC $A5 01 + $A5 03 */
        Serial.println("E67: requestProgrammingMode (broadcast)...");
        { uint8_t bc[2] = { 0xA5, 0x01 }; send_broadcast_cmd(bc, 2); delay(110); }
        Serial.println("E67: enableProgrammingMode (broadcast)...");
        { uint8_t bc[2] = { 0xA5, 0x03 }; send_broadcast_cmd(bc, 2); delay(500); }

        /* Step 6: Upload + execute kernel */
        Serial.println("FULLREAD: uploading kernel...");
        cmd_kernel(NULL);

        /* Check kernel alive via $1A BB */
        delay(1000);
        {
            uint8_t bb_req[2] = { 0x1A, 0xBB };
            int kret = uds_request(&g_isotp_link, bb_req, 2, &resp, 5000);
            if (kret == UDS_OK) {
                Serial.print("E67: kernel alive: ");
                for (uint16_t k = 0; k < resp.data_len; k++) {
                    Serial.print(resp.data[k], HEX); Serial.print(" ");
                }
                Serial.println();
            } else {
                led_error();
                print_err("E67 kernel not running");
                return;
            }
        }

        /* Step 7: flash tool $35 block-by-block read.
         * With ISR ring buffer + NeoPixel disabled during read,
         * ISO-TP multi-frame should be zero-error now. */
        Serial.println("FULLREAD: E67 streaming read ($35 01)...");
        led_reading();
        g_reading_active = true;  /* suppress NeoPixel during read */

        if (g_vin[0] && g_osid[0]) {
            snprintf(g_sd_filename, sizeof(g_sd_filename), "%s_%s.bin", g_vin, g_osid);
        } else if (g_vin[0]) {
            snprintf(g_sd_filename, sizeof(g_sd_filename), "%s.bin", g_vin);
        } else {
            g_sd_filename[0] = '\0';
        }

        bool use_sd = false;
        FsFile sd_file;
        if (g_sd_filename[0] && g_sd_ok) {
            g_sd.remove(g_sd_filename);
            sd_file = g_sd.open(g_sd_filename, O_WRITE | O_CREAT);
            if (sd_file) {
                use_sd = true;
                Serial.print("SD: writing to "); Serial.println(g_sd_filename);
            }
        }

        const uint32_t flash_size = 0x200000;
        const uint32_t e67_block_size = 2048;
        const uint32_t e67_total_blocks = flash_size / e67_block_size;
        uint32_t total_bytes = 0;
        uint32_t start_ms = millis();
        uint32_t last_progress = 0;
        uint32_t errors = 0;
        static uds_msg_t blk_msg;

        for (uint32_t blk = 0; blk < e67_total_blocks && errors < 10; blk++) {
            uint32_t addr = blk * e67_block_size;

            uint8_t ru_req[5] = { 0x35, 0x01,
                (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)(addr) };
            int ru_ret = isotp_send(&g_isotp_link, ru_req, 5);
            if (ru_ret != ISOTP_RET_OK) { errors++; continue; }

            bool got_data = false;
            for (int attempt = 0; attempt < 4 && !got_data; attempt++) {
                int mr = uds_receive(&g_isotp_link, &blk_msg, 10000);
                if (mr != UDS_OK) break;
                if (blk_msg.service == 0x36) {
                    got_data = true;
                    if (blk_msg.data_len >= e67_block_size) {
                        uint16_t hdr_skip = blk_msg.data_len - e67_block_size;
                        if (use_sd) sd_file.write(blk_msg.data + hdr_skip, e67_block_size);
                        total_bytes += e67_block_size;
                    } else if (blk_msg.data_len > 0) {
                        if (use_sd) sd_file.write(blk_msg.data, blk_msg.data_len);
                        uint32_t remaining = e67_block_size - blk_msg.data_len;
                        uint8_t ff_buf[256];
                        memset(ff_buf, 0xFF, sizeof(ff_buf));
                        while (use_sd && remaining > 0) {
                            uint32_t chunk = (remaining > sizeof(ff_buf)) ? sizeof(ff_buf) : remaining;
                            sd_file.write(ff_buf, chunk);
                            remaining -= chunk;
                        }
                        total_bytes += e67_block_size;
                    }
                }
            }

            if (!got_data) {
                errors++;
                /* Flush + reset ISO-TP */
                { uint32_t fe = millis() + 200;
                  while (millis() < fe) { uint32_t ri; uint8_t rd[8]; uint8_t rl;
                    if (can_receive(&ri, rd, &rl) != 0) break; }
                  isotp_init_link(&g_isotp_link, g_tester_id,
                    g_isotp_send_buf, sizeof(g_isotp_send_buf),
                    g_isotp_recv_buf, sizeof(g_isotp_recv_buf)); }
                uint8_t ff_buf[256]; memset(ff_buf, 0xFF, sizeof(ff_buf));
                uint32_t remaining = e67_block_size;
                while (use_sd && remaining > 0) {
                    uint32_t chunk = (remaining > sizeof(ff_buf)) ? sizeof(ff_buf) : remaining;
                    sd_file.write(ff_buf, chunk); remaining -= chunk;
                }
                total_bytes += e67_block_size;
            }

            if (total_bytes / 65536 > last_progress) {
                last_progress = total_bytes / 65536;
                uint32_t elapsed = (millis() - start_ms) / 1000;
                float speed = (elapsed > 0) ? (float)total_bytes / elapsed / 1024.0f : 0;
                Serial.print("FULLREAD:PROGRESS ");
                Serial.print(total_bytes / 1024); Serial.print("/2048 KB  ");
                Serial.print(speed, 1); Serial.print(" KB/s  ");
                Serial.print(elapsed); Serial.println("s");
            }
            send_broadcast_tp_if_due();
        }

        if (use_sd) {
            sd_file.flush();
            Serial.print("SD: file size "); Serial.print((uint32_t)sd_file.size());
            Serial.println(" bytes");
            sd_file.close();
            sd_file = g_sd.open(g_sd_filename, O_RDONLY);
            if (sd_file) {
                uint32_t cksum = 0; uint8_t ck_buf[512]; int n;
                while ((n = sd_file.read(ck_buf, sizeof(ck_buf))) > 0)
                    for (int ci = 0; ci < n; ci++) cksum += ck_buf[ci];
                Serial.print("SD: checksum 0x"); Serial.println(cksum, HEX);
                sd_file.close();
            }
        }

        g_reading_active = false;

        uint32_t elapsed = (millis() - start_ms) / 1000;
        Serial.print("FULLREAD:DONE bytes="); Serial.print(total_bytes);
        Serial.print(" errors="); Serial.print(errors);
        Serial.print(" time="); Serial.print(elapsed);
        Serial.println("s");

        /* Return to normal mode. */
        { uint8_t bc[1] = { 0x20 }; send_broadcast_cmd(bc, 1); }
        delay(2000);
        g_auto_broadcast = false;
        g_extkern_active = false;
        g_sd_filename[0] = '\0';

        led_celebrate();
        print_ok();
        return;

    } else if (g_module == MODULE_E38) {
        /*
         * E38 sequence (from stock tool capture):
         *   1. disableDTCSetting (0xA2) via 0x7E0
         *   2. Read VIN + OSID via 0x7E0 (default session)
         *   3. SecurityAccess via 0x7E0 (default session — BEFORE programming mode)
         *   4. Enter programming mode via 0x0101 broadcast sequence
         *   5. Kernel upload via 0x7E0
         *   6. Read all blocks
         *   7. returnToNormalMode via 0x0101 broadcast
         */

        /* Step 1: disableDTCSetting via 0x7E0 */
        Serial.println("FULLREAD: disableDTCSetting...");
        {
            uint8_t a2_req[2] = { 0xA2, 0x00 };
            uds_request(&g_isotp_link, a2_req, 1, &resp, UDS_DEFAULT_TIMEOUT_MS);
        }

        /* Step 2: Read VIN + OSID (default session) */
        bool have_vin  = read_vin_from_ecu();
        bool have_osid = read_osid_from_ecu();

        if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin); }
        else           { Serial.println("VIN: (not available)"); }
        if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
        else           { Serial.println("OSID: (not available)"); }

        Serial.print("FILE:");
        Serial.print(have_vin ? g_vin : "UNKNOWN");
        Serial.print("_");
        Serial.print(have_osid ? g_osid : "NOOSID");
        Serial.println(".bin");

        /* Step 3: SecurityAccess (still in default session!) */
        Serial.println("FULLREAD: SecurityAccess (default session)...");
        led_auth();
        if (!do_security_access()) { led_error(); return; }

        /* Step 4: Enter programming mode via 0x0101 broadcast */
        e38_enter_programming_mode();

        /* Step 5: Upload + execute kernel */
        Serial.println("FULLREAD: uploading kernel...");
        cmd_kernel(NULL);
        if (!g_auto_broadcast) {
            led_error();
            print_err("kernel upload failed");
            return;
        }

        /* Step 6: Read all blocks */
        Serial.println("FULLREAD: reading flash...");
        if (g_vin[0] && g_osid[0]) {
            snprintf(g_sd_filename, sizeof(g_sd_filename), "%s_%s.bin", g_vin, g_osid);
        } else if (g_vin[0]) {
            snprintf(g_sd_filename, sizeof(g_sd_filename), "%s.bin", g_vin);
        } else {
            g_sd_filename[0] = '\0';
        }
        char read_arg[32];
        snprintf(read_arg, sizeof(read_arg), "0 %lu", (unsigned long)block_count);
        cmd_read(read_arg);
        g_sd_filename[0] = '\0';  /* reset */

        /* Step 7: Return to normal */
        e38_return_to_normal();
        g_auto_broadcast = false;

        Serial.println("FULLREAD:DONE");
        print_ok();
    } else {
        /*
         * T87 / other modules: flash tool-matched sequence
         *   1. Read VIN + OSID
         *   2. $28 broadcast — disableNormalCommunication
         *   3. SecurityAccess ($27) on physical ID
         *   4. $A5 01 broadcast — requestProgrammingMode
         *   5. $A5 03 broadcast — enableProgrammingMode (ECU reboots to bootloader)
         *   6. Kernel upload ($34/$36)
         *   7. Read all blocks ($35)
         */

        /* Step 0: returnToNormalMode — reset any prior diagnostic session */
        Serial.println("FULLREAD: returnToNormal (reset prior session)...");
        {
            uint8_t bc[1] = { 0x20 };
            send_broadcast_cmd(bc, 1);
            delay(200);
        }

        bool have_vin  = read_vin_from_ecu();
        bool have_osid = read_osid_from_ecu();

        if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin); }
        else           { Serial.println("VIN: (not available)"); }
        if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
        else           { Serial.println("OSID: (not available)"); }

        Serial.print("FILE:");
        Serial.print(have_vin ? g_vin : "UNKNOWN");
        Serial.print("_");
        Serial.print(have_osid ? g_osid : "NOOSID");
        Serial.println(".bin");

        /* flash tool-matched sequence: $20 → $28 → $27 → $A5 01 → $A5 03
         * No $10 02 or $A2 — flash tool capture confirms they are not needed.
         * $28 before $27, matching proven flash tool order. */

        /* Step 2: $28 disableNormalCommunication (broadcast) */
        Serial.println("FULLREAD: disableNormalComm (broadcast)...");
        {
            uint8_t bc[1] = { 0x28 };
            send_broadcast_cmd(bc, 1);
            delay(110);
        }

        /* Step 3: SecurityAccess ($27) — after $28 per flash tool order */
        Serial.println("FULLREAD: SecurityAccess...");
        led_auth();
        if (!do_security_access()) { led_error(); return; }

        /* Step 4+5: Enter programming mode ($10 02/$A2 for T87A, then $A5 01/$A5 03) */
        t87_enter_programming_mode();

        /* Step 6: Kernel upload */
        Serial.println("FULLREAD: uploading kernel...");
        cmd_kernel(NULL);
        if (!g_auto_broadcast) {
            print_err("kernel upload failed");
            return;
        }

        /* Step 7: Read all blocks */
        /* Set SD filename from VIN/OSID if available */
        if (g_vin[0] && g_osid[0]) {
            snprintf(g_sd_filename, sizeof(g_sd_filename), "%s_%s.bin", g_vin, g_osid);
        } else if (g_vin[0]) {
            snprintf(g_sd_filename, sizeof(g_sd_filename), "%s.bin", g_vin);
        } else {
            g_sd_filename[0] = '\0';
        }
        char read_arg[32];
        snprintf(read_arg, sizeof(read_arg), "0 %lu", (unsigned long)block_count);

        if (g_t87a_detected) {
            Serial.println("FULLREAD: reading flash via external-kernel raw CAN...");
            cmd_extkern_read(read_arg);
        } else {
            Serial.println("FULLREAD: reading flash via UDS...");
            cmd_read(read_arg);
        }
        g_sd_filename[0] = '\0';  /* reset */

        /* Return to normal mode */
        t87_return_to_normal();
        g_auto_broadcast = false;
        if (g_t87a_detected) g_extkern_active = false;

        led_connected();
        Serial.println("FULLREAD:DONE");
        print_ok();
    }
}

static void cmd_calread(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (no_kernel_for_active_module()) return;

    if (g_module != MODULE_E38 && g_module != MODULE_E67 && !is_t87_family()) {
        print_err("CALREAD only supported for E38/E67/T87/T93");
        return;
    }

    /* T87/T93: use flash tool broadcast mode entry + read kernel, read cal region */
    if (is_t87_family()) {
        Serial.println("CALREAD: T87 calibration-only read");
        Serial.print("  Region: 0x");
        Serial.print(T87_CAL_START, HEX);
        Serial.print(" - 0x");
        Serial.print(T87_CAL_START + T87_CAL_SIZE - 1, HEX);
        Serial.print(" (");
        Serial.print(T87_CAL_SIZE / 1024);
        Serial.println(" KB)");

        /* Step 1: $20 reset any prior session */
        t87_broadcast_reset();

        /* Step 2: Read VIN + OSID (default session) */
        bool have_vin  = read_vin_from_ecu();
        bool have_osid = read_osid_from_ecu();
        if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin); }
        else           { Serial.println("VIN: (not available)"); }
        if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
        else           { Serial.println("OSID: (not available)"); }

        /* Step 3: Build output filename — /Read/<MODULE>_<OSID>_CALREAD_<ts>.bin
         * Module tag ("T87A"/"T87"/"T93") groups bins by ECU in the file list. */
        if (g_sd_ok && !g_sd.exists("/Read") && !g_sd.mkdir("/Read")) {
            Serial.println("WARN: /Read/ create failed (write-protect or full?) — file save will fail");
        }
        char ts[20];
        format_timestamp(ts, sizeof(ts));
        {
            const char *mod = module_name_for_filename();
            if (have_vin && have_osid) {
                snprintf(g_sd_filename, sizeof(g_sd_filename),
                         "/Read/%s_%s_%s_CALREAD_%s.bin", mod, g_vin, g_osid, ts);
            } else if (have_osid) {
                snprintf(g_sd_filename, sizeof(g_sd_filename),
                         "/Read/%s_%s_CALREAD_%s.bin", mod, g_osid, ts);
            } else if (have_vin) {
                snprintf(g_sd_filename, sizeof(g_sd_filename),
                         "/Read/%s_%s_CALREAD_%s.bin", mod, g_vin, ts);
            } else {
                snprintf(g_sd_filename, sizeof(g_sd_filename),
                         "/Read/%s_CALREAD_%s.bin", mod, ts);
            }
        }
        Serial.print("CALREAD: target ");
        Serial.println(g_sd_filename);

        /* Step 4: $28 disableNormalComm (before SecurityAccess) */
        t87_broadcast_disable_comms();

        /* Step 5: SecurityAccess */
        Serial.println("CALREAD: SecurityAccess...");
        led_auth();
        if (!do_security_access()) {
            led_error();
            g_sd_filename[0] = '\0';
            return;
        }

        /* Step 6: $A5 01/$A5 03 enter programming mode (after SecurityAccess) */
        t87_enter_programming_mode();

        /* Step 7: Upload read kernel.
         * T87A needs the Rollin Smoke EXTKERN kernel (same one HSREAD uses),
         * because cmd_extkern_read speaks its $A0/heartbeat protocol.
         * cmd_kernel(NULL) on T87A picks the 60-byte clean-room stub which
         * doesn't implement extkern — causing "no kernel heartbeat" timeout. */
        if (g_t87a_detected) {
            if (!hsread_upload_rollin_smoke()) {
                led_error();
                g_sd_filename[0] = '\0';
                g_extkern_active = false;
                print_err("CALREAD: kernel upload failed");
                return;
            }
        } else {
            Serial.println("CALREAD: uploading read kernel...");
            cmd_kernel(NULL);  /* default T87 flash-tool read kernel */
            if (!g_auto_broadcast) {
                led_error();
                g_sd_filename[0] = '\0';
                print_err("kernel upload failed");
                return;
            }
        }

        /* Step 8: Read cal region (0x080000, 512 blocks) */
        uint32_t cal_blocks = T87_CAL_SIZE / WRITE_BLOCK_SIZE;  /* 512 */
        char read_arg[32];
        snprintf(read_arg, sizeof(read_arg), "%lX %lu",
                 (unsigned long)T87_CAL_START, (unsigned long)cal_blocks);

        if (g_t87a_detected) {
            Serial.println("CALREAD: reading calibration via external-kernel raw CAN...");
            cmd_extkern_read(read_arg);
        } else {
            Serial.println("CALREAD: reading calibration region...");
            cmd_read(read_arg);
        }

        /* Step 9: Return to normal / kernel exit.
         * For T87A with AUTOKEXIT on, try KEXIT FIRST — the $20 + $14 pair
         * that t87_return_to_normal() sends appears to push the kernel into
         * a silent-zombie state where nothing responds. */
        g_auto_broadcast = false;
        if (g_t87a_detected) g_extkern_active = false;
        g_sd_filename[0] = '\0';

        bool kernel_exited = false;
        if (g_t87a_detected && g_auto_kexit) {
            kernel_exited = kexit_run_trailer_and_observe();
        }
        if (!kernel_exited) {
            t87_return_to_normal();
            if (g_t87a_detected && g_auto_kexit) {
                Serial.println("CALREAD: kernel still resident — power-cycle the TCM to clear");
            }
        }
        return;
    }

    uint32_t cal_block_count = E38_CAL_BLOCK_COUNT;
    uint32_t cal_start_addr  = E38_CAL_START;

    Serial.println("CALREAD: starting calibration-only read");
    Serial.print("  Region: 0x");
    Serial.print(cal_start_addr, HEX);
    Serial.print(" - 0x");
    Serial.print(cal_start_addr + (cal_block_count * 0x800UL) - 1, HEX);
    Serial.print(" (");
    Serial.print((cal_block_count * 0x800UL) / 1024);
    Serial.println(" KB)");

    uds_msg_t resp;

    /* Same setup as FULLREAD for E38 */

    /* Step 1: disableDTCSetting */
    Serial.println("CALREAD: disableDTCSetting...");
    {
        uint8_t a2_req[1] = { 0xA2 };
        uds_request(&g_isotp_link, a2_req, 1, &resp, UDS_DEFAULT_TIMEOUT_MS);
    }

    /* Step 2: Read VIN + OSID */
    bool have_vin  = read_vin_from_ecu();
    bool have_osid = read_osid_from_ecu();
    if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin); }
    else           { Serial.println("VIN: (not available)"); }
    if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
    else           { Serial.println("OSID: (not available)"); }

    /* Build output filename — /Read/CALREAD_<vin>_<osid>_<ts>.bin */
    if (g_sd_ok && !g_sd.exists("/Read") && !g_sd.mkdir("/Read")) {
        Serial.println("WARN: /Read/ create failed (write-protect or full?) — file save will fail");
    }
    {
        char ts[20];
        format_timestamp(ts, sizeof(ts));
        const char *mod = module_name_for_filename();
        if (have_vin && have_osid) {
            snprintf(g_sd_filename, sizeof(g_sd_filename),
                     "/Read/%s_%s_%s_CALREAD_%s.bin", mod, g_vin, g_osid, ts);
        } else if (have_osid) {
            snprintf(g_sd_filename, sizeof(g_sd_filename),
                     "/Read/%s_%s_CALREAD_%s.bin", mod, g_osid, ts);
        } else if (have_vin) {
            snprintf(g_sd_filename, sizeof(g_sd_filename),
                     "/Read/%s_%s_CALREAD_%s.bin", mod, g_vin, ts);
        } else {
            snprintf(g_sd_filename, sizeof(g_sd_filename),
                     "/Read/%s_CALREAD_%s.bin", mod, ts);
        }
    }
    Serial.print("CALREAD: target ");
    Serial.println(g_sd_filename);

    /* Step 3: SecurityAccess (default session) */
    Serial.println("CALREAD: SecurityAccess...");
    led_auth();
    if (!do_security_access()) {
        led_error();
        g_sd_filename[0] = '\0';
        return;
    }

    /* Step 4: Enter programming mode */
    e38_enter_programming_mode();

    /* Step 5: Upload + execute kernel */
    Serial.println("CALREAD: uploading kernel...");
    cmd_kernel(NULL);
    if (!g_auto_broadcast) {
        led_error();
        g_sd_filename[0] = '\0';
        print_err("kernel upload failed");
        return;
    }

    /* Step 6: Read cal region only */
    Serial.println("CALREAD: reading calibration region...");
    char read_arg[32];
    snprintf(read_arg, sizeof(read_arg), "%lX %lu",
             (unsigned long)cal_start_addr, (unsigned long)cal_block_count);
    cmd_read(read_arg);

    /* Step 7: Return to normal */
    e38_return_to_normal();
    g_auto_broadcast = false;
    g_sd_filename[0] = '\0';
}

static void cmd_erase(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }

    char buf[64];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *astr = strtok(buf, " ");
    char *sstr = strtok(NULL, " ");
    if (!astr || !sstr) {
        print_err("usage: ERASE <addr> <size>");
        return;
    }

    uint32_t addr = parse_hex(astr);
    uint32_t size = parse_dec_or_hex(sstr);

    /* RoutineControl Start (0xFF00 = common erase routine) */
    uint8_t erase_data[8];
    erase_data[0] = (uint8_t)(addr >> 24);
    erase_data[1] = (uint8_t)(addr >> 16);
    erase_data[2] = (uint8_t)(addr >> 8);
    erase_data[3] = (uint8_t)(addr);
    erase_data[4] = (uint8_t)(size >> 24);
    erase_data[5] = (uint8_t)(size >> 16);
    erase_data[6] = (uint8_t)(size >> 8);
    erase_data[7] = (uint8_t)(size);

    uds_msg_t resp;
    int ret = uds_routine_control(&g_isotp_link, UDS_ROUTINE_START, 0xFF00,
                                  erase_data, 8, &resp);
    if (ret == UDS_OK) {
        Serial.println("Erase complete");
        print_ok();
    } else {
        print_uds_error(ret, &resp);
    }
}

/* ---------- Flash write helpers ---------- */

static uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFF;
}

/*
 * Read a "WDATA:<addr_hex>:<hex_data>\r\n" line from Serial.
 * Decodes hex pairs directly into block_buf (zero-copy).
 * Returns number of data bytes decoded, or -1 on timeout/parse error.
 */
static int hex_line_to_block(uint8_t *block_buf, uint32_t *addr, uint32_t timeout_ms)
{
    uint32_t deadline = millis() + timeout_ms;

    /* Read and match "WDATA:" prefix */
    const char *prefix = "WDATA:";
    uint8_t pi = 0;
    while (pi < 6) {
        if (millis() > deadline) return -1;
        if (!Serial.available()) { send_broadcast_tp_if_due(); continue; }
        char c = (char)Serial.read();
        if (c == prefix[pi]) { pi++; }
        else if (c == '\r' || c == '\n') { pi = 0; } /* skip blank lines */
        else { /* skip non-matching line */
            while (millis() < deadline) {
                if (!Serial.available()) { send_broadcast_tp_if_due(); continue; }
                c = (char)Serial.read();
                if (c == '\n') break;
            }
            pi = 0;
        }
    }

    /* Read address (up to ':') */
    char addr_str[12];
    uint8_t ai = 0;
    while (ai < sizeof(addr_str) - 1) {
        if (millis() > deadline) return -1;
        if (!Serial.available()) { send_broadcast_tp_if_due(); continue; }
        char c = (char)Serial.read();
        if (c == ':') break;
        addr_str[ai++] = c;
    }
    addr_str[ai] = '\0';
    *addr = (uint32_t)strtoul(addr_str, NULL, 16);

    /* Read hex data pairs directly into block_buf */
    int count = 0;
    while ((uint32_t)count < WRITE_BLOCK_SIZE) {
        if (millis() > deadline) return -1;
        /* Read high nibble */
        uint8_t hi, lo;
        while (true) {
            if (millis() > deadline) return -1;
            if (!Serial.available()) { send_broadcast_tp_if_due(); continue; }
            char c = (char)Serial.read();
            if (c == '\r' || c == '\n') goto done;
            hi = hex_nibble(c);
            if (hi == 0xFF) goto done; /* non-hex = end */
            break;
        }
        /* Read low nibble */
        while (true) {
            if (millis() > deadline) return -1;
            if (!Serial.available()) { send_broadcast_tp_if_due(); continue; }
            char c = (char)Serial.read();
            lo = hex_nibble(c);
            if (lo == 0xFF) return -1; /* odd nibble = error */
            break;
        }
        block_buf[count++] = (uint8_t)((hi << 4) | lo);
    }
    /* Consume trailing newline if present */
    {
        uint32_t drain_end = millis() + 50;
        while (millis() < drain_end) {
            if (!Serial.available()) break;
            char c = (char)Serial.read();
            if (c == '\n') break;
        }
    }
done:
    return count;
}

static void print_addr_hex6(uint32_t addr) {
    if (addr < 0x100000) Serial.print('0');
    if (addr < 0x10000)  Serial.print('0');
    if (addr < 0x1000)   Serial.print('0');
    if (addr < 0x100)    Serial.print('0');
    if (addr < 0x10)     Serial.print('0');
    Serial.print(addr, HEX);
}

/* ---------- T87 flash tool write helpers ---------- */

/* T87 broadcast: $20 returnToNormalMode — resets any prior session */
static void t87_broadcast_reset(void)
{
    Serial.println("T87: returnToNormalMode (broadcast)...");
    uint8_t bc[1] = { 0x20 };
    send_broadcast_cmd(bc, 1);
    delay(200);
}

/* T87 broadcast: $28 disableNormalCommunication — must be BEFORE SecurityAccess */
static void t87_broadcast_disable_comms(void)
{
    Serial.println("T87: disableNormalComm (broadcast)...");
    uint8_t bc[1] = { 0x28 };
    send_broadcast_cmd(bc, 1);
    delay(100);
}

/* T87 broadcast: $A5 01/$A5 03 — must be AFTER SecurityAccess.
 * The correct full sequence is:
 *   $20 reset → VIN/OSID → $28 disableComm → $27 SecurityAccess → $A5 01/$A5 03
 */
static void t87_enter_programming_mode(void)
{
    uint8_t bc[2];

    if (g_t87a_detected) {
        /* T87A (EXTKERN sequence): $10 02 → $28 → $A2 → $A5 01 → $A5 03 */
        Serial.println("T87A: programmingSession (broadcast)...");
        bc[0] = 0x10; bc[1] = 0x02;
        send_broadcast_cmd(bc, 2);
        delay(120);

        Serial.println("T87A: disableNormalComm (broadcast)...");
        bc[0] = 0x28; bc[1] = 0x00;
        send_broadcast_cmd(bc, 1);
        delay(110);

        Serial.println("T87A: reportProgrammedState (broadcast)...");
        bc[0] = 0xA2; bc[1] = 0x00;
        send_broadcast_cmd(bc, 1);
        delay(15);
    }

    Serial.println("T87: requestProgrammingMode (broadcast)...");
    bc[0] = 0xA5; bc[1] = 0x01;
    send_broadcast_cmd(bc, 2);
    delay(300);

    Serial.println("T87: enableProgrammingMode (broadcast)...");
    bc[0] = 0xA5; bc[1] = 0x03;
    send_broadcast_cmd(bc, 2);
    delay(300);   /* TCM reboots to bootloader — no response expected */

    Serial.println(g_t87a_detected ? "T87A: programming mode active" : "T87: programming mode active");
}

/* T87 return to normal — broadcast $20 + $14 clearDTCs */
static void t87_return_to_normal(void)
{
    uint8_t bc[1];
    bc[0] = 0x20;
    send_broadcast_cmd(bc, 1);
    delay(100);
    bc[0] = 0x14;
    send_broadcast_cmd(bc, 1);
    Serial.println("T87: returnToNormalMode + clearDTCs sent");
}

/* T87 flash tool erase — send $36 EE, wait for $76 00 EE */
static bool t87_flash_erase(uint32_t timeout_ms)
{
    uint8_t req[2] = { 0x36, 0xEE };
    uds_msg_t resp;
    int ret = uds_request(&g_isotp_link, req, 2, &resp, timeout_ms);
    if (ret != UDS_OK) {
        Serial.print("T87 ERASE: failed ret=");
        Serial.println(ret);
        if (ret == UDS_ERR_NEGATIVE) {
            Serial.print("  NRC=0x");
            Serial.println(resp.nrc, HEX);
        }
        return false;
    }
    /* Response is parsed as: service=$76, sub_function=$00, data[0]=$EE */
    if (resp.service == 0x76) {
        Serial.print("T87 ERASE: complete ($76");
        if (resp.sub_function != 0) {
            Serial.print(' ');
            if (resp.sub_function < 0x10) Serial.print('0');
            Serial.print(resp.sub_function, HEX);
        } else {
            Serial.print(" 00");
        }
        for (uint16_t i = 0; i < resp.data_len && i < 4; i++) {
            Serial.print(' ');
            if (resp.data[i] < 0x10) Serial.print('0');
            Serial.print(resp.data[i], HEX);
        }
        Serial.println(")");
        return true;
    }
    Serial.print("T87 ERASE: unexpected svc=0x");
    Serial.print(resp.service, HEX);
    Serial.print(" sub=0x");
    Serial.print(resp.sub_function, HEX);
    Serial.print(" len=");
    Serial.println(resp.data_len);
    return false;
}

/* T87 flash tool write block — send $36 00 <addr:4> <2048 bytes>, expect $76 00 73 */
static bool t87_flash_write_block(uint32_t addr, const uint8_t *data)
{
    uint8_t payload[6 + WRITE_BLOCK_SIZE];  /* 2054 bytes */
    payload[0] = 0x36;
    payload[1] = 0x00;
    payload[2] = (uint8_t)(addr >> 24);
    payload[3] = (uint8_t)(addr >> 16);
    payload[4] = (uint8_t)(addr >> 8);
    payload[5] = (uint8_t)(addr);
    memcpy(&payload[6], data, WRITE_BLOCK_SIZE);

    uds_msg_t resp;
    int ret = uds_request(&g_isotp_link, payload, (uint16_t)(6 + WRITE_BLOCK_SIZE),
                          &resp, UDS_PENDING_TIMEOUT_MS);
    if (ret != UDS_OK) {
        Serial.print("T87 WRITE: block 0x");
        Serial.print(addr, HEX);
        Serial.print(" failed ret=");
        Serial.println(ret);
        if (ret == UDS_ERR_NEGATIVE) {
            Serial.print("  NRC=0x");
            Serial.println(resp.nrc, HEX);
        }
        return false;
    }
    /* Response parsed as: service=$76, sub_function=$00, data[0]=$73 */
    if (resp.service == 0x76) {
        return true;
    }
    Serial.print("T87 WRITE: unexpected svc=0x");
    Serial.println(resp.service, HEX);
    return false;
}

/* T87 flash tool finalize — send $36 FF, wait for $76 00 86 */
static bool t87_flash_finalize(void)
{
    uint8_t req[2] = { 0x36, 0xFF };
    uds_msg_t resp;
    int ret = uds_request(&g_isotp_link, req, 2, &resp, 10000);
    if (ret != UDS_OK) {
        Serial.print("T87 FINALIZE: failed ret=");
        Serial.println(ret);
        if (ret == UDS_ERR_NEGATIVE) {
            Serial.print("  NRC=0x");
            Serial.println(resp.nrc, HEX);
        }
        return false;
    }
    /* Response parsed as: service=$76, sub_function=$00, data[0]=$86 */
    if (resp.service == 0x76) {
        Serial.println("T87 FINALIZE: OK ($76)");
        return true;
    }
    Serial.print("T87 FINALIZE: unexpected svc=0x");
    Serial.println(resp.service, HEX);
    return false;
}

/* ---------- WRITE command ---------- */

static void cmd_write(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (!arg || !*arg) { print_err("usage: WRITE <start_addr> <num_blocks>"); return; }

    char buf[64];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *astr = strtok(buf, " ");
    char *sstr = strtok(NULL, " ");
    if (!astr || !sstr) {
        print_err("usage: WRITE <start_addr> <num_blocks>");
        return;
    }

    uint32_t start_addr = parse_hex(astr);
    uint32_t num_blocks = parse_dec_or_hex(sstr);
    if (num_blocks == 0 || num_blocks > 0xFFFF) {
        print_err("invalid block count");
        return;
    }

    /* stock tool kernel: send $34 RequestDownload before $36 blocks.
     * Format: $34 $00 $08 [addr:4] (matching $35 read format). */
    int ret;
    uds_msg_t resp;

    led_writing();
    Serial.println("WRITE:WARNING *** DO NOT POWER DOWN UNTIL WRITE IS VERIFIED ***");

    /* RequestDownload */
    Serial.println("WRITE: RequestDownload...");
    {
        uint8_t rd_req[7];
        rd_req[0] = UDS_SID_REQUEST_DOWNLOAD;
        rd_req[1] = 0x00;
        rd_req[2] = 0x08;
        rd_req[3] = (uint8_t)(start_addr >> 24);
        rd_req[4] = (uint8_t)(start_addr >> 16);
        rd_req[5] = (uint8_t)(start_addr >> 8);
        rd_req[6] = (uint8_t)(start_addr);

        ret = uds_request(&g_isotp_link, rd_req, sizeof(rd_req), &resp,
                          WRITE_ERASE_TIMEOUT_MS);
        if (ret == UDS_OK) {
            Serial.println("RequestDownload accepted");
        } else {
            Serial.print("WRITE: $34 failed ret=");
            Serial.print(ret);
            if (ret == UDS_ERR_NEGATIVE) {
                Serial.print(" NRC=0x");
                Serial.print(resp.nrc, HEX);
            }
            Serial.println();
            Serial.println("WRITE: WARN - continuing with $36 anyway...");
        }
    }

    /* Signal PC to start sending WDATA lines */
    Serial.print("WRITE:READY ");
    Serial.print(start_addr, HEX);
    Serial.print(" ");
    Serial.println(num_blocks);

    uint32_t addr = start_addr;
    uint32_t good_blocks = 0;
    uint32_t failed_blocks = 0;
    uint8_t  block_buf[WRITE_BLOCK_SIZE];

    for (uint32_t blk = 0; blk < num_blocks; blk++) {
        /* Receive one WDATA line from PC */
        uint32_t wdata_addr = 0;
        int data_len = hex_line_to_block(block_buf, &wdata_addr, WRITE_WDATA_TIMEOUT_MS);
        if (data_len <= 0) {
            Serial.print("WRITE:TIMEOUT block=");
            Serial.println(blk);
            Serial.println("CRITICAL: WRITE INCOMPLETE - DO NOT POWER DOWN");
            Serial.print("Kernel alive. Resume with: WRITE ");
            print_addr_hex6(addr);
            Serial.print(" ");
            Serial.println(num_blocks - blk);
            return;
        }

        /* Pad short blocks with 0xFF (erased state) */
        while (data_len < (int)WRITE_BLOCK_SIZE) {
            block_buf[data_len++] = 0xFF;
        }

        /* Build TransferData: $36 $00 [addr:4] [data:0x800] */
        /* We prepend the 4-byte address to the data and pass to uds_transfer_data */
        uint8_t td_payload[4 + WRITE_BLOCK_SIZE];
        td_payload[0] = (uint8_t)(addr >> 24);
        td_payload[1] = (uint8_t)(addr >> 16);
        td_payload[2] = (uint8_t)(addr >> 8);
        td_payload[3] = (uint8_t)(addr);
        memcpy(&td_payload[4], block_buf, WRITE_BLOCK_SIZE);

        /* Send with retries — first block may trigger erase (extended timeout) */
        uint32_t td_timeout = (blk == 0) ? WRITE_ERASE_TIMEOUT_MS
                                          : UDS_PENDING_TIMEOUT_MS;
        bool block_ok = false;
        uds_msg_t resp;
        for (int attempt = 0; attempt < WRITE_MAX_RETRIES; attempt++) {
            ret = uds_transfer_data_ex(&g_isotp_link, 0x00, td_payload,
                                        (uint16_t)(4 + WRITE_BLOCK_SIZE), &resp,
                                        td_timeout);
            if (ret == UDS_OK) {
                block_ok = true;
                break;
            }
            Serial.print("WRITE:RETRY block=");
            Serial.print(blk);
            Serial.print(" attempt=");
            Serial.println(attempt + 1);
        }

        if (block_ok) {
            good_blocks++;
            Serial.print("WDATA:ACK:");
            print_addr_hex6(addr);
            Serial.println();
        } else {
            failed_blocks++;
            Serial.print("WDATA:NAK:");
            print_addr_hex6(addr);
            Serial.print(":");
            Serial.println(resp.nrc, HEX);
            Serial.println("CRITICAL: WRITE FAILED - DO NOT POWER DOWN");
            Serial.print("Kernel alive. Resume with: WRITE ");
            print_addr_hex6(addr);
            Serial.print(" ");
            Serial.println(num_blocks - blk);
            return;
        }

        addr += WRITE_BLOCK_SIZE;

        /* Keep bus alive */
        send_broadcast_tp_if_due();

        /* Progress every 64 blocks */
        if ((blk & 0x3F) == 0x3F) {
            Serial.print("WRITE:PROGRESS ");
            Serial.print(blk + 1);
            Serial.print("/");
            Serial.println(num_blocks);
        }
    }

    /* TransferExit */
    {
        uds_msg_t resp;
        uds_transfer_exit(&g_isotp_link, &resp);
    }

    led_success();
    Serial.print("WRITE:DONE blocks=");
    Serial.print(good_blocks);
    Serial.print(" bytes=");
    Serial.println(good_blocks * WRITE_BLOCK_SIZE);
    if (failed_blocks > 0) {
        Serial.print("WRITE:FAILED_BLOCKS=");
        Serial.println(failed_blocks);
    }
    print_ok();
}

/* ---------- CALWRITE command ---------- */

static void cmd_calwrite(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (no_kernel_for_active_module()) return;

    if (g_module != MODULE_E38 && g_module != MODULE_E67 && g_module != MODULE_E38N && !is_t87_family()) {
        print_err("CALWRITE only supported for E38/E67/E38N/T87/T93");
        return;
    }

    /* T87/T93 flash tool cal write — completely separate protocol */
    if (is_t87_family()) {
        /* T87A has its own EXTKERN-native path. Route there. */
        if (g_t87a_detected) {
            cmd_t87a_calwrite(NULL);
            return;
        }
        Serial.println("CALWRITE: T87 flash tool cal write");

        /* Step 1: $20 reset any prior session */
        t87_broadcast_reset();

        /* Step 2: Read VIN + OSID (default session, before mode switch) */
        bool have_vin  = read_vin_from_ecu();
        bool have_osid = read_osid_from_ecu();
        if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin); }
        else           { Serial.println("VIN: (not available)"); }
        if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
        else           { Serial.println("OSID: (not available)"); }

        Serial.print("CALWRITE:INFO module=T87 region=0x");
        Serial.print(T87_WRITE_CAL_START, HEX);
        Serial.print(" size=");
        Serial.print(T87_WRITE_CAL_SIZE);
        Serial.print(" blocks=");
        Serial.println(T87_WRITE_CAL_BLOCKS);

        /* Step 3: $28 disableNormalComm (before SecurityAccess) */
        t87_broadcast_disable_comms();

        /* Step 4: SecurityAccess */
        Serial.println("CALWRITE: SecurityAccess...");
        led_auth();
        if (!do_security_access()) { led_error(); return; }

        /* Step 5: $A5 01/$A5 03 enter programming mode (after SecurityAccess) */
        t87_enter_programming_mode();

        /* Step 4: Upload calflash write kernel */
        Serial.println("CALWRITE: uploading write kernel...");
        cmd_kernel("write");
        if (!g_auto_broadcast) {
            led_error();
            print_err("kernel upload failed");
            return;
        }

        /* *** DANGER ZONE *** */
        Serial.println("CALWRITE:WARNING *** DO NOT POWER DOWN UNTIL COMPLETE ***");

        /* Step 5: Erase cal region ($36 EE — kernel handles internally) */
        led_erasing();
        Serial.println("CALWRITE: erasing cal region (~5s)...");
        if (!t87_flash_erase(10000)) {
            led_error();
            Serial.println("CRITICAL: ERASE FAILED - DO NOT POWER DOWN");
            return;
        }

        /* Step 6: Write blocks — receive WDATA from PC, send $36 00 per block */
        led_writing();
        Serial.print("WRITE:READY ");
        Serial.print(T87_WRITE_CAL_START, HEX);
        Serial.print(" ");
        Serial.println(T87_WRITE_CAL_BLOCKS);

        uint32_t addr = T87_WRITE_CAL_START;
        uint32_t good_blocks = 0;
        uint8_t  block_buf[WRITE_BLOCK_SIZE];

        for (uint32_t blk = 0; blk < T87_WRITE_CAL_BLOCKS; blk++) {
            uint32_t wdata_addr = 0;
            int data_len = hex_line_to_block(block_buf, &wdata_addr, WRITE_WDATA_TIMEOUT_MS);
            if (data_len <= 0) {
                Serial.print("CALWRITE:TIMEOUT block=");
                Serial.println(blk);
                Serial.println("CRITICAL: WRITE INCOMPLETE - DO NOT POWER DOWN");
                return;
            }
            while (data_len < (int)WRITE_BLOCK_SIZE) block_buf[data_len++] = 0xFF;

            bool block_ok = false;
            for (int attempt = 0; attempt < WRITE_MAX_RETRIES; attempt++) {
                if (t87_flash_write_block(addr, block_buf)) {
                    block_ok = true;
                    break;
                }
            }

            if (block_ok) {
                good_blocks++;
                Serial.print("WDATA:ACK:");
                print_addr_hex6(addr);
                Serial.println();
            } else {
                Serial.print("WDATA:NAK:");
                print_addr_hex6(addr);
                Serial.println();
                Serial.println("CRITICAL: WRITE FAILED - DO NOT POWER DOWN");
                return;
            }

            addr += WRITE_BLOCK_SIZE;
            send_broadcast_tp_if_due();

            if ((blk & 0x3F) == 0x3F) {
                Serial.print("WRITE:PROGRESS ");
                Serial.print(blk + 1);
                Serial.print("/");
                Serial.println(T87_WRITE_CAL_BLOCKS);
            }
        }

        /* Step 7: Finalize ($36 FF → $76 00 86) */
        if (!t87_flash_finalize()) {
            led_error();
            Serial.println("CRITICAL: FINALIZE FAILED - DO NOT POWER DOWN");
            return;
        }

        Serial.print("WRITE:DONE blocks=");
        Serial.print(good_blocks);
        Serial.print(" bytes=");
        Serial.println(good_blocks * WRITE_BLOCK_SIZE);

        /* Return to normal mode */
        t87_return_to_normal();
        g_auto_broadcast = false;
        led_success();
        Serial.println("CALWRITE:DONE - safe to power down");
        Serial.println("Verify with a separate FULLREAD if desired.");
        print_ok();
        return;
    }
    /* End T87 path */

    uint32_t cal_start   = 0x1C0000;
    uint32_t cal_size    = 0x040000;  /* 256 KB */
    uint32_t cal_blocks  = cal_size / WRITE_BLOCK_SIZE;  /* 128 blocks */

    Serial.println("CALWRITE: starting automated calibration write");

    uds_msg_t resp;
    int ret;

    /* Step 1: disableDTCSetting */
    Serial.println("CALWRITE: disableDTCSetting...");
    {
        uint8_t a2_req[2] = { 0xA2, 0x00 };
        uds_request(&g_isotp_link, a2_req, 1, &resp, UDS_DEFAULT_TIMEOUT_MS);
    }

    /* Step 2: Read VIN + OSID for user confirmation */
    bool have_vin  = read_vin_from_ecu();
    bool have_osid = read_osid_from_ecu();
    if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin); }
    else           { Serial.println("VIN: (not available)"); }
    if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
    else           { Serial.println("OSID: (not available)"); }

    Serial.print("CALWRITE:INFO module=E38 region=0x");
    Serial.print(cal_start, HEX);
    Serial.print(" size=");
    Serial.print(cal_size);
    Serial.print(" blocks=");
    Serial.println(cal_blocks);

    /* Step 3: SecurityAccess (default session) */
    Serial.println("CALWRITE: SecurityAccess...");
    led_auth();
    if (!do_security_access()) { led_error(); return; }

    /* Step 4: Enter programming mode */
    e38_enter_programming_mode();

    /* Step 5: Kernel upload + execute */
    if (g_module == MODULE_E38N) {
        Serial.println("CALWRITE: uploading external kernel...");
        cmd_kernel(NULL);  /* E38N kernel auto-selected */
    } else {
        Serial.println("CALWRITE: uploading write kernel...");
        cmd_kernel("write");
    }
    if (!g_auto_broadcast) {
        led_error();
        print_err("kernel upload failed");
        return;
    }

    /* E38N EXTKERN: use raw CAN protocol for cal write */
    if (g_module == MODULE_E38N) {
        Serial.println("CALWRITE: external-kernel raw CAN cal write");
        Serial.println("CALWRITE:WARNING *** DO NOT POWER DOWN UNTIL COMPLETE ***");

        /* Erase cal sectors: 4 x 64KB at 0x1C0000-0x1FFFFF */
        led_erasing();
        Serial.println("CALWRITE: erasing cal region (4 sectors)...");
        uint32_t cal_sectors[] = { 0x1C0000, 0x1D0000, 0x1E0000, 0x1F0000 };
        for (int s = 0; s < 4; s++) {
            Serial.print("ERASE:0x");
            Serial.print(cal_sectors[s], HEX);
            if (!extkern_erase_sector(cal_sectors[s], 10000)) {
                Serial.println(" FAIL");
                led_error();
                Serial.println("CRITICAL: ERASE FAILED - DO NOT POWER DOWN");
                return;
            }
            Serial.println(" OK");
            send_broadcast_tp_if_due();
        }

        /* Write cal data: 64 x 4KB blocks (0x1C0000-0x1FFFFF) */
        uint32_t extkern_cal_blocks = cal_size / 0x1000;  /* 64 blocks */
        led_writing();
        Serial.print("WRITE:READY ");
        Serial.print(cal_start, HEX);
        Serial.print(" ");
        Serial.println(extkern_cal_blocks * 2);  /* PC sends 0x800 blocks */

        uint32_t addr_n = cal_start;
        uint32_t good_n = 0;
        uint8_t  nbuf[0x1000];

        for (uint32_t blk = 0; blk < extkern_cal_blocks; blk++) {
            /* Receive two 0x800 WDATA lines per 4KB block */
            uint32_t wa = 0;
            int dl = hex_line_to_block(nbuf, &wa, WRITE_WDATA_TIMEOUT_MS);
            if (dl <= 0) { Serial.println("CALWRITE:TIMEOUT"); return; }
            while (dl < 0x800) nbuf[dl++] = 0xFF;
            dl = hex_line_to_block(nbuf + 0x800, &wa, WRITE_WDATA_TIMEOUT_MS);
            if (dl <= 0) { Serial.println("CALWRITE:TIMEOUT"); return; }
            while (dl < 0x800) nbuf[0x800 + dl++] = 0xFF;

            if (!extkern_write_block(addr_n, nbuf, 0x1000, 10000)) {
                Serial.print("WDATA:NAK:");
                print_addr_hex6(addr_n);
                Serial.println();
                Serial.println("CRITICAL: WRITE FAILED - DO NOT POWER DOWN");
                return;
            }
            good_n++;
            Serial.print("WDATA:ACK:");
            print_addr_hex6(addr_n);
            Serial.println();
            addr_n += 0x1000;
            send_broadcast_tp_if_due();
        }

        led_success();
        Serial.print("WRITE:DONE blocks=");
        Serial.print(good_n);
        Serial.print(" bytes=");
        Serial.println(good_n * 0x1000UL);

        /* Verification readback via EXTKERN $A0 */
        led_reading();
        Serial.println("CALWRITE:VERIFY readback starting...");
        Serial.println("FULLWRITE:VERIFY");
        char ra[32];
        snprintf(ra, sizeof(ra), "%lX %lu",
                 (unsigned long)cal_start, (unsigned long)cal_blocks);
        cmd_extkern_read(ra);

        Serial.println("CALWRITE: waiting for PC verification...");
        {
            uint32_t vd = millis() + 60000;
            while (millis() < vd) {
                if (Serial.available()) {
                    char line[32]; uint8_t li = 0;
                    while (li < sizeof(line)-1 && millis() < vd) {
                        if (!Serial.available()) continue;
                        char c = (char)Serial.read();
                        if (c == '\r' || c == '\n') { if (li > 0) break; else continue; }
                        line[li++] = c;
                    }
                    line[li] = '\0';
                    if (strncmp(line, "VERIFY:OK", 9) == 0) {
                        Serial.println("CALWRITE: PASSED");
                        e38_return_to_normal();
                        g_auto_broadcast = false;
                        led_success();
                        Serial.println("CALWRITE:DONE");
                        print_ok();
                        return;
                    } else if (strncmp(line, "VERIFY:FAIL", 11) == 0) {
                        led_error();
                        Serial.println("CALWRITE: VERIFICATION FAILED");
                        Serial.println("CRITICAL: DO NOT POWER DOWN");
                        return;
                    }
                }
                send_broadcast_tp_if_due();
            }
            Serial.println("CALWRITE: verify timeout");
        }
        return;
    }

    /* Step 6: Probe kernel — find out what services it supports */
    Serial.println("CALWRITE: probing kernel (2s settle delay)...");
    delay(2000);  /* Let kernel fully initialize */

    /* Try TesterPresent first — simplest request */
    {
        uint8_t tp[1] = { 0x3E };
        ret = uds_request(&g_isotp_link, tp, 1, &resp, 3000);
        Serial.print("  $3E TesterPresent: ");
        if (ret == UDS_OK) Serial.println("OK");
        else { Serial.print("ret="); Serial.println(ret); }
    }

    /* Try $35 RequestUpload (we KNOW this works for reads) — confirms kernel alive */
    {
        uint8_t ru[7] = { 0x35, 0x00, 0x08,
                          (uint8_t)(cal_start >> 24), (uint8_t)(cal_start >> 16),
                          (uint8_t)(cal_start >> 8),  (uint8_t)(cal_start) };
        ret = isotp_send(&g_isotp_link, ru, sizeof(ru));
        if (ret == ISOTP_RET_OK) {
            ret = uds_receive(&g_isotp_link, &resp, 5000);
            Serial.print("  $35 RequestUpload: ");
            if (ret == UDS_OK) {
                Serial.print("OK SID=0x");
                Serial.println(resp.service, HEX);
                /* Drain any $36 data block that follows the $75 response */
                if (resp.service == 0x75) {
                    uds_receive(&g_isotp_link, &resp, 5000);
                    Serial.println("  (drained $36 data block)");
                }
            } else {
                Serial.print("ret="); Serial.println(ret);
            }
        }
    }

    /* Try $34 RequestDownload — multiple formats */
    Serial.println("  Testing $34 formats...");

    /* Format A: $34 $00 $08 [addr:4] — mirror of $35 read format */
    {
        uint8_t rd[7] = { 0x34, 0x00, 0x08,
                          (uint8_t)(cal_start >> 24), (uint8_t)(cal_start >> 16),
                          (uint8_t)(cal_start >> 8),  (uint8_t)(cal_start) };
        ret = uds_request(&g_isotp_link, rd, sizeof(rd), &resp, 5000);
        Serial.print("  $34 00 08 [addr]: ");
        if (ret == UDS_OK) { Serial.print("OK data="); for (uint16_t i=0;i<resp.data_len&&i<8;i++){if(resp.data[i]<0x10)Serial.print('0');Serial.print(resp.data[i],HEX);} Serial.println(); }
        else if (ret == UDS_ERR_NEGATIVE) { Serial.print("NRC=0x"); Serial.println(resp.nrc, HEX); }
        else { Serial.print("ret="); Serial.println(ret); }
    }

    /* Format B: $34 $00 [size:3] — GM kernel upload format, size=cal_size */
    {
        uint8_t rd[5] = { 0x34, 0x00,
                          (uint8_t)(cal_size >> 16), (uint8_t)(cal_size >> 8),
                          (uint8_t)(cal_size) };
        ret = uds_request(&g_isotp_link, rd, sizeof(rd), &resp, 5000);
        Serial.print("  $34 00 [calsize]: ");
        if (ret == UDS_OK) { Serial.print("OK data="); for (uint16_t i=0;i<resp.data_len&&i<8;i++){if(resp.data[i]<0x10)Serial.print('0');Serial.print(resp.data[i],HEX);} Serial.println(); }
        else if (ret == UDS_ERR_NEGATIVE) { Serial.print("NRC=0x"); Serial.println(resp.nrc, HEX); }
        else { Serial.print("ret="); Serial.println(ret); }
    }

    /* Format C: $34 $10 $08 $00 — stock tool T87-style (format=$10, size=0x800) */
    {
        uint8_t rd[4] = { 0x34, 0x10, 0x08, 0x00 };
        ret = uds_request(&g_isotp_link, rd, sizeof(rd), &resp, 5000);
        Serial.print("  $34 10 08 00: ");
        if (ret == UDS_OK) { Serial.print("OK data="); for (uint16_t i=0;i<resp.data_len&&i<8;i++){if(resp.data[i]<0x10)Serial.print('0');Serial.print(resp.data[i],HEX);} Serial.println(); }
        else if (ret == UDS_ERR_NEGATIVE) { Serial.print("NRC=0x"); Serial.println(resp.nrc, HEX); }
        else { Serial.print("ret="); Serial.println(ret); }
    }

    /* Format D: $34 $00 $44 [addr:4] [size:4] — standard UDS */
    {
        uint8_t rd[11] = { 0x34, 0x00, 0x44,
                           (uint8_t)(cal_start >> 24), (uint8_t)(cal_start >> 16),
                           (uint8_t)(cal_start >> 8),  (uint8_t)(cal_start),
                           (uint8_t)(cal_size >> 24), (uint8_t)(cal_size >> 16),
                           (uint8_t)(cal_size >> 8),  (uint8_t)(cal_size) };
        ret = uds_request(&g_isotp_link, rd, sizeof(rd), &resp, 5000);
        Serial.print("  $34 00 44 [addr][size]: ");
        if (ret == UDS_OK) { Serial.print("OK data="); for (uint16_t i=0;i<resp.data_len&&i<8;i++){if(resp.data[i]<0x10)Serial.print('0');Serial.print(resp.data[i],HEX);} Serial.println(); }
        else if (ret == UDS_ERR_NEGATIVE) { Serial.print("NRC=0x"); Serial.println(resp.nrc, HEX); }
        else { Serial.print("ret="); Serial.println(ret); }
    }

    /* Try $31 RoutineControl — erase? */
    {
        uint8_t rc[3] = { 0x31, 0xFF, 0x00 };
        ret = uds_request(&g_isotp_link, rc, sizeof(rc), &resp, 5000);
        Serial.print("  $31 FF00 RoutineControl: ");
        if (ret == UDS_OK) { Serial.print("OK"); Serial.println(); }
        else if (ret == UDS_ERR_NEGATIVE) { Serial.print("NRC=0x"); Serial.println(resp.nrc, HEX); }
        else { Serial.print("ret="); Serial.println(ret); }
    }

    /* *** DANGER ZONE — flash will be modified below *** */
    Serial.println("CALWRITE:WARNING *** DO NOT POWER DOWN UNTIL WRITE IS VERIFIED ***");

    /* Use whichever $34 format worked above (if any) — currently probing only.
     * Write phase proceeds with $36 blocks regardless. */

    /* Step 8: Signal PC and receive blocks */
    led_writing();
    Serial.print("WRITE:READY ");
    Serial.print(cal_start, HEX);
    Serial.print(" ");
    Serial.println(cal_blocks);

    uint32_t addr = cal_start;
    uint32_t good_blocks = 0;
    uint8_t  block_buf[WRITE_BLOCK_SIZE];

    for (uint32_t blk = 0; blk < cal_blocks; blk++) {
        uint32_t wdata_addr = 0;
        int data_len = hex_line_to_block(block_buf, &wdata_addr, WRITE_WDATA_TIMEOUT_MS);
        if (data_len <= 0) {
            Serial.print("CALWRITE:TIMEOUT block=");
            Serial.println(blk);
            Serial.println("CRITICAL: WRITE INCOMPLETE - DO NOT POWER DOWN");
            Serial.print("Kernel alive. Resume: WRITE ");
            print_addr_hex6(addr);
            Serial.print(" ");
            Serial.println(cal_blocks - blk);
            return;
        }
        while (data_len < (int)WRITE_BLOCK_SIZE) block_buf[data_len++] = 0xFF;

        uint8_t td_payload[4 + WRITE_BLOCK_SIZE];
        td_payload[0] = (uint8_t)(addr >> 24);
        td_payload[1] = (uint8_t)(addr >> 16);
        td_payload[2] = (uint8_t)(addr >> 8);
        td_payload[3] = (uint8_t)(addr);
        memcpy(&td_payload[4], block_buf, WRITE_BLOCK_SIZE);

        /* First block triggers kernel erase — use extended timeout.
         * Kernel sends NRC 0x78 (responsePending) during erase. */
        uint32_t td_timeout = (blk == 0) ? WRITE_ERASE_TIMEOUT_MS
                                          : UDS_PENDING_TIMEOUT_MS;
        if (blk == 0) { led_erasing(); Serial.println("CALWRITE: first block (erase+write, please wait)..."); }

        bool block_ok = false;
        for (int attempt = 0; attempt < WRITE_MAX_RETRIES; attempt++) {
            ret = uds_transfer_data_ex(&g_isotp_link, 0x00, td_payload,
                                        (uint16_t)(4 + WRITE_BLOCK_SIZE), &resp,
                                        td_timeout);
            if (ret == UDS_OK) { block_ok = true; break; }
        }

        if (block_ok) {
            good_blocks++;
            if (blk == 0) { led_writing(); Serial.println("CALWRITE:ERASED (first block written)"); }
            Serial.print("WDATA:ACK:");
            print_addr_hex6(addr);
            Serial.println();
        } else {
            Serial.print("WDATA:NAK:");
            print_addr_hex6(addr);
            Serial.print(":");
            Serial.println(resp.nrc, HEX);
            Serial.println("CRITICAL: WRITE FAILED - DO NOT POWER DOWN");
            Serial.print("Kernel alive. Resume: WRITE ");
            print_addr_hex6(addr);
            Serial.print(" ");
            Serial.println(cal_blocks - blk);
            return;
        }

        addr += WRITE_BLOCK_SIZE;
        send_broadcast_tp_if_due();

        if ((blk & 0x1F) == 0x1F) {
            Serial.print("WRITE:PROGRESS ");
            Serial.print(blk + 1);
            Serial.print("/");
            Serial.println(cal_blocks);
        }
    }

    /* TransferExit */
    uds_transfer_exit(&g_isotp_link, &resp);

    led_success();  /* green flash — write phase complete */
    Serial.print("WRITE:DONE blocks=");
    Serial.print(good_blocks);
    Serial.print(" bytes=");
    Serial.println(good_blocks * WRITE_BLOCK_SIZE);

    /* Step 9: Verification readback */
    led_reading();  /* cyan flash during verify read */
    Serial.println("CALWRITE:VERIFY readback starting...");
    Serial.println("FULLWRITE:VERIFY");
    char read_arg[32];
    snprintf(read_arg, sizeof(read_arg), "%lX %lu",
             (unsigned long)cal_start, (unsigned long)cal_blocks);
    cmd_read(read_arg);

    /* Wait for PC verdict */
    Serial.println("CALWRITE: waiting for PC verification...");
    {
        uint32_t verify_deadline = millis() + 60000;
        while (millis() < verify_deadline) {
            if (Serial.available()) {
                char line[32];
                uint8_t li = 0;
                while (li < sizeof(line) - 1 && millis() < verify_deadline) {
                    if (!Serial.available()) continue;
                    char c = (char)Serial.read();
                    if (c == '\r' || c == '\n') { if (li > 0) break; else continue; }
                    line[li++] = c;
                }
                line[li] = '\0';
                if (strncmp(line, "VERIFY:OK", 9) == 0) {
                    Serial.println("CALWRITE: verification PASSED - safe to power down");
                    e38_return_to_normal();
                    g_auto_broadcast = false;
                    led_success();
                    Serial.println("CALWRITE:DONE");
                    print_ok();
                    return;
                } else if (strncmp(line, "VERIFY:FAIL", 11) == 0) {
                    led_error();
                    Serial.println("CALWRITE: VERIFICATION FAILED");
                    Serial.println("CRITICAL: DO NOT POWER DOWN");
                    Serial.println("Options: retry CALWRITE, or manual WRITE + READ");
                    return;
                }
            }
            send_broadcast_tp_if_due();
        }
        /* Timeout waiting for PC — still print result */
        Serial.println("CALWRITE: verify timeout (no PC response)");
        Serial.println("Write completed but NOT verified. Check manually with READ.");
        Serial.println("DO NOT POWER DOWN until verified.");
    }
}

/* ---------- FULLWRITE command ---------- */

static void cmd_fullwrite(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (no_kernel_for_active_module()) return;

    /* E38N EXTKERN: full automated sequence with raw CAN protocol */
    if (g_module == MODULE_E38N) {
        Serial.println("FULLWRITE: E38 external-kernel mode");

        uds_msg_t resp;

        /* Step 1: disableDTCSetting */
        Serial.println("FULLWRITE: disableDTCSetting...");
        {
            uint8_t a2_req[2] = { 0xA2, 0x00 };
            uds_request(&g_isotp_link, a2_req, 1, &resp, UDS_DEFAULT_TIMEOUT_MS);
        }

        /* Step 2: Read VIN + OSID */
        bool have_vin  = read_vin_from_ecu();
        bool have_osid = read_osid_from_ecu();
        if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin); }
        if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }

        /* Step 3: SecurityAccess (default session, same as E38) */
        Serial.println("FULLWRITE: SecurityAccess...");
        led_auth();
        if (!do_security_access()) { led_error(); return; }

        /* Step 4: Enter programming mode */
        e38_enter_programming_mode();

        /* Step 5: Upload external kernel */
        Serial.println("FULLWRITE: uploading external kernel...");
        cmd_kernel(NULL);
        if (!g_auto_broadcast) {
            led_error();
            print_err("kernel upload failed");
            return;
        }

        /* Step 6: Execute the EXTKERN write protocol */
        cmd_extkern_fullwrite();

        /* Step 7: Readback verification */
        Serial.println("FULLWRITE:VERIFY readback starting...");
        Serial.println("FULLWRITE:VERIFY");
        cmd_extkern_read("0 1024");  /* 1024 x 0x800 = 2MB */

        /* Wait for PC verdict */
        Serial.println("FULLWRITE: waiting for PC verification...");
        {
            uint32_t verify_deadline = millis() + 120000;
            while (millis() < verify_deadline) {
                if (Serial.available()) {
                    char line[32];
                    uint8_t li = 0;
                    while (li < sizeof(line) - 1 && millis() < verify_deadline) {
                        if (!Serial.available()) continue;
                        char c = (char)Serial.read();
                        if (c == '\r' || c == '\n') { if (li > 0) break; else continue; }
                        line[li++] = c;
                    }
                    line[li] = '\0';
                    if (strncmp(line, "VERIFY:OK", 9) == 0) {
                        Serial.println("FULLWRITE: verification PASSED");
                        e38_return_to_normal();
                        g_auto_broadcast = false;
                        led_success();
                        Serial.println("FULLWRITE:DONE");
                        print_ok();
                        return;
                    } else if (strncmp(line, "VERIFY:FAIL", 11) == 0) {
                        led_error();
                        Serial.println("FULLWRITE: VERIFICATION FAILED");
                        Serial.println("CRITICAL: DO NOT POWER DOWN");
                        return;
                    }
                }
                send_broadcast_tp_if_due();
            }
            Serial.println("FULLWRITE: verify timeout (no PC response)");
        }
        return;
    }

    uint32_t flash_start, flash_size, block_count;
    if (g_module == MODULE_E38 || g_module == MODULE_E67) {
        flash_start = 0x000000;
        flash_size  = 0x200000;  /* 2 MB */
        block_count = E38_BLOCK_COUNT;
    } else {
        flash_start = 0x000000;
        flash_size  = 0x400000;  /* 4 MB */
        block_count = T87_BLOCK_COUNT;
    }

    Serial.println("FULLWRITE: starting automated full flash write");

    uds_msg_t resp;
    int ret;

    if (g_module == MODULE_E38 || g_module == MODULE_E67) {
        /* E38 sequence — mirrors cmd_fullread E38 path through kernel upload */

        /* Step 1: disableDTCSetting */
        Serial.println("FULLWRITE: disableDTCSetting...");
        {
            uint8_t a2_req[2] = { 0xA2, 0x00 };
            uds_request(&g_isotp_link, a2_req, 1, &resp, UDS_DEFAULT_TIMEOUT_MS);
        }

        /* Step 2: Read VIN + OSID */
        bool have_vin  = read_vin_from_ecu();
        bool have_osid = read_osid_from_ecu();
        if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin); }
        else           { Serial.println("VIN: (not available)"); }
        if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
        else           { Serial.println("OSID: (not available)"); }

        Serial.print("FULLWRITE:INFO module=E38 size=");
        Serial.println(flash_size);

        /* Step 3: SecurityAccess (default session) */
        Serial.println("FULLWRITE: SecurityAccess...");
        led_auth();
        if (!do_security_access()) { led_error(); return; }

        /* Step 4: Enter programming mode */
        e38_enter_programming_mode();

        /* Step 5: Kernel upload */
        Serial.println("FULLWRITE: uploading write kernel...");
        cmd_kernel("write");
        if (!g_auto_broadcast) {
            led_error();
            print_err("kernel upload failed");
            return;
        }

    } else if (is_t87_family()) {
        /* T87/T93 flash tool full flash write — completely separate protocol */
        Serial.println("FULLWRITE: T87/T93 flash tool full flash write");

        /* Step 1: $20 reset any prior session */
        t87_broadcast_reset();

        bool have_vin  = read_vin_from_ecu();
        bool have_osid = read_osid_from_ecu();
        if (have_vin)  { Serial.print("VIN:");  Serial.println(g_vin); }
        else           { Serial.println("VIN: (not available)"); }
        if (have_osid) { Serial.print("OSID:"); Serial.println(g_osid); }
        else           { Serial.println("OSID: (not available)"); }

        Serial.print("FULLWRITE:INFO module=T87 region=0x");
        Serial.print(T87_WRITE_FULL_START, HEX);
        Serial.print(" size=");
        Serial.print(T87_WRITE_FULL_SIZE);
        Serial.print(" blocks=");
        Serial.println(T87_WRITE_FULL_BLOCKS);

        /* $28 disableNormalComm (before SecurityAccess) */
        t87_broadcast_disable_comms();

        /* SecurityAccess */
        Serial.println("FULLWRITE: SecurityAccess...");
        led_auth();
        if (!do_security_access()) { led_error(); return; }

        /* $A5 01/$A5 03 enter programming mode (after SecurityAccess) */
        t87_enter_programming_mode();

        /* Upload full flash write kernel */
        Serial.println("FULLWRITE: uploading full write kernel...");
        cmd_kernel("write_full");
        if (!g_auto_broadcast) {
            led_error();
            print_err("kernel upload failed");
            return;
        }

        /* *** DANGER ZONE *** */
        Serial.println("FULLWRITE:WARNING *** DO NOT POWER DOWN UNTIL COMPLETE ***");

        /* Erase ($36 EE — kernel handles full flash erase, ~40s) */
        led_erasing();
        Serial.println("FULLWRITE: erasing full flash (~40s)...");
        if (!t87_flash_erase(WRITE_ERASE_TIMEOUT_MS)) {
            led_error();
            Serial.println("CRITICAL: ERASE FAILED - DO NOT POWER DOWN");
            return;
        }

        /* Write blocks — receive WDATA from PC, send $36 00 per block */
        led_writing();
        Serial.print("WRITE:READY ");
        Serial.print(T87_WRITE_FULL_START, HEX);
        Serial.print(" ");
        Serial.println(T87_WRITE_FULL_BLOCKS);

        uint32_t addr = T87_WRITE_FULL_START;
        uint32_t good_blocks = 0;
        uint8_t  block_buf[WRITE_BLOCK_SIZE];

        for (uint32_t blk = 0; blk < T87_WRITE_FULL_BLOCKS; blk++) {
            uint32_t wdata_addr = 0;
            int data_len = hex_line_to_block(block_buf, &wdata_addr, WRITE_WDATA_TIMEOUT_MS);
            if (data_len <= 0) {
                Serial.print("FULLWRITE:TIMEOUT block=");
                Serial.println(blk);
                Serial.println("CRITICAL: WRITE INCOMPLETE - DO NOT POWER DOWN");
                return;
            }
            while (data_len < (int)WRITE_BLOCK_SIZE) block_buf[data_len++] = 0xFF;

            bool block_ok = false;
            for (int attempt = 0; attempt < WRITE_MAX_RETRIES; attempt++) {
                if (t87_flash_write_block(addr, block_buf)) {
                    block_ok = true;
                    break;
                }
            }

            if (block_ok) {
                good_blocks++;
                Serial.print("WDATA:ACK:");
                print_addr_hex6(addr);
                Serial.println();
            } else {
                Serial.print("WDATA:NAK:");
                print_addr_hex6(addr);
                Serial.println();
                Serial.println("CRITICAL: WRITE FAILED - DO NOT POWER DOWN");
                return;
            }

            addr += WRITE_BLOCK_SIZE;
            send_broadcast_tp_if_due();

            if ((blk & 0x3F) == 0x3F) {
                Serial.print("WRITE:PROGRESS ");
                Serial.print(blk + 1);
                Serial.print("/");
                Serial.println(T87_WRITE_FULL_BLOCKS);
            }
        }

        /* Finalize ($36 FF → $76 00 86) */
        if (!t87_flash_finalize()) {
            led_error();
            Serial.println("CRITICAL: FINALIZE FAILED - DO NOT POWER DOWN");
            return;
        }

        Serial.print("WRITE:DONE blocks=");
        Serial.print(good_blocks);
        Serial.print(" bytes=");
        Serial.println(good_blocks * WRITE_BLOCK_SIZE);

        /* Return to normal mode */
        t87_return_to_normal();
        g_auto_broadcast = false;
        led_success();
        Serial.println("FULLWRITE:DONE - safe to power down");
        Serial.println("Verify with a separate FULLREAD if desired.");
        print_ok();
        return;

    } else {
        print_err("FULLWRITE: unsupported module");
        return;
    }

    /* Verify kernel is running — query diagnostic DIDs (E38 only) */
    Serial.println("FULLWRITE: checking kernel health...");
    {
        uint8_t q1[2] = { 0x1A, 0xC1 };
        ret = uds_request(&g_isotp_link, q1, 2, &resp, 3000);
        if (ret == UDS_OK) {
            Serial.print("Kernel CVN: ");
            for (uint16_t i = 0; i < resp.data_len && i < 8; i++) {
                if (resp.data[i] < 0x10) Serial.print('0');
                Serial.print(resp.data[i], HEX);
            }
            Serial.println();
        } else {
            Serial.print("Kernel $1A C1: ret=");
            Serial.println(ret);
        }
    }

    /* *** DANGER ZONE — common path for both modules *** */
    Serial.println("FULLWRITE:WARNING *** DO NOT POWER DOWN UNTIL WRITE IS VERIFIED ***");

    /* RequestDownload ($34) — stock tool kernel format: $34 $00 $08 [addr:4] */
    Serial.println("FULLWRITE: RequestDownload...");
    {
        uint8_t rd_req[7];
        rd_req[0] = UDS_SID_REQUEST_DOWNLOAD;
        rd_req[1] = 0x00;
        rd_req[2] = 0x08;
        rd_req[3] = (uint8_t)(flash_start >> 24);
        rd_req[4] = (uint8_t)(flash_start >> 16);
        rd_req[5] = (uint8_t)(flash_start >> 8);
        rd_req[6] = (uint8_t)(flash_start);

        ret = uds_request(&g_isotp_link, rd_req, sizeof(rd_req), &resp,
                          WRITE_ERASE_TIMEOUT_MS);
        if (ret == UDS_OK) {
            Serial.print("RequestDownload accepted, data: ");
            for (uint16_t i = 0; i < resp.data_len && i < 8; i++) {
                if (resp.data[i] < 0x10) Serial.print('0');
                Serial.print(resp.data[i], HEX);
            }
            Serial.println();
        } else {
            Serial.print("RequestDownload ret=");
            Serial.print(ret);
            if (ret == UDS_ERR_NEGATIVE) {
                Serial.print(" NRC=0x");
                Serial.print(resp.nrc, HEX);
            }
            Serial.println();
            Serial.println("FULLWRITE: WARN - $34 failed, trying $36 directly...");
        }
    }

    /* Signal PC and receive blocks */
    led_writing();
    Serial.print("WRITE:READY ");
    Serial.print(flash_start, HEX);
    Serial.print(" ");
    Serial.println(block_count);

    uint32_t addr = flash_start;
    uint32_t good_blocks = 0;
    uint8_t  block_buf[WRITE_BLOCK_SIZE];

    for (uint32_t blk = 0; blk < block_count; blk++) {
        uint32_t wdata_addr = 0;
        int data_len = hex_line_to_block(block_buf, &wdata_addr, WRITE_WDATA_TIMEOUT_MS);
        if (data_len <= 0) {
            Serial.print("FULLWRITE:TIMEOUT block=");
            Serial.println(blk);
            Serial.println("CRITICAL: WRITE INCOMPLETE - DO NOT POWER DOWN");
            Serial.print("Kernel alive. Resume: WRITE ");
            print_addr_hex6(addr);
            Serial.print(" ");
            Serial.println(block_count - blk);
            return;
        }
        while (data_len < (int)WRITE_BLOCK_SIZE) block_buf[data_len++] = 0xFF;

        uint8_t td_payload[4 + WRITE_BLOCK_SIZE];
        td_payload[0] = (uint8_t)(addr >> 24);
        td_payload[1] = (uint8_t)(addr >> 16);
        td_payload[2] = (uint8_t)(addr >> 8);
        td_payload[3] = (uint8_t)(addr);
        memcpy(&td_payload[4], block_buf, WRITE_BLOCK_SIZE);

        /* First block triggers kernel erase — use extended timeout */
        uint32_t td_timeout = (blk == 0) ? WRITE_ERASE_TIMEOUT_MS
                                          : UDS_PENDING_TIMEOUT_MS;
        if (blk == 0) { led_erasing(); Serial.println("FULLWRITE: first block (erase+write, please wait)..."); }

        bool block_ok = false;
        for (int attempt = 0; attempt < WRITE_MAX_RETRIES; attempt++) {
            ret = uds_transfer_data_ex(&g_isotp_link, 0x00, td_payload,
                                        (uint16_t)(4 + WRITE_BLOCK_SIZE), &resp,
                                        td_timeout);
            if (ret == UDS_OK) { block_ok = true; break; }
        }

        if (block_ok) {
            good_blocks++;
            if (blk == 0) { led_writing(); Serial.println("FULLWRITE:ERASED (first block written)"); }
            Serial.print("WDATA:ACK:");
            print_addr_hex6(addr);
            Serial.println();
        } else {
            Serial.print("WDATA:NAK:");
            print_addr_hex6(addr);
            Serial.print(":");
            Serial.println(resp.nrc, HEX);
            Serial.println("CRITICAL: WRITE FAILED - DO NOT POWER DOWN");
            Serial.print("Kernel alive. Resume: WRITE ");
            print_addr_hex6(addr);
            Serial.print(" ");
            Serial.println(block_count - blk);
            return;
        }

        addr += WRITE_BLOCK_SIZE;
        send_broadcast_tp_if_due();

        if ((blk & 0x3F) == 0x3F) {
            Serial.print("WRITE:PROGRESS ");
            Serial.print(blk + 1);
            Serial.print("/");
            Serial.println(block_count);
        }
    }

    uds_transfer_exit(&g_isotp_link, &resp);

    led_success();  /* green flash — write phase complete */
    Serial.print("WRITE:DONE blocks=");
    Serial.print(good_blocks);
    Serial.print(" bytes=");
    Serial.println(good_blocks * WRITE_BLOCK_SIZE);

    /* Verification readback */
    led_reading();  /* cyan flash during verify read */
    Serial.println("FULLWRITE:VERIFY readback starting...");
    Serial.println("FULLWRITE:VERIFY");
    char read_arg[32];
    snprintf(read_arg, sizeof(read_arg), "%lX %lu",
             (unsigned long)flash_start, (unsigned long)block_count);
    cmd_read(read_arg);

    /* Wait for PC verdict */
    Serial.println("FULLWRITE: waiting for PC verification...");
    {
        uint32_t verify_deadline = millis() + 120000;
        while (millis() < verify_deadline) {
            if (Serial.available()) {
                char line[32];
                uint8_t li = 0;
                while (li < sizeof(line) - 1 && millis() < verify_deadline) {
                    if (!Serial.available()) continue;
                    char c = (char)Serial.read();
                    if (c == '\r' || c == '\n') { if (li > 0) break; else continue; }
                    line[li++] = c;
                }
                line[li] = '\0';
                if (strncmp(line, "VERIFY:OK", 9) == 0) {
                    Serial.println("FULLWRITE: verification PASSED - safe to power down");
                    if (g_module == MODULE_E38 || g_module == MODULE_E67) {
                        e38_return_to_normal();
                    }
                    g_auto_broadcast = false;
                    led_success();
                    Serial.println("FULLWRITE:DONE");
                    print_ok();
                    return;
                } else if (strncmp(line, "VERIFY:FAIL", 11) == 0) {
                    led_error();
                    Serial.println("FULLWRITE: VERIFICATION FAILED");
                    Serial.println("CRITICAL: DO NOT POWER DOWN");
                    Serial.println("Options: retry FULLWRITE, or manual WRITE + READ");
                    return;
                }
            }
            send_broadcast_tp_if_due();
        }
        Serial.println("FULLWRITE: verify timeout (no PC response)");
        Serial.println("Write completed but NOT verified. Check manually with READ.");
        Serial.println("DO NOT POWER DOWN until verified.");
    }
}

/* ---------- end flash write ---------- */

static void cmd_reset(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    uint8_t type = UDS_RESET_HARD;
    if (arg && *arg) type = (uint8_t)parse_dec_or_hex(arg);

    uds_msg_t resp;
    int ret = uds_ecu_reset(&g_isotp_link, type, &resp);
    if (ret == UDS_OK) {
        Serial.println("ECU reset sent");
        print_ok();
    } else {
        print_uds_error(ret, &resp);
    }
}

static void cmd_raw(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (!arg || !*arg) { print_err("usage: RAW <hex bytes>"); return; }

    uint8_t  req[256];
    uint16_t req_len = 0;
    const char *p = arg;
    while (*p && req_len < sizeof(req)) {
        while (*p == ' ') p++;
        if (!*p) break;
        char hex[3] = { p[0], (char)(p[1] ? p[1] : '0'), '\0' };
        req[req_len++] = (uint8_t)strtoul(hex, NULL, 16);
        p += (p[1] ? 2 : 1);
    }

    uds_msg_t resp;
    int ret = uds_request(&g_isotp_link, req, req_len, &resp, UDS_DEFAULT_TIMEOUT_MS);

    if (ret == UDS_OK) {
        Serial.print("RSP: ");
        Serial.print(resp.service, HEX);
        Serial.print(" ");
        Serial.print(resp.sub_function, HEX);
        Serial.print(" | ");
        for (uint16_t i = 0; i < resp.data_len; i++) {
            if (resp.data[i] < 0x10) Serial.print('0');
            Serial.print(resp.data[i], HEX);
            Serial.print(" ");
        }
        Serial.println();
        print_ok();
    } else {
        print_uds_error(ret, &resp);
    }
}

static void cmd_cansend(const char *arg)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }
    if (!arg || !*arg) { print_err("usage: CANSEND <id> <hex bytes>"); return; }

    char buf[128];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *id_str = strtok(buf, " ");
    if (!id_str) { print_err("missing CAN ID"); return; }
    uint32_t can_id = parse_hex(id_str);

    uint8_t data[8];
    uint8_t len = 0;
    char *p;
    while ((p = strtok(NULL, " ")) != NULL && len < 8) {
        data[len++] = (uint8_t)strtoul(p, NULL, 16);
    }

    if (can_send(can_id, data, len) == 0) {
        Serial.print("CAN TX 0x"); Serial.print(can_id, HEX);
        Serial.print(" ["); Serial.print(len); Serial.println("]");
        print_ok();
    } else {
        print_err("CAN send failed");
    }
}

/* ---------- SCAN: probe CAN bus for modules ---------- */

/* Known GM diagnostic CAN ID pairs */
struct gm_scan_entry {
    uint32_t    tx_id;
    uint32_t    rx_id;
    const char *name;
};

static const gm_scan_entry GM_MODULES[] = {
    { 0x7E0, 0x7E8, "ECM" },     /* Engine Control Module */
    { 0x7E2, 0x7EA, "TCM" },     /* Transmission Control Module */
    { 0x7E1, 0x7E9, "ECM2" },    /* Secondary ECM (some platforms) */
    { 0x7E3, 0x7EB, "ABS" },     /* ABS / EBCM */
    { 0x7E4, 0x7EC, "IPC" },     /* Instrument Panel Cluster */
    { 0x7E5, 0x7ED, "BCM" },     /* Body Control Module */
    { 0x7E6, 0x7EE, "RCDLR" },   /* Rear Compartment Door Latch */
    { 0x7E7, 0x7EF, "HVAC" },    /* HVAC */
};
#define GM_MODULE_COUNT (sizeof(GM_MODULES) / sizeof(GM_MODULES[0]))

static void cmd_scan(void)
{
    if (!g_can_initialized) { print_err("not initialized"); return; }

    /* Save current state */
    uint32_t save_tx  = g_tester_id;
    uint32_t save_rx  = g_ecu_id;

    Serial.println("SCAN:START");
    int found = 0;

    for (unsigned i = 0; i < GM_MODULE_COUNT; i++) {
        /* Switch to this module's CAN IDs */
        g_tester_id = GM_MODULES[i].tx_id;
        g_ecu_id    = GM_MODULES[i].rx_id;
        isotp_init_link(&g_isotp_link, g_tester_id,
                        g_isotp_send_buf, sizeof(g_isotp_send_buf),
                        g_isotp_recv_buf, sizeof(g_isotp_recv_buf));

        /* Drain any stale CAN frames */
        { uint32_t rid; uint8_t rd[8]; uint8_t rl;
          while (can_receive(&rid, rd, &rl) == 0) {} }

        led_scan_probe();   /* dim white while probing */

        Serial.print("SCAN:PROBE 0x");
        Serial.print(GM_MODULES[i].tx_id, HEX);
        Serial.print(" ");
        Serial.println(GM_MODULES[i].name);

        /* Send TesterPresent with 500ms timeout for scan speed */
        uint8_t tp_req[2] = { UDS_SID_TESTER_PRESENT, 0x00 };
        uds_msg_t tp_resp;
        int ret = uds_request(&g_isotp_link, tp_req, 2, &tp_resp, 500);
        /* Any response (positive or negative) means module is present.
         * Only timeout (-1) or send failure (-3) means no module. */
        if (ret == UDS_OK || ret == UDS_ERR_NEGATIVE) {
            led_scan_hit();   /* teal — module found */
            found++;
            Serial.print("SCAN:FOUND 0x");
            Serial.print(GM_MODULES[i].tx_id, HEX);
            Serial.print("/0x");
            Serial.print(GM_MODULES[i].rx_id, HEX);
            Serial.print(" ");
            Serial.println(GM_MODULES[i].name);

            /* Re-init link (clean state after NRC) then try to read VIN */
            isotp_init_link(&g_isotp_link, g_tester_id,
                            g_isotp_send_buf, sizeof(g_isotp_send_buf),
                            g_isotp_recv_buf, sizeof(g_isotp_recv_buf));
            if (read_vin_from_ecu()) {
                Serial.print("SCAN:VIN 0x");
                Serial.print(GM_MODULES[i].tx_id, HEX);
                Serial.print(" ");
                Serial.println(g_vin);
            }
        } else {
            led_off();        /* brief off between misses */
            delay(50);
        }
    }

    /* Restore original state */
    g_tester_id = save_tx;
    g_ecu_id    = save_rx;
    isotp_init_link(&g_isotp_link, g_tester_id,
                    g_isotp_send_buf, sizeof(g_isotp_send_buf),
                    g_isotp_recv_buf, sizeof(g_isotp_recv_buf));

    led_connected();   /* back to green */
    Serial.print("SCAN:DONE ");
    Serial.print(found);
    Serial.println(" module(s) found");
    print_ok();
}

/* Forward declarations for menu system */
static void cmd_status(void);
static void cmd_help(void);
static void cmd_capture(const char *arg);

/* ---------- Menu helper commands ---------- */

/* Read DTCs — UDS $19 02 FF (reportDTCByStatusMask, all DTCs) */
/* Decode a 2-byte OBD-II DTC into human-readable "P0100" format. */
static void print_obd2_dtc(uint8_t hi, uint8_t lo) {
    static const char prefix[] = "PCBU";
    char code[6];
    code[0] = prefix[(hi >> 6) & 0x03];
    code[1] = '0' + ((hi >> 4) & 0x03);
    code[2] = '0' + (hi & 0x0F);
    code[3] = '0' + ((lo >> 4) & 0x0F);
    code[4] = '0' + (lo & 0x0F);
    code[5] = '\0';
    Serial.print(code);
}

/* Read DTCs — try OBD-II $03 first (all ECUs must support it),
 * fall back to UDS $19 02 FF if $03 is rejected. */
static void cmd_read_dtc(void) {
    if (!g_can_initialized) { print_err("CAN not initialized"); return; }
    uds_msg_t resp;

    /* Try OBD-II Mode $03 first */
    uint8_t obd_req[1] = { 0x03 };
    int ret = uds_request(&g_isotp_link, obd_req, 1, &resp, UDS_DEFAULT_TIMEOUT_MS);
    if (ret == UDS_OK && !resp.is_negative) {
        /* Response: service 0x43, sub_function = DTC count byte,
         * data = pairs of [hi lo] DTC bytes.
         * UDS parser: sub_function = raw[1] = first byte after 0x43,
         * data starts at raw[2]. For OBD-II $03, raw[1] = DTC count. */
        uint8_t dtc_count = resp.sub_function;
        Serial.print("  DTCs: "); Serial.println(dtc_count);
        for (uint16_t i = 0; i + 1 < resp.data_len; i += 2) {
            Serial.print("    ");
            print_obd2_dtc(resp.data[i], resp.data[i + 1]);
            Serial.println();
        }
        if (dtc_count == 0) Serial.println("  (none)");
        print_ok();
        return;
    }

    /* Fall back to UDS $19 02 FF */
    uint8_t uds_req[3] = { 0x19, 0x02, 0xFF };
    ret = uds_request(&g_isotp_link, uds_req, 3, &resp, UDS_DEFAULT_TIMEOUT_MS);
    if (ret == UDS_OK && !resp.is_negative) {
        Serial.print("  DTC response (");
        Serial.print(resp.data_len); Serial.print(" bytes): ");
        for (uint16_t i = 0; i < resp.data_len; i++) {
            if (resp.data[i] < 0x10) Serial.print('0');
            Serial.print(resp.data[i], HEX); Serial.print(' ');
        }
        Serial.println();
        if (resp.data_len > 1) {
            Serial.print("  DTCs found: "); Serial.println((resp.data_len - 1) / 4);
        }
        print_ok();
        return;
    }

    print_err("DTC read failed (both OBD-II $03 and UDS $19 rejected)");
    print_uds_error(ret, &resp);
}

/* Clear DTCs — try OBD-II $04 first, fall back to UDS $14 FF FF FF. */
static void cmd_clear_dtc(void) {
    if (!g_can_initialized) { print_err("CAN not initialized"); return; }
    uds_msg_t resp;

    /* OBD-II Mode $04 */
    uint8_t obd_req[1] = { 0x04 };
    int ret = uds_request(&g_isotp_link, obd_req, 1, &resp, UDS_DEFAULT_TIMEOUT_MS);
    if (ret == UDS_OK && !resp.is_negative) {
        Serial.println("  DTCs cleared (OBD-II).");
        print_ok();
        return;
    }

    /* Fall back to UDS $14 */
    uint8_t uds_req[4] = { 0x14, 0xFF, 0xFF, 0xFF };
    ret = uds_request(&g_isotp_link, uds_req, 4, &resp, UDS_DEFAULT_TIMEOUT_MS);
    if (ret == UDS_OK && !resp.is_negative) {
        Serial.println("  DTCs cleared (UDS).");
        print_ok();
        return;
    }

    print_err("DTC clear failed (both OBD-II $04 and UDS $14 rejected)");
    print_uds_error(ret, &resp);
}

/* Bus Sniffer — live CAN frame display, stops on any keypress */
static void cmd_bus_sniffer(void) {
    if (!g_can_initialized) { print_err("CAN not initialized"); return; }

    /* Accept-all filter */
    can_set_filter(0, 0);
    can_reset_overflow();
    { uint32_t _id; uint8_t _d[8]; uint8_t _l;
      while (can_receive(&_id, _d, &_l) == 0) { /* drain */ } }

    Serial.println("BUS SNIFFER active — press any key to stop");
    Serial.println("ID       DLC  DATA");
    Serial.println("-------- ---  -----------------------");

    uint32_t frame_count = 0;
    while (true) {
        if (Serial.available()) {
            while (Serial.available()) Serial.read();
            break;
        }
        uint32_t id; uint8_t data[8]; uint8_t len;
        if (can_receive(&id, data, &len) != 0) continue;

        /* Print: 0x7E8     8    01 02 03 04 05 06 07 08 */
        Serial.print("0x");
        if (id < 0x100) Serial.print('0');
        if (id < 0x10)  Serial.print('0');
        Serial.print(id, HEX);
        Serial.print("     ");
        Serial.print(len);
        Serial.print("    ");
        for (int i = 0; i < len; i++) {
            if (data[i] < 0x10) Serial.print('0');
            Serial.print(data[i], HEX);
            if (i < len - 1) Serial.print(' ');
        }
        Serial.println();
        frame_count++;
    }
    Serial.print("Sniffer stopped. Frames: "); Serial.println(frame_count);
}

/* Kernel Popper — capture CAN traffic to SD CSV, for kernel extraction */
static void cmd_kernel_popper(void) {
    if (!g_can_initialized) { print_err("CAN not initialized"); return; }

    /* Prompt for ECU type */
    Serial.print("  Enter ECU type (e.g. E92): ");
    char type_buf[16];
    int tpos = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < 30000) {
        if (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') { if (tpos > 0) break; }
            else if (c == 0x08 || c == 0x7F) { if (tpos > 0) tpos--; }
            else if (tpos < (int)sizeof(type_buf) - 1) type_buf[tpos++] = c;
        }
    }
    type_buf[tpos] = '\0';
    if (tpos == 0) { Serial.println("\n  Cancelled."); return; }
    /* Uppercase */
    for (int i = 0; type_buf[i]; i++) {
        if (type_buf[i] >= 'a' && type_buf[i] <= 'z') type_buf[i] -= 32;
    }
    Serial.println(type_buf);

    /* Prompt for duration */
    Serial.print("  Duration seconds [30]: ");
    char dur_buf[16];
    int dpos = 0;
    t0 = millis();
    while (millis() - t0 < 15000) {
        if (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') break;
            else if (c == 0x08 || c == 0x7F) { if (dpos > 0) dpos--; }
            else if (dpos < (int)sizeof(dur_buf) - 1) dur_buf[dpos++] = c;
        }
    }
    dur_buf[dpos] = '\0';
    uint32_t duration_s = (dpos > 0) ? (uint32_t)atol(dur_buf) : 30;
    if (duration_s == 0) duration_s = 30;
    if (duration_s > 600) duration_s = 600;
    Serial.println(duration_s);

    /* Build filename: <TYPE>-YYMMDD-HHMMSS-Kernel_Dump.csv */
    char fname[64];
    char ts[20];
    format_timestamp(ts, sizeof(ts));
    /* ts is like "260416_123045" — reformat to YYMMDD-HHMMSS */
    snprintf(fname, sizeof(fname), "/%s-%c%c%c%c%c%c-%c%c%c%c%c%c-Kernel_Dump.csv",
             type_buf,
             ts[0], ts[1], ts[2], ts[3], ts[4], ts[5],   /* YYMMDD */
             ts[7], ts[8], ts[9], ts[10], ts[11], ts[12]); /* HHMMSS */

    Serial.print("  Capture to: "); Serial.println(fname);
    Serial.print("  Duration: "); Serial.print(duration_s); Serial.println("s");

    /* Use existing CAPTURE infrastructure via cmd_capture */
    char dur_str[16];
    snprintf(dur_str, sizeof(dur_str), "%lu", (unsigned long)(duration_s * 1000));
    cmd_capture(dur_str);
}

/* ================================================================
 *  INTERACTIVE MENU SYSTEM v2
 * ================================================================
 *  MENU command shows a hierarchical, number-driven interface:
 *    MAIN  →  Connect / Diag / ECU / Hacker / RAW
 *  Typing "B" at any submenu goes back one level.
 *  Typing "X" exits the menu entirely.
 *  All existing flat commands still work when NOT in menu mode.
 * ================================================================ */

/* --- menu levels --- */
typedef enum {
    MENU_OFF = 0,       /* normal flat-command mode */
    MENU_MAIN,          /* top-level main menu */
    MENU_CONNECT,       /* CAN connect submenu */
    MENU_DIAG,          /* diagnostics submenu */
    MENU_ECU,           /* ECU select submenu */
    MENU_ECU_OPS,       /* ECU operations (read/write/VIN) */
    MENU_BINSELECT,     /* SD card bin file picker */
    MENU_WRITETYPE,     /* cal / full / BAM write choice */
    MENU_HACKER,        /* hacker tools submenu */
} menu_level_t;

static menu_level_t g_menu_level = MENU_OFF;
static gm_module_t  g_menu_module = MODULE_E38;   /* module chosen in menu */

/* SD bin selection state */
static char   g_sd_bins[16][48];   /* up to 16 filenames cached */
static int    g_sd_bin_count = 0;
static int    g_sd_bin_selected = -1;

/* --- per-module capability flags --- */
#define CAP_FULLREAD     (1 << 0)
#define CAP_CALREAD      (1 << 1)
#define CAP_BAMREAD      (1 << 2)
#define CAP_READ         (1 << 3)
#define CAP_CALWRITE     (1 << 4)
#define CAP_FULLWRITE    (1 << 5)
#define CAP_BAMWRITE     (1 << 6)
#define CAP_TESTWRITE    (1 << 7)
#define CAP_KERNEL       (1 << 8)
#define CAP_ERASE        (1 << 9)
#define CAP_E92_FULLREAD (1 << 10)   /* one-shot Kernel-based E92 read */
#define CAP_HSREAD       (1 << 11)   /* T87A: HS-CAN fully-unlocked 4 MB read via Rollin Smoke kernel */
#define CAP_HSWRITE      (1 << 12)   /* T87A: HS-CAN write (requires kernel already running) */
#define CAP_T42_READ     (1 << 13)   /* T42: Flashy clean-room MC68377 kernel, MVP auth+upload+ping */

static uint16_t module_caps(gm_module_t m) {
    switch (m) {
        case MODULE_E38:
            return CAP_FULLREAD | CAP_CALREAD | CAP_READ |
                   CAP_CALWRITE | CAP_FULLWRITE | CAP_KERNEL | CAP_ERASE;
        case MODULE_E67:
            return CAP_FULLREAD | CAP_CALREAD | CAP_READ |
                   CAP_CALWRITE | CAP_FULLWRITE | CAP_KERNEL | CAP_ERASE;
        case MODULE_E38N:
            return CAP_FULLREAD | CAP_READ |
                   CAP_CALWRITE | CAP_FULLWRITE | CAP_TESTWRITE | CAP_KERNEL | CAP_ERASE;
        case MODULE_T87:
            return CAP_FULLREAD | CAP_CALREAD | CAP_READ |
                   CAP_CALWRITE | CAP_FULLWRITE | CAP_KERNEL | CAP_ERASE;
        case MODULE_E92:
            return CAP_E92_FULLREAD;
        case MODULE_T93:
            return CAP_BAMREAD | CAP_BAMWRITE | CAP_FULLREAD | CAP_CALREAD | CAP_READ |
                   CAP_CALWRITE | CAP_FULLWRITE | CAP_KERNEL | CAP_ERASE;
        case MODULE_T42:
            return CAP_T42_READ;
        case MODULE_E40:
        default:
            return 0;  /* diag-only */
    }
}

/* Special: T87A is T87 + g_t87a_detected — override caps */
static uint16_t menu_active_caps(void) {
    if (g_menu_module == MODULE_T87 && g_t87a_detected) {
        /* CAL Read works via cmd_calread's T87A branch (cmd_extkern_read on
         * the Rollin Smoke kernel). CAL Write is intentionally omitted —
         * cmd_calwrite uses the flash-tool protocol which the EXTKERN kernel
         * doesn't speak, and HSWRITE is the correct path for T87A writes. */
        return CAP_BAMREAD | CAP_BAMWRITE | CAP_HSREAD | CAP_HSWRITE |
               CAP_CALREAD | CAP_KERNEL | CAP_READ;
    }
    if (g_menu_module == MODULE_T93) {
        return CAP_BAMREAD | CAP_BAMWRITE | CAP_KERNEL | CAP_READ;
    }
    return module_caps(g_menu_module);
}

/* --- module names / CAN IDs --- */
struct module_info_t {
    const char    *name;
    const char    *desc;
    gm_module_t    mod;
    uint32_t       tx_id;
    uint32_t       rx_id;
    uint16_t       algo;
};

/* ECU table — ordered for ECU submenu display.
 * T87A shares MODULE_T87 enum but has its own row (g_t87a_detected flag). */
static const module_info_t g_module_table[] = {
    { "E38",  "ECM",               MODULE_E38,  0x7E0, 0x7E8, 402 },
    { "E67",  "ECM",               MODULE_E67,  0x7E0, 0x7E8, 393 },
    { "E92",  "ECM",               MODULE_E92,  0x7E0, 0x7E8, 513 },
    { "E40",  "ECM",               MODULE_E40,  0x7E0, 0x7E8,   0 },
    { "T87",  "TCM",               MODULE_T87,  0x7E2, 0x7EA, 569 },
    { "T87A", "TCM (BAM)",         MODULE_T87,  0x7E2, 0x7EA, 569 },
    { "T93",  "TCM (BAM)",         MODULE_T93,  0x7E2, 0x7EA,  93 },
    { "T42",  "TCM",               MODULE_T42,  0x7E2, 0x7EA, 371 },
};
#define MODULE_TABLE_COUNT  (sizeof(g_module_table) / sizeof(g_module_table[0]))
/* ECU submenu shows all entries */
#define ECU_MENU_COUNT  8

/* --- SD bin folder per module --- */
static const char* module_bin_folder(gm_module_t m) {
    switch (m) {
        case MODULE_E38:  return "/bins/E38/";
        case MODULE_E67:  return "/bins/E67/";
        case MODULE_E38N: return "/bins/E38N/";
        case MODULE_T87:  return g_t87a_detected ? "/bins/T87A/" : "/bins/T87/";
        case MODULE_T93:  return "/bins/T93/";
        case MODULE_T42:  return "/bins/T42/";
        case MODULE_E92:  return "/bins/E92/";
        case MODULE_E40:  return "/bins/E40/";
        default:          return "/bins/";
    }
}

/* Short uppercase module tag used as the prefix on bin filenames.
 * Examples: "T87A", "E38", "E92". Used so /Read/ output files sort
 * by module in the file explorer. */
static const char* module_name_for_filename(void) {
    switch (g_module) {
        case MODULE_E38:  return "E38";
        case MODULE_E67:  return "E67";
        case MODULE_E38N: return "E38N";
        case MODULE_T87:  return g_t87a_detected ? "T87A" : "T87";
        case MODULE_T93:  return "T93";
        case MODULE_T42:  return "T42";
        case MODULE_E92:  return "E92";
        case MODULE_E40:  return "E40";
        default:          return "ECU";
    }
}

/* --- display helpers --- */

static void menu_show_main(void) {
    char clk[40];
    format_clock_status(clk, sizeof(clk));
    Serial.println();
    Serial.println("=============================");
    Serial.println("  Flashy v" PASSTHRU_VERSION);
    Serial.print("  "); Serial.println(clk);
    if (g_can_initialized) {
        Serial.print("  CAN: "); Serial.print(g_can_baud_rate); Serial.println(" bps");
    } else {
        Serial.println("  CAN: not connected");
    }
    Serial.println("=============================");
    Serial.println("  1) Help");
    Serial.println("  2) Connect >>");
    Serial.println("  3) Diag >>");
    Serial.println("  4) ECU >>");
    Serial.println("  5) Hacker >>");
    Serial.println("  6) RAW");
    Serial.println("=============================");
}

static void menu_show_connect(void) {
    Serial.println();
    Serial.println("  -- CONNECT --");
    Serial.println("  1) Auto Detect");
    Serial.println("  2) 500000 bps");
    Serial.println("  3) 250000 bps");
    Serial.println("  4) 1000000 bps");
    Serial.println("  5) 125000 bps");
    Serial.println("  B) Back");
}

static void menu_show_diag(void) {
    Serial.println();
    Serial.println("  -- DIAG --");
    Serial.println("  1) Ping");
    Serial.println("  2) Scan");
    Serial.println("  3) Read DTC");
    Serial.println("  4) Clear DTC");
    Serial.println("  5) SD Card");
    Serial.println("  6) Set Clock");
    Serial.println("  7) Status");
    Serial.println("  B) Back");
}

static void menu_show_ecu(void) {
    Serial.println();
    Serial.println("  SELECT ECU:");
    Serial.println("  1) E38 ECM");
    Serial.println("  2) E67 ECM");
    Serial.println("  3) E92 ECM");
    Serial.println("  4) E40 ECM");
    Serial.println("  5) T87 TCM");
    Serial.println("  6) T87A TCM");
    Serial.println("  7) T93 TCM (BAM)");
    Serial.println("  8) T42 TCM");
    Serial.println("  B) Back");
}

/* Build & show ECU Ops menu — capability-gated per model */
static void menu_show_ecu_ops(void) {
    Serial.println();
    /* Header: module name + algo */
    const char *mname = "???";
    uint16_t algo = 0;
    for (unsigned i = 0; i < MODULE_TABLE_COUNT; i++) {
        if (g_module_table[i].mod == g_menu_module) {
            if (g_menu_module == MODULE_T87 && g_t87a_detected) {
                if (strcmp(g_module_table[i].name, "T87A") == 0) {
                    mname = "T87A TCM"; algo = g_module_table[i].algo; break;
                }
                continue;
            }
            if (g_menu_module == MODULE_T87 && !g_t87a_detected) {
                if (strcmp(g_module_table[i].name, "T87") == 0) {
                    mname = "T87 TCM"; algo = g_module_table[i].algo; break;
                }
                continue;
            }
            static char hdr[32];
            snprintf(hdr, sizeof(hdr), "%s %s", g_module_table[i].name, g_module_table[i].desc);
            mname = hdr;
            algo = g_module_table[i].algo;
            break;
        }
    }
    Serial.print("  "); Serial.print(mname);
    Serial.print(" (algo "); Serial.print(algo); Serial.println(")");
    Serial.println("  ----------------------------");

    uint16_t caps = menu_active_caps();

    /* T87A custom layout — group by speed path (High Speed vs Bench Tool)
     * and use the naming the user prefers. Fixed numbering 1..9.
     * Dispatch in menu_handle_ecu_ops_choice mirrors this exact order. */
    if (g_menu_module == MODULE_T87 && g_t87a_detected) {
        Serial.println("  1) Cal Read   [High Speed]");
        Serial.println("  2) Full Read  [High Speed]");
        Serial.println("  3) CAL Write  [High Speed]");
        Serial.println("  4) Full Write [High Speed]");
        Serial.println("  5) BAM Read   [Bench Tool]");
        Serial.println("  6) BAM Write  [Bench Tool]");
        Serial.println("  7) VIN");
        Serial.println("  8) VIN Write");
        Serial.println("  9) Reset");
        Serial.println("  B) Back");
        return;
    }

    int n = 1;
    /* Read options */
    if (caps & CAP_FULLREAD)     { Serial.print("  "); Serial.print(n++); Serial.println(") Full Read"); }
    if (caps & CAP_E92_FULLREAD) { Serial.print("  "); Serial.print(n++); Serial.println(") Full Read"); }
    if (caps & CAP_CALREAD)      { Serial.print("  "); Serial.print(n++); Serial.println(") Cal Read"); }
    if (caps & CAP_BAMREAD)      { Serial.print("  "); Serial.print(n++); Serial.println(") BAM Read"); }
    if (caps & CAP_HSREAD)       { Serial.print("  "); Serial.print(n++); Serial.println(") HS Read (Rollin Smoke kernel)"); }
    if (caps & CAP_T42_READ)     { Serial.print("  "); Serial.print(n++); Serial.println(") T42 Read (A: baseline patches 1+2)"); }
    if (caps & CAP_T42_READ)     { Serial.print("  "); Serial.print(n++); Serial.println(") T42 Read B (+ full prog-mode $10 02 $A2)"); }
    if (caps & CAP_T42_READ)     { Serial.print("  "); Serial.print(n++); Serial.println(") T42 Read C (+ sustained 100 ms $3E heartbeat)"); }
    if (caps & CAP_T42_READ)     { Serial.print("  "); Serial.print(n++); Serial.println(") T42 Read D (+ probe fan-out BB/21/55/$35/$3E)"); }
    if (caps & CAP_T42_READ)     { Serial.print("  "); Serial.print(n++); Serial.println(") T42 Read E (+ 2000 ms boot delay)"); }
    /* Write options */
    if (caps & CAP_FULLWRITE)    { Serial.print("  "); Serial.print(n++); Serial.println(") Full Write"); }
    if (caps & CAP_CALWRITE)     { Serial.print("  "); Serial.print(n++); Serial.println(") Cal Write"); }
    if (caps & CAP_BAMWRITE)     { Serial.print("  "); Serial.print(n++); Serial.println(") BAM Write"); }
    if (caps & CAP_HSWRITE)      { Serial.print("  "); Serial.print(n++); Serial.println(") HS Write"); }
    /* Always-available options */
    Serial.print("  "); Serial.print(n++); Serial.println(") VIN");
    Serial.print("  "); Serial.print(n++); Serial.println(") VIN Write");
    Serial.print("  "); Serial.print(n);   Serial.println(") Reset");
    Serial.println("  B) Back");
}

static int menu_load_sd_bins(void) {
    g_sd_bin_count = 0;
    g_sd_bin_selected = -1;
    if (!g_sd_ok) return 0;

    const char *folder = module_bin_folder(g_menu_module);
    FsFile dir = g_sd.open(folder);
    if (!dir || !dir.isDir()) {
        /* Folder doesn't exist yet — show helpful message */
        Serial.print("  (no folder: "); Serial.print(folder); Serial.println(")");
        if (dir) dir.close();
        return 0;
    }

    FsFile entry;
    while (entry.openNext(&dir, O_RDONLY) && g_sd_bin_count < 16) {
        if (!entry.isDir()) {
            char name[48];
            entry.getName(name, sizeof(name));
            /* Only show .bin files */
            int slen = strlen(name);
            if (slen > 4 && strcasecmp(&name[slen - 4], ".bin") == 0) {
                strncpy(g_sd_bins[g_sd_bin_count], name, sizeof(g_sd_bins[0]) - 1);
                g_sd_bins[g_sd_bin_count][sizeof(g_sd_bins[0]) - 1] = '\0';
                g_sd_bin_count++;
            }
        }
        entry.close();
    }
    dir.close();
    return g_sd_bin_count;
}

static void menu_show_binselect(void) {
    Serial.println();
    Serial.println("  -- SELECT BIN --");
    const char *folder = module_bin_folder(g_menu_module);
    Serial.print("  SD Card: "); Serial.println(folder);
    Serial.println("  -------------------------");

    menu_load_sd_bins();

    if (g_sd_bin_count == 0) {
        Serial.println("    (no .bin files found)");
    } else {
        for (int i = 0; i < g_sd_bin_count; i++) {
            Serial.print("    "); Serial.print(i + 1); Serial.print(") ");
            Serial.println(g_sd_bins[i]);
        }
    }
    Serial.println();
    Serial.println("    B) Back");
}

static void menu_show_writetype(void) {
    Serial.println();
    Serial.print("  File: "); Serial.println(g_sd_bins[g_sd_bin_selected]);
    Serial.println("  -- WRITE TYPE --");
    uint16_t caps = menu_active_caps();
    int n = 1;
    if (caps & CAP_CALWRITE)  { Serial.print("    "); Serial.print(n++); Serial.println(") Cal Only"); }
    if (caps & CAP_FULLWRITE) { Serial.print("    "); Serial.print(n++); Serial.println(") Full Flash"); }
    if (caps & CAP_BAMWRITE)  { Serial.print("    "); Serial.print(n++); Serial.println(") BAM Write"); }
    Serial.println();
    Serial.println("    B) Back");
}

static void menu_show_hacker(void) {
    Serial.println();
    Serial.println("  -- HACKER --");
    Serial.println("  1) Kernel Popper");
    Serial.println("  2) Bus Sniffer");
    Serial.println("  B) Back");
}

/* --- apply module selection (auto ALGO + SETID) --- */
static void menu_select_module(int idx) {
    if (idx < 0 || idx >= (int)MODULE_TABLE_COUNT) return;
    const module_info_t *m = &g_module_table[idx];

    g_menu_module = m->mod;
    g_module      = m->mod;
    g_tester_id   = m->tx_id;
    g_ecu_id      = m->rx_id;

    /* Set seed-key algo */
    switch (m->mod) {
        case MODULE_T87:  seedkey_set_algo(SEEDKEY_T87); break;
        case MODULE_E38:  seedkey_set_algo(SEEDKEY_E38); break;
        case MODULE_E67:  seedkey_set_algo(SEEDKEY_E67); break;
        case MODULE_E38N: seedkey_set_algo(SEEDKEY_E38); break;
        case MODULE_T42:  seedkey_set_algo(SEEDKEY_T42); break;
        case MODULE_E92:  seedkey_set_algo(SEEDKEY_E92); break;
        case MODULE_T93:  seedkey_set_algo(SEEDKEY_T87); break;  /* T93 reuses T87 algo for HS-CAN until confirmed */
        case MODULE_E40:  /* no algo yet */ break;
        case MODULE_NONE: /* unreachable — menu picker hands us a real module */ break;
    }

    /* T87A entry — set the detected flag based on table name */
    if (strcmp(m->name, "T87A") == 0) g_t87a_detected = true;
    else if (m->mod == MODULE_T87) g_t87a_detected = false;

    /* Re-init ISO-TP link with new IDs */
    isotp_init_link(&g_isotp_link, g_tester_id,
                    g_isotp_send_buf, sizeof(g_isotp_send_buf),
                    g_isotp_recv_buf, sizeof(g_isotp_recv_buf));

    /* Auto-init CAN if not already initialized */
    if (!g_can_initialized) {
        Serial.println(">> Auto-initializing CAN at 500 kbps...");
        cmd_init(NULL);
    }

    Serial.print(">> Module: "); Serial.print(m->name);
    Serial.print(" "); Serial.print(m->desc);
    Serial.print("  Algo="); Serial.print(m->algo);
    Serial.print("  CAN=0x"); Serial.print(m->tx_id, HEX);
    Serial.print("/0x"); Serial.println(m->rx_id, HEX);
}

/* --- menu input handler --- returns true if input was consumed by menu --- */
static bool menu_handle_input(const char *input) {
    if (g_menu_level == MENU_OFF) return false;

    /* Universal: X exits menu */
    if (strcasecmp(input, "X") == 0) {
        g_menu_level = MENU_OFF;
        Serial.println("Menu exited.");
        return true;
    }
    /* Universal: B goes back one level */
    if (strcasecmp(input, "B") == 0) {
        switch (g_menu_level) {
            case MENU_MAIN:      g_menu_level = MENU_OFF; Serial.println("Menu exited."); return true;
            case MENU_CONNECT:   g_menu_level = MENU_MAIN; menu_show_main(); return true;
            case MENU_DIAG:      g_menu_level = MENU_MAIN; menu_show_main(); return true;
            case MENU_ECU:       g_menu_level = MENU_MAIN; menu_show_main(); return true;
            case MENU_ECU_OPS:   g_menu_level = MENU_ECU;  menu_show_ecu();  return true;
            case MENU_BINSELECT: g_menu_level = MENU_ECU_OPS; menu_show_ecu_ops(); return true;
            case MENU_WRITETYPE: g_menu_level = MENU_BINSELECT; menu_show_binselect(); return true;
            case MENU_HACKER:    g_menu_level = MENU_MAIN; menu_show_main(); return true;
            default:             g_menu_level = MENU_OFF; return true;
        }
    }

    int choice = atoi(input);

    switch (g_menu_level) {

    /* ---- MAIN MENU ---- */
    case MENU_MAIN:
        switch (choice) {
            case 1: cmd_help(); menu_show_main(); break;
            case 2: g_menu_level = MENU_CONNECT; menu_show_connect(); break;
            case 3: g_menu_level = MENU_DIAG;    menu_show_diag();    break;
            case 4: g_menu_level = MENU_ECU;     menu_show_ecu();     break;
            case 5: g_menu_level = MENU_HACKER;  menu_show_hacker();  break;
            case 6:
                g_menu_level = MENU_OFF;
                Serial.println("RAW command mode. Type MENU to return.");
                break;
            default:
                Serial.println("  Invalid choice.");
                menu_show_main();
                break;
        }
        return true;

    /* ---- CONNECT ---- */
    case MENU_CONNECT:
        switch (choice) {
            case 1: cmd_autoinit(); break;
            case 2: cmd_init("500000"); break;
            case 3: cmd_init("250000"); break;
            case 4: cmd_init("1000000"); break;
            case 5: cmd_init("125000"); break;
            default:
                Serial.println("  Invalid choice.");
                break;
        }
        if (g_menu_level == MENU_CONNECT) {
            /* After connect, go back to main */
            g_menu_level = MENU_MAIN;
            menu_show_main();
        }
        return true;

    /* ---- DIAG ---- */
    case MENU_DIAG:
        switch (choice) {
            case 1: cmd_ping(); break;
            case 2:
                if (!g_can_initialized) {
                    Serial.println("  Connect first.");
                } else {
                    cmd_scan();
                    /* After scan with exactly 1 module, prompt to program */
                    /* (scan prints "SCAN:DONE N module(s)" — user can
                     * select from ECU menu manually) */
                }
                break;
            case 3:
                if (!g_can_initialized) { Serial.println("  Connect first."); }
                else { cmd_read_dtc(); }
                break;
            case 4:
                if (!g_can_initialized) { Serial.println("  Connect first."); }
                else { cmd_clear_dtc(); }
                break;
            case 5:
                /* SD Card status */
                if (g_sd_ok) {
                    Serial.println("  SD card: OK");
                } else {
                    Serial.println("  SD card: not found");
                }
                break;
            case 6: cmd_setclock(""); break;
            case 7: cmd_status(); break;
            default:
                Serial.println("  Invalid choice.");
                break;
        }
        if (g_menu_level == MENU_DIAG) menu_show_diag();
        return true;

    /* ---- ECU SELECT ---- */
    case MENU_ECU:
        if (choice >= 1 && choice <= ECU_MENU_COUNT) {
            menu_select_module(choice - 1);
            g_menu_level = MENU_ECU_OPS;
            menu_show_ecu_ops();
        } else {
            Serial.println("  Invalid choice.");
            menu_show_ecu();
        }
        return true;

    /* ---- ECU OPERATIONS ---- */
    case MENU_ECU_OPS: {
        uint16_t caps = menu_active_caps();

        /* T87A fixed-layout dispatch — must match menu_show_ecu_ops render. */
        if (g_menu_module == MODULE_T87 && g_t87a_detected) {
            switch (choice) {
                case 1: cmd_calread();                    menu_show_ecu_ops(); return true;
                case 2: cmd_hsread(NULL);                 menu_show_ecu_ops(); return true;
                case 3: {
                    /* CAL Write — prompt for source file path (accepts
                     * 1 MB cal-only or 4 MB full-flash). Suggest the
                     * last CALREAD file on SD as a sensible default. */
                    Serial.println("  CAL Write — enter source file path on SD:");
                    Serial.println("  Example: /Read/CALREAD_24288836_<ts>.bin");
                    Serial.print("  Path: ");
                    char path_buf[128];
                    int ppos = 0;
                    uint32_t t0 = millis();
                    while (millis() - t0 < 60000) {
                        if (Serial.available()) {
                            char c = (char)Serial.read();
                            if (c == '\n' || c == '\r') { if (ppos > 0) break; }
                            else if (c == 0x08 || c == 0x7F) { if (ppos > 0) ppos--; }
                            else if (ppos < (int)sizeof(path_buf) - 1) path_buf[ppos++] = c;
                        }
                    }
                    path_buf[ppos] = '\0';
                    /* Don't re-echo path_buf — the terminal's local echo
                     * already shows what the user typed, and the subsequent
                     * "CALWRITE: source <path>" line confirms the parsed
                     * path cleanly. Echoing here was causing duplicate
                     * display on some terminals. */
                    Serial.println();
                    if (ppos > 0) {
                        cmd_t87a_calwrite(path_buf);
                    } else {
                        Serial.println("  Cancelled.");
                    }
                    menu_show_ecu_ops();
                    return true;
                }
                case 4: {
                    /* Full HS Write — prompt for source file path on SD.
                     * Accepts any 4 MB full-flash .bin (or larger; truncated
                     * to first 4 MB via file_size check). */
                    Serial.println("  Full Write — enter source file path on SD:");
                    Serial.println("  Example: /Write/T87A_OS-24288836_Unlock_Patched.bin");
                    Serial.print("  Path: ");
                    char path_buf[128];
                    int ppos = 0;
                    uint32_t t0 = millis();
                    while (millis() - t0 < 60000) {
                        if (Serial.available()) {
                            char c = (char)Serial.read();
                            if (c == '\n' || c == '\r') { if (ppos > 0) break; }
                            else if (c == 0x08 || c == 0x7F) { if (ppos > 0) ppos--; }
                            else if (ppos < (int)sizeof(path_buf) - 1) path_buf[ppos++] = c;
                        }
                    }
                    path_buf[ppos] = '\0';
                    Serial.println();
                    if (ppos > 0) {
                        cmd_t87a_hswrite(path_buf);
                    } else {
                        Serial.println("  Cancelled.");
                    }
                    menu_show_ecu_ops();
                    return true;
                }
                case 5: cmd_bamread();                    menu_show_ecu_ops(); return true;
                case 6: cmd_bamwrite();                   menu_show_ecu_ops(); return true;
                case 7: cmd_vin();                        menu_show_ecu_ops(); return true;
                case 8: {
                    /* VIN Write — inline prompt (same logic as generic path) */
                    Serial.print("  Enter 17-char VIN: ");
                    char vin_buf[24];
                    int vpos = 0;
                    uint32_t t0 = millis();
                    while (millis() - t0 < 30000) {
                        if (Serial.available()) {
                            char c = (char)Serial.read();
                            if (c == '\n' || c == '\r') { if (vpos > 0) break; }
                            else if (c == 0x08 || c == 0x7F) { if (vpos > 0) vpos--; }
                            else if (vpos < (int)sizeof(vin_buf) - 1) vin_buf[vpos++] = c;
                        }
                    }
                    vin_buf[vpos] = '\0';
                    if (vpos == 17) { Serial.println(vin_buf); cmd_vinwrite(vin_buf); }
                    else if (vpos == 0) { Serial.println("\n  Cancelled."); }
                    else { Serial.println(vin_buf); Serial.println("  ERR: VIN must be exactly 17 characters."); }
                    menu_show_ecu_ops();
                    return true;
                }
                case 9:
                    Serial.println("Reset: T87A — using kernel-exit kill stream (~2-3 min)");
                    (void)kexit_run_trailer_and_observe();
                    menu_show_ecu_ops();
                    return true;
                default:
                    Serial.println("  Invalid choice.");
                    menu_show_ecu_ops();
                    return true;
            }
        }

        int n = 1;

        /* Read options */
        if (caps & CAP_FULLREAD)     { if (choice == n) { cmd_fullread();     menu_show_ecu_ops(); return true; } n++; }
        if (caps & CAP_E92_FULLREAD) { if (choice == n) { cmd_e92_fullread(); menu_show_ecu_ops(); return true; } n++; }
        if (caps & CAP_CALREAD)      { if (choice == n) { cmd_calread();      menu_show_ecu_ops(); return true; } n++; }
        if (caps & CAP_BAMREAD)      { if (choice == n) { cmd_bamread();      menu_show_ecu_ops(); return true; } n++; }
        if (caps & CAP_HSREAD)       { if (choice == n) { cmd_hsread(NULL);   menu_show_ecu_ops(); return true; } n++; }
        if (caps & CAP_T42_READ)     { if (choice == n) { cmd_t42_read("A"); menu_show_ecu_ops(); return true; } n++; }
        if (caps & CAP_T42_READ)     { if (choice == n) { cmd_t42_read("B"); menu_show_ecu_ops(); return true; } n++; }
        if (caps & CAP_T42_READ)     { if (choice == n) { cmd_t42_read("C"); menu_show_ecu_ops(); return true; } n++; }
        if (caps & CAP_T42_READ)     { if (choice == n) { cmd_t42_read("D"); menu_show_ecu_ops(); return true; } n++; }
        if (caps & CAP_T42_READ)     { if (choice == n) { cmd_t42_read("E"); menu_show_ecu_ops(); return true; } n++; }
        /* Write options */
        if (caps & CAP_FULLWRITE)    { if (choice == n) { cmd_fullwrite();    menu_show_ecu_ops(); return true; } n++; }
        if (caps & CAP_CALWRITE)     { if (choice == n) { cmd_calwrite();     menu_show_ecu_ops(); return true; } n++; }
        if (caps & CAP_BAMWRITE)     { if (choice == n) { cmd_bamwrite();     menu_show_ecu_ops(); return true; } n++; }
        if (caps & CAP_HSWRITE)      { if (choice == n) { cmd_t87a_hswrite(NULL); menu_show_ecu_ops(); return true; } n++; }

        /* Always-available: VIN */
        if (choice == n) { cmd_vin(); menu_show_ecu_ops(); return true; }
        n++;

        /* Always-available: VIN Write (inline prompt) */
        if (choice == n) {
            Serial.print("  Enter 17-char VIN: ");
            char vin_buf[24];
            int vpos = 0;
            uint32_t t0 = millis();
            while (millis() - t0 < 30000) {
                if (Serial.available()) {
                    char c = (char)Serial.read();
                    if (c == '\n' || c == '\r') { if (vpos > 0) break; }
                    else if (c == 0x08 || c == 0x7F) { if (vpos > 0) vpos--; }
                    else if (vpos < (int)sizeof(vin_buf) - 1) vin_buf[vpos++] = c;
                }
            }
            vin_buf[vpos] = '\0';
            if (vpos == 17) {
                Serial.println(vin_buf);
                cmd_vinwrite(vin_buf);
            } else if (vpos == 0) {
                Serial.println("\n  Cancelled.");
            } else {
                Serial.println(vin_buf);
                Serial.println("  ERR: VIN must be exactly 17 characters.");
            }
            menu_show_ecu_ops();
            return true;
        }
        n++;

        /* Always-available: Reset. Smart branching for T87A — if the
         * Rollin Smoke kernel is resident, UDS $11 01 won't reach the OS
         * (kernel doesn't speak UDS). Use the proven 16 MB kill-stream
         * path instead, which MCU-resets the TCM and cold-boots the OS. */
        if (choice == n) {
            if (g_menu_module == MODULE_T87 && g_t87a_detected) {
                Serial.println("Reset: T87A — using kernel-exit kill stream (~2-3 min)");
                (void)kexit_run_trailer_and_observe();
            } else {
                cmd_reset("1");
            }
            menu_show_ecu_ops();
            return true;
        }

        Serial.println("  Invalid choice.");
        menu_show_ecu_ops();
        return true;
    }

    /* ---- BIN SELECT (from write flow) ---- */
    case MENU_BINSELECT:
        if (choice >= 1 && choice <= g_sd_bin_count) {
            g_sd_bin_selected = choice - 1;
            Serial.print("  Selected: "); Serial.println(g_sd_bins[g_sd_bin_selected]);
            g_menu_level = MENU_WRITETYPE;
            menu_show_writetype();
        } else {
            Serial.println("  Invalid choice.");
            menu_show_binselect();
        }
        return true;

    /* ---- WRITE TYPE (after bin selected) ---- */
    case MENU_WRITETYPE: {
        uint16_t caps = menu_active_caps();
        int n = 1;
        /* Build full path for the selected bin */
        char binpath[96];
        snprintf(binpath, sizeof(binpath), "%s%s",
                 module_bin_folder(g_menu_module),
                 g_sd_bins[g_sd_bin_selected]);

        if (caps & CAP_CALWRITE) {
            if (choice == n) {
                Serial.print("  >> CALWRITE from SD: "); Serial.println(binpath);
                strncpy(g_sd_filename, binpath, sizeof(g_sd_filename) - 1);
                g_menu_level = MENU_ECU_OPS;
                cmd_calwrite();
                menu_show_ecu_ops();
                return true;
            }
            n++;
        }
        if (caps & CAP_FULLWRITE) {
            if (choice == n) {
                Serial.print("  >> FULLWRITE from SD: "); Serial.println(binpath);
                strncpy(g_sd_filename, binpath, sizeof(g_sd_filename) - 1);
                g_menu_level = MENU_ECU_OPS;
                cmd_fullwrite();
                menu_show_ecu_ops();
                return true;
            }
            n++;
        }
        if (caps & CAP_BAMWRITE) {
            if (choice == n) {
                Serial.print("  >> BAMWRITE from SD: "); Serial.println(binpath);
                strncpy(g_sd_filename, binpath, sizeof(g_sd_filename) - 1);
                g_menu_level = MENU_ECU_OPS;
                cmd_bamwrite();
                menu_show_ecu_ops();
                return true;
            }
            n++;
        }
        Serial.println("  Invalid choice.");
        menu_show_writetype();
        return true;
    }

    /* ---- HACKER ---- */
    case MENU_HACKER:
        switch (choice) {
            case 1: cmd_kernel_popper(); break;
            case 2: cmd_bus_sniffer();   break;
            default:
                Serial.println("  Invalid choice.");
                break;
        }
        if (g_menu_level == MENU_HACKER) menu_show_hacker();
        return true;

    default:
        g_menu_level = MENU_OFF;
        return false;
    }
}

static void cmd_menu(void) {
    g_menu_level = MENU_MAIN;
    menu_show_main();
}

/* ================================================================ */

/* ---------- CAN bus capture (SavvyCAN CSV format) ---------- */
/*
 * CAPTURE [duration_ms]
 * Puts the CAN peripheral in accept-all mode and streams every received
 * frame over USB serial as a SavvyCAN-format CSV row. Output is directly
 * consumable by tools/extract_kernel*.py. Stops after duration_ms (default
 * 30000, max 600000) or on any serial byte from the host.
 */
static void cmd_capture(const char *arg)
{
    if (!g_can_initialized) { print_err("CAN not initialized — run INIT first"); return; }

    uint32_t duration_ms = arg ? (uint32_t)atol(arg) : 30000UL;
    if (duration_ms == 0)        duration_ms = 30000UL;
    if (duration_ms > 600000UL)  duration_ms = 600000UL;

    /* Accept-all filter, drain any stale frames */
    can_set_filter(0, 0);
    can_reset_overflow();
    {
        uint32_t _id; uint8_t _d[8]; uint8_t _l;
        while (can_receive(&_id, _d, &_l) == 0) { /* drain */ }
    }

    Serial.print("CAPTURE_START duration_ms=");
    Serial.println(duration_ms);
    Serial.println("Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8");

    const uint32_t start_ms = millis();
    const uint32_t start_us = micros();
    uint32_t frame_count = 0;
    char line[96];

    while ((millis() - start_ms) < duration_ms) {
        if (Serial.available()) { while (Serial.available()) Serial.read(); break; }

        uint32_t id; uint8_t data[8]; uint8_t len;
        if (can_receive(&id, data, &len) != 0) continue;

        uint32_t dt_us   = micros() - start_us;
        uint32_t dt_s    = dt_us / 1000000UL;
        uint32_t dt_frac = dt_us % 1000000UL;

        int n = snprintf(line, sizeof(line),
                         "%lu.%06lu,%lX,false,Rx,0,%u",
                         (unsigned long)dt_s, (unsigned long)dt_frac,
                         (unsigned long)id, (unsigned)len);
        for (int i = 0; i < 8 && n < (int)sizeof(line) - 4; i++) {
            if (i < len) n += snprintf(line + n, sizeof(line) - n, ",%02X", data[i]);
            else         n += snprintf(line + n, sizeof(line) - n, ",");
        }
        Serial.println(line);
        frame_count++;
    }

    Serial.print("CAPTURE_DONE frames=");
    Serial.print(frame_count);
    Serial.print(" dropped_ring=");
    Serial.print(can_get_overflow_count());
    Serial.print(" dropped_hw=");
    Serial.println(can_get_hw_overflow());
    print_ok();
}

static void cmd_status(void)
{
    Serial.println("=== J2534 Pass-Thru Status ===");
    Serial.print("Version:  "); Serial.println(PASSTHRU_VERSION);
    Serial.print("CAN init: "); Serial.println(g_can_initialized ? "YES" : "NO");
    Serial.print("Tester:   0x"); Serial.println(g_tester_id, HEX);
    Serial.print("ECU:      0x"); Serial.println(g_ecu_id, HEX);
    Serial.print("Module:   ");
    switch (g_module) {
        case MODULE_T87:  Serial.println(g_t87a_detected ? "T87A" : "T87"); break;
        case MODULE_E38:  Serial.println("E38"); break;
        case MODULE_E67:  Serial.println("E67"); break;
        case MODULE_T42:  Serial.println("T42"); break;
        case MODULE_E38N: Serial.println("E38N"); break;
        case MODULE_E92:  Serial.println("E92"); break;
        case MODULE_E40:  Serial.println("E40"); break;
        case MODULE_T93:  Serial.println("T93"); break;
        case MODULE_NONE: Serial.println("None (use MENU or ALGO to select)"); break;
        default:          Serial.println("?");   break;
    }
    Serial.print("AutoPing: "); Serial.println(g_auto_tester ? "ON" : "OFF");
    Serial.print("BusTP:    "); Serial.println(g_auto_broadcast ? "ON" : "OFF");
}

static void cmd_help(void)
{
    Serial.println("=== Commands ===");
    Serial.println("INIT [baud]         - Init CAN (default 500000)");
    Serial.println("SETID <tx> <rx>     - Set tester/ECU CAN IDs (hex)");
    Serial.println("DIAG [session]      - DiagnosticSessionControl");
    Serial.println("PING                - TesterPresent");
    Serial.println("AUTOPING [on|off]   - Auto TesterPresent");
    Serial.println("BUSTP [on|off]      - Auto 0x0101 broadcast TP (~3s)");
    Serial.println("AUTH [key_hex]      - SecurityAccess seed/key");
    Serial.println("ALGO <t87|e38|t42|e92|t93> - Set seed-key algo");
    Serial.println("KERNEL              - Upload kernel to ECU");
    Serial.println("READ <addr> <blocks>- Read ECU (GM: 0x800/blk)");
    Serial.println("VIN                 - Read VIN from ECU");
    Serial.println("SCAN                - Scan bus for all modules + VINs");
    Serial.println("OSID                - Read OS/Cal ID from ECU");
    Serial.println("FULLREAD            - Auto: VIN+OSID+AUTH+KERNEL+READ");
    Serial.println("CALREAD             - Cal-only read (E38: 256KB)");
    Serial.println("ERASE <addr> <size> - Erase flash region");
    Serial.println("WRITE <addr> <blks> - Write blocks from PC (kernel running)");
    Serial.println("CALWRITE            - Auto: cal-only write+verify (E38)");
    Serial.println("FULLWRITE           - Auto: full flash write+verify");
    Serial.println("TESTWRITE           - EXTKERN: erase+write 1 sector test");
    Serial.println("BAMREAD             - T87A: BAM boot-mode full read (4MB, ~7.5 min)");
    Serial.println("BAMWRITE            - T87A: BAM boot-mode write+verify");
    Serial.println("HSREAD              - T87A: HS-CAN full read via Rollin Smoke kernel (4MB, ~2.5 min, needs dual-unlock)");
    Serial.println("HSWRITE [file]      - T87A: HS-CAN write via external kernel");
    Serial.println("KEXIT               - T87A: experimental kernel-exit trailer (manual, run after HSREAD/CALREAD)");
    Serial.println("AUTOKEXIT [on|off]  - When on, HSREAD/CALREAD run the exit trailer inline before DONE");
    Serial.println("RESET [type]        - ECU Reset (1=hard 2=keyoff 3=soft)");
    Serial.println("RAW <hex>           - Send raw UDS request");
    Serial.println("CAPTURE [dur_ms]    - Log CAN frames as SavvyCAN CSV (default 30000ms)");
    Serial.println("KERNEL              - Upload Flashy clean-room E92 kernel + PING test");
    Serial.println("STATUS              - Show connection state");
    Serial.println("MENU                - Interactive module menu");
    Serial.println("--- SD browser ---");
    Serial.println("SDLIST [path]       - List files+dirs in path (default /)");
    Serial.println("SDTREE [path]       - Recursive listing (max depth 3)");
    Serial.println("SDSTAT              - Card capacity + free space");
    Serial.println("SDPEEK <f> [off] [n]- Hex+ASCII dump (xxd-style, max 256 B)");
    Serial.println("SDDELETE <path>     - Remove file");
    Serial.println("SDWRITE <path> <sz> - Stage file from PC over serial");
    Serial.println("HELP                - This message");
}

/* ---------- SD card commands ---------- */

/* List one directory. Shows subdirectories too (marked as <DIR>) so the user
 * can see /Read/ and /Write/ from the root and navigate in. */
static void sdlist_dir(const char *path)
{
    FsFile dir = g_sd.open(path);
    if (!dir) { print_err("SD open path failed"); return; }
    if (!dir.isDir()) {
        dir.close();
        print_err("path is not a directory");
        return;
    }

    Serial.print("SDLIST:START path=");
    Serial.println(path);

    int files = 0;
    int dirs  = 0;
    FsFile entry;
    while (entry.openNext(&dir, O_RDONLY)) {
        char name[64];
        entry.getName(name, sizeof(name));
        if (entry.isDir()) {
            Serial.print("SDDIR:");
            Serial.println(name);
            dirs++;
        } else {
            Serial.print("SDFILE:");
            Serial.print(name);
            Serial.print(":");
            Serial.println(entry.size());
            files++;
        }
        entry.close();
    }
    dir.close();

    Serial.print("SDLIST:END files=");
    Serial.print(files);
    Serial.print(" dirs=");
    Serial.println(dirs);
    print_ok();
}

static void cmd_sdlist(const char *arg)
{
    if (!g_sd_ok) { print_err("no SD card"); return; }
    const char *path = "/";
    char buf[128];
    if (arg && *arg) {
        strncpy(buf, arg, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        /* trim trailing whitespace */
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\r' || buf[n-1] == '\n')) {
            buf[--n] = '\0';
        }
        if (n > 0) path = buf;
    }
    sdlist_dir(path);
}

/* Recursive tree listing, bounded to 3 levels deep so a huge tree doesn't
 * flood the serial buffer. Prints "SDTREE:<depth>:<type>:<path>:<size>". */
static void sdtree_walk(const char *path, int depth)
{
    if (depth > 3) return;
    FsFile dir = g_sd.open(path);
    if (!dir || !dir.isDir()) { if (dir) dir.close(); return; }

    FsFile entry;
    while (entry.openNext(&dir, O_RDONLY)) {
        char name[64];
        entry.getName(name, sizeof(name));

        /* build child path ("/<parent>/<name>") */
        char child[200];
        if (strcmp(path, "/") == 0) {
            snprintf(child, sizeof(child), "/%s", name);
        } else {
            snprintf(child, sizeof(child), "%s/%s", path, name);
        }

        if (entry.isDir()) {
            Serial.print("SDTREE:");
            Serial.print(depth);
            Serial.print(":D:");
            Serial.println(child);
            entry.close();
            sdtree_walk(child, depth + 1);
        } else {
            Serial.print("SDTREE:");
            Serial.print(depth);
            Serial.print(":F:");
            Serial.print(child);
            Serial.print(":");
            Serial.println(entry.size());
            entry.close();
        }
    }
    dir.close();
}

static void cmd_sdtree(const char *arg)
{
    if (!g_sd_ok) { print_err("no SD card"); return; }
    const char *path = "/";
    char buf[128];
    if (arg && *arg) {
        strncpy(buf, arg, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\r' || buf[n-1] == '\n')) {
            buf[--n] = '\0';
        }
        if (n > 0) path = buf;
    }
    Serial.print("SDTREE:START path=");
    Serial.println(path);
    sdtree_walk(path, 0);
    Serial.println("SDTREE:END");
    print_ok();
}

/* Card capacity + free space + top-level entry count. */
static void cmd_sdstat(void)
{
    if (!g_sd_ok) { print_err("no SD card"); return; }

    /* SdFat clusterCount/freeClusterCount + bytesPerCluster give us totals */
    uint32_t sectors_per_cluster = g_sd.sectorsPerCluster();
    uint32_t cluster_count       = g_sd.clusterCount();
    uint32_t free_clusters       = g_sd.freeClusterCount();
    /* bytes per sector is always 512 on FAT cards */
    uint64_t bytes_per_cluster   = (uint64_t)sectors_per_cluster * 512ULL;
    uint64_t total_bytes         = bytes_per_cluster * (uint64_t)cluster_count;
    uint64_t free_bytes          = bytes_per_cluster * (uint64_t)free_clusters;
    uint64_t used_bytes          = (total_bytes > free_bytes) ? (total_bytes - free_bytes) : 0;

    Serial.print("SDSTAT:TOTAL_MB "); Serial.println((unsigned long)(total_bytes / (1024ULL * 1024ULL)));
    Serial.print("SDSTAT:USED_MB  "); Serial.println((unsigned long)(used_bytes  / (1024ULL * 1024ULL)));
    Serial.print("SDSTAT:FREE_MB  "); Serial.println((unsigned long)(free_bytes  / (1024ULL * 1024ULL)));

    /* Count files+dirs in root as quick sanity metric */
    int files = 0, dirs = 0;
    FsFile dir = g_sd.open("/");
    if (dir) {
        FsFile entry;
        while (entry.openNext(&dir, O_RDONLY)) {
            if (entry.isDir()) dirs++; else files++;
            entry.close();
        }
        dir.close();
    }
    Serial.print("SDSTAT:ROOT_FILES "); Serial.println(files);
    Serial.print("SDSTAT:ROOT_DIRS  "); Serial.println(dirs);
    print_ok();
}

/* KNOWN ISSUE (2026-04-24): SDPEEK silently hangs (no output, no ERR) when
 * the requested offset is >= 0x10000 (64 KB). Smaller offsets work fine.
 * Suspected SdFat seekSet behaviour on large offsets in this driver
 * version. Workaround: copy the .bin off SD to a PC and inspect with xxd
 * or hexdump. Offsets up to 0xFFFF are fully usable inside SDPEEK. */
static void cmd_sdpeek(const char *arg)
{
    if (!g_sd_ok) { print_err("no SD card"); return; }
    if (!arg) { print_err("usage: SDPEEK <filename> [offset] [len]"); return; }

    char buf[128];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *fname = strtok(buf, " ");
    char *ostr  = strtok(NULL, " ");
    char *lstr  = strtok(NULL, " ");

    uint32_t offset = ostr ? (uint32_t)strtoul(ostr, NULL, 0) : 0;
    uint32_t len    = lstr ? (uint32_t)strtoul(lstr, NULL, 0) : 64;
    if (len > 256) len = 256;

    FsFile f = g_sd.open(fname, O_RDONLY);
    if (!f) { print_err("file not found"); return; }

    Serial.print("SDPEEK: ");
    Serial.print(fname);
    Serial.print(" size=");
    Serial.print(f.size());
    Serial.print(" offset=0x");
    Serial.print(offset, HEX);
    Serial.print(" len=");
    Serial.println(len);

    f.seekSet(offset);
    uint8_t data[256];
    int got = f.read(data, len);
    f.close();

    if (got <= 0) { print_err("read failed"); return; }

    /* xxd-style: "OFFSET: HEX*16  |ASCII|" per 16-byte row */
    for (int row = 0; row < got; row += 16) {
        int row_len = (got - row) < 16 ? (got - row) : 16;
        /* offset column (absolute file offset) */
        uint32_t row_off = offset + (uint32_t)row;
        if (row_off < 0x10) Serial.print("0000000");
        else if (row_off < 0x100) Serial.print("000000");
        else if (row_off < 0x1000) Serial.print("00000");
        else if (row_off < 0x10000) Serial.print("0000");
        else if (row_off < 0x100000) Serial.print("000");
        else if (row_off < 0x1000000) Serial.print("00");
        else if (row_off < 0x10000000) Serial.print("0");
        Serial.print(row_off, HEX);
        Serial.print(": ");
        /* hex bytes */
        for (int i = 0; i < 16; i++) {
            if (i < row_len) {
                uint8_t b = data[row + i];
                if (b < 0x10) Serial.print('0');
                Serial.print(b, HEX);
            } else {
                Serial.print("  ");
            }
            Serial.print(' ');
        }
        /* ASCII column */
        Serial.print('|');
        for (int i = 0; i < row_len; i++) {
            uint8_t b = data[row + i];
            Serial.print((char)((b >= 0x20 && b <= 0x7E) ? b : '.'));
        }
        Serial.println('|');
    }
    print_ok();
}

static void cmd_sddelete(const char *arg)
{
    if (!g_sd_ok) { print_err("no SD card"); return; }
    if (!arg) { print_err("usage: SDDELETE <filename>"); return; }
    if (g_sd.remove(arg)) {
        print_ok();
    } else {
        print_err("delete failed");
    }
}

/* ---------- SDWRITE: receive file over serial (hex-encoded chunks) ---------- */
static FsFile g_sdwrite_file;
static uint32_t g_sdwrite_total = 0;
static uint32_t g_sdwrite_received = 0;
static bool g_sdwrite_active = false;

static void cmd_sdwrite(const char *arg)
{
    if (!g_sd_ok) { print_err("no SD card"); return; }
    if (!arg) { print_err("usage: SDWRITE <filename> <size>"); return; }

    char buf[128];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *fname = strtok(buf, " ");
    char *sstr  = strtok(NULL, " ");

    if (!fname || !sstr) { print_err("usage: SDWRITE <filename> <size>"); return; }

    g_sdwrite_total = (uint32_t)strtoul(sstr, NULL, 0);
    if (g_sdwrite_total == 0) { print_err("invalid size"); return; }

    g_sd.remove(fname);
    g_sdwrite_file = g_sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
    if (!g_sdwrite_file) { print_err("cannot create file"); return; }
    g_sdwrite_file.preAllocate(g_sdwrite_total);

    g_sdwrite_received = 0;
    g_sdwrite_active = true;

    Serial.print("SDWRITE:READY ");
    Serial.print(fname);
    Serial.print(" ");
    Serial.println(g_sdwrite_total);
}

/* Called from process_command when g_sdwrite_active is true.
   Expects hex-encoded lines: "SDDATA:<hex>" or "SDDONE" or "SDABORT" */
static void sdwrite_process_line(char *line)
{
    if (strncmp(line, "SDABORT", 7) == 0) {
        g_sdwrite_file.close();
        g_sdwrite_active = false;
        Serial.println("SDWRITE:ABORTED");
        return;
    }

    if (strncmp(line, "SDDONE", 6) == 0) {
        g_sdwrite_file.truncate(g_sdwrite_received);
        g_sdwrite_file.close();
        g_sdwrite_active = false;
        Serial.print("SDWRITE:DONE bytes=");
        Serial.println(g_sdwrite_received);
        return;
    }

    /* Expect hex data line: just raw hex bytes, no prefix needed */
    /* Strip any "SDDATA:" prefix if present */
    if (strncmp(line, "SDDATA:", 7) == 0) line += 7;

    /* Decode hex pairs */
    uint8_t chunk[512];
    int chunk_len = 0;
    for (int i = 0; line[i] && line[i+1] && chunk_len < (int)sizeof(chunk); i += 2) {
        /* Skip whitespace */
        while (line[i] == ' ') i++;
        if (!line[i] || !line[i+1]) break;

        uint8_t hi = 0, lo = 0;
        char c = line[i];
        if      (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else break;

        c = line[i+1];
        if      (c >= '0' && c <= '9') lo = c - '0';
        else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
        else break;

        chunk[chunk_len++] = (hi << 4) | lo;
    }

    if (chunk_len == 0) return;

    g_sdwrite_file.write(chunk, chunk_len);
    g_sdwrite_received += chunk_len;

    /* ACK with progress */
    Serial.print("SDWRITE:ACK ");
    Serial.print(g_sdwrite_received);
    Serial.print("/");
    Serial.println(g_sdwrite_total);
}

/* ---------- Command dispatcher ---------- */

static void process_command(char *line)
{
    /* Trim leading whitespace / control chars */
    while (*line && (*line <= ' ')) line++;

    /* Trim trailing whitespace */
    int len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n' || line[len-1] == ' ')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    /* SDWRITE mode: intercept all lines for hex data transfer */
    if (g_sdwrite_active) {
        sdwrite_process_line(line);
        return;
    }

    /* MENU mode: intercept numbered input (but allow MENU command to pass through) */
    if (g_menu_level != MENU_OFF) {
        /* Uppercase single-char for B/X detection */
        char upper[SERIAL_CMD_MAX_LEN];
        strncpy(upper, line, sizeof(upper) - 1);
        upper[sizeof(upper) - 1] = '\0';
        for (int i = 0; upper[i]; i++) {
            if (upper[i] >= 'a' && upper[i] <= 'z') upper[i] -= 32;
        }
        /* Let MENU command pass through to re-enter top */
        if (strcmp(upper, "MENU") != 0) {
            if (menu_handle_input(upper)) return;
        }
    }

    /* Split command and argument */
    char *cmd = line;
    char *arg = NULL;
    for (int i = 0; i < len; i++) {
        if (line[i] == ' ') {
            line[i] = '\0';
            arg = &line[i + 1];
            break;
        }
        /* Uppercase the command portion */
        if (line[i] >= 'a' && line[i] <= 'z') {
            line[i] -= ('a' - 'A');
        }
    }

    if      (strcmp(cmd, "INIT")     == 0) cmd_init(arg);
    else if (strcmp(cmd, "AUTOINIT") == 0) cmd_autoinit();
    else if (strcmp(cmd, "SETID")    == 0) cmd_setid(arg);
    else if (strcmp(cmd, "DIAG")     == 0) cmd_diag(arg);
    else if (strcmp(cmd, "PING")     == 0) cmd_ping();
    else if (strcmp(cmd, "AUTH")     == 0) cmd_auth(arg);
    else if (strcmp(cmd, "ALGO")     == 0) cmd_algo(arg);
    else if (strcmp(cmd, "KERNEL")   == 0) cmd_kernel(arg);
    else if (strcmp(cmd, "READ")     == 0) cmd_read(arg);
    else if (strcmp(cmd, "VIN")      == 0) cmd_vin();
    else if (strcmp(cmd, "VINWRITE") == 0) cmd_vinwrite(arg);
    else if (strcmp(cmd, "SCAN")     == 0) cmd_scan();
    else if (strcmp(cmd, "OSID")     == 0) cmd_osid();
    else if (strcmp(cmd, "FULLREAD") == 0) cmd_fullread();
    else if (strcmp(cmd, "CALREAD")  == 0) cmd_calread();
    else if (strcmp(cmd, "ERASE")    == 0) cmd_erase(arg);
    else if (strcmp(cmd, "WRITE")    == 0) cmd_write(arg);
    else if (strcmp(cmd, "CALWRITE") == 0) {
        /* If user passed a file path, they're targeting the T87A EXTKERN
         * cal-write path (the only CALWRITE variant that accepts a path).
         * Route directly — cmd_t87a_calwrite handles auth + kernel bring-up
         * internally, so module-pre-select isn't required. */
        if (arg && *arg) {
            cmd_t87a_calwrite(arg);
        } else {
            cmd_calwrite();
        }
    }
    else if (strcmp(cmd, "FULLWRITE")== 0) cmd_fullwrite();
    else if (strcmp(cmd, "TESTWRITE")== 0) cmd_testwrite();
    else if (strcmp(cmd, "BAMREAD")  == 0) cmd_bamread();
    else if (strcmp(cmd, "HSREAD")   == 0) cmd_hsread(arg);
    else if (strcmp(cmd, "HSWRITE")  == 0) cmd_t87a_hswrite(arg);
    else if (strcmp(cmd, "KEXIT")    == 0) cmd_kexit();
    else if (strcmp(cmd, "AUTOKEXIT")== 0) cmd_autokexit(arg);
    else if (strcmp(cmd, "BAMWRITE") == 0) {
        if (arg) { strncpy(g_sd_filename, arg, sizeof(g_sd_filename)-1); g_sd_filename[sizeof(g_sd_filename)-1]='\0'; }
        cmd_bamwrite();
        g_sd_filename[0] = '\0';
    }
    else if (strcmp(cmd, "RESET")    == 0) cmd_reset(arg);
    else if (strcmp(cmd, "RAW")      == 0) cmd_raw(arg);
    else if (strcmp(cmd, "CANSEND")  == 0) cmd_cansend(arg);
    else if (strcmp(cmd, "CAPTURE")  == 0) cmd_capture(arg);
    else if (strcmp(cmd, "KERNEL")     == 0) cmd_kernel_upload();
    else if (strcmp(cmd, "KERNELREAD") == 0) cmd_kernel_read(arg);
    else if (strcmp(cmd, "E92FULLREAD")== 0) cmd_e92_fullread();
    else if (strcmp(cmd, "E92ID")      == 0) cmd_e92id();
    else if (strcmp(cmd, "E92SAPROBE") == 0) cmd_e92saprobe();
    else if (strcmp(cmd, "T42READ")    == 0) cmd_t42_read(arg);
#if KERNEL_T42_USBJTAG_AVAILABLE
    else if (strcmp(cmd, "T42USB")     == 0) cmd_t42_usb(arg);
#endif
    else if (strcmp(cmd, "T42DUMP")    == 0) cmd_t42_dump(arg);
    else if (strcmp(cmd, "T42RA_READ") == 0) cmd_t42_ra_read(arg);
    else if (strcmp(cmd, "KLIST")      == 0) kernel_registry_klist();
    else if (strcmp(cmd, "KUSE")       == 0) {
        if (!arg || !*arg) { print_err("usage: KUSE <id>  (see KLIST)"); }
        else {
            const kernel_entry_t *e = kernel_select_by_id(arg);
            if (!e) { Serial.print("KUSE: no kernel with id '");
                      Serial.print(arg); Serial.println("'"); }
            else { Serial.print("KUSE: selected "); Serial.print(e->id);
                   Serial.print(" ("); Serial.print(e->display_name);
                   Serial.println(")"); }
        }
    }
    else if (strcmp(cmd, "SET")        == 0) cmd_set(arg);
    else if (strcmp(cmd, "PARAMS")     == 0) cmd_params();
    else if (strcmp(cmd, "SETCLOCK")   == 0) cmd_setclock(arg);
    else if (strcmp(cmd, "CLOCK")      == 0) cmd_setclock("");
    else if (strcmp(cmd, "STATUS")   == 0) cmd_status();
    else if (strcmp(cmd, "SDLIST")   == 0) cmd_sdlist(arg);
    else if (strcmp(cmd, "SDTREE")   == 0) cmd_sdtree(arg);
    else if (strcmp(cmd, "SDSTAT")   == 0) cmd_sdstat();
    else if (strcmp(cmd, "SDPEEK")   == 0) cmd_sdpeek(arg);
    else if (strcmp(cmd, "SDDELETE") == 0) cmd_sddelete(arg);
    else if (strcmp(cmd, "SDWRITE")  == 0) cmd_sdwrite(arg);
    else if (strcmp(cmd, "HELP")     == 0) cmd_help();
    else if (strcmp(cmd, "MENU")     == 0) cmd_menu();
    else if (strcmp(cmd, "LEDWAIT")  == 0) {
        uint32_t ms = arg ? (uint32_t)atol(arg) : 10000;
        led_waiting(ms);
        print_ok();
    }
    else if (strcmp(cmd, "LEDPARTY") == 0) {
        led_celebrate();
        print_ok();
    }
    else if (strcmp(cmd, "AUTOPING") == 0) {
        if (arg && (strcmp(arg, "on") == 0 || strcmp(arg, "ON") == 0)) {
            g_auto_tester = true;
        } else {
            g_auto_tester = false;
        }
        Serial.print("AutoPing: "); Serial.println(g_auto_tester ? "ON" : "OFF");
        print_ok();
    }
    else if (strcmp(cmd, "BUSTP") == 0) {
        if (arg && (strcmp(arg, "on") == 0 || strcmp(arg, "ON") == 0)) {
            g_auto_broadcast = true;
            send_broadcast_tp();  /* send one immediately */
        } else {
            g_auto_broadcast = false;
        }
        Serial.print("BusTP (0x0101): "); Serial.println(g_auto_broadcast ? "ON" : "OFF");
        print_ok();
    }
    else {
        Serial.print("Unknown command: ");
        Serial.println(cmd);
    }
}

/* ---------- Arduino entry points ---------- */

void setup()
{
    /* Enable NeoPixel power gate (pin 7 on Feather M4 CAN) */
    pinMode(PIN_NEOPIXEL_POWER, OUTPUT);
    digitalWrite(PIN_NEOPIXEL_POWER, HIGH);
    delay(50);  /* let power gate stabilize before talking to NeoPixel */

    pixel.begin();
    pixel.setBrightness(200);
    led_idle();  /* show blue immediately — don't wait for serial */

    Serial.begin(SERIAL_BAUD);
    while (!Serial) delay(10);

    Serial.println("=============================");
    Serial.println(" J2534 Pass-Thru v" PASSTHRU_VERSION);
    Serial.println(" Feather M4 CAN (SAME51)");
    Serial.println("=============================");

    /* SD card init (Adalogger FeatherWing, CS=10) */
    if (g_sd.begin(SD_CS_PIN, SD_SCK_MHZ(12))) {
        g_sd_ok = true;
        Serial.println("SD card: OK");

        /* Ensure /Read and /Write directories exist on first boot.
         * /Read  — where SD-direct reads land (BAMREAD, FULLREAD, etc.)
         * /Write — default location MENU's write flow looks for source
         *          .bin files. Users drop files here from a card reader.
         * Both dirs are silently no-op if they already exist. */
        if (!g_sd.exists("/Read"))  { g_sd.mkdir("/Read"); }
        if (!g_sd.exists("/Write")) { g_sd.mkdir("/Write"); }
    } else {
        g_sd_ok = false;
        Serial.println("SD card: not found (reads will use serial only)");
    }

    /* PCF8523 RTC (optional on Adalogger). I2C already started by Wire. */
    Wire.begin();
    if (g_rtc.begin()) {
        g_rtc_ok = true;
        if (!g_rtc.initialized() || g_rtc.lostPower()) {
            Serial.println("RTC: present, time not set — run SETCLOCK YYMMDD HHMM");
        } else {
            DateTime n = g_rtc.now();
            Serial.print("RTC: ");
            Serial.print(n.year());   Serial.print('-');
            if (n.month()  < 10) Serial.print('0'); Serial.print(n.month());  Serial.print('-');
            if (n.day()    < 10) Serial.print('0'); Serial.print(n.day());    Serial.print(' ');
            if (n.hour()   < 10) Serial.print('0'); Serial.print(n.hour());   Serial.print(':');
            if (n.minute() < 10) Serial.print('0'); Serial.print(n.minute()); Serial.print(':');
            if (n.second() < 10) Serial.print('0'); Serial.println(n.second());
        }
        g_rtc.start();   /* clear stop bit if set */
    } else {
        Serial.println("RTC: not detected (fallback to SETCLOCK soft clock)");
    }

    /* Auto-detect CAN baud at boot. If an ECU is already powered on,
     * this saves the user from typing INIT. ~4 s sweep (1 s per rate).
     * If no bus activity, silently skip — user can INIT or AUTOINIT later. */
    Serial.print("CAN auto-detect: ");
    Serial.flush();
    {
        uint32_t baud = can_autobaud();
        if (baud) {
            isotp_init_link(&g_isotp_link, g_tester_id,
                            g_isotp_send_buf, sizeof(g_isotp_send_buf),
                            g_isotp_recv_buf, sizeof(g_isotp_recv_buf));
            uds_set_poll_callback(poll_can_rx);
            g_can_initialized = true;
            g_can_baud_rate   = baud;
            Serial.print(baud); Serial.println(" bps");
        } else {
            Serial.println("no bus (use INIT or AUTOINIT)");
        }
    }

    /* Auto-launch into the interactive menu. Users who want raw command
     * entry can select option 6 (RAW) from the main menu, which drops
     * back to the `>` prompt. Typing MENU from there re-enters.
     *
     * Main menu already has HELP as option 1 and RAW as option 6 —
     * those are the two escape hatches. See menu_show_main(). */
    cmd_menu();
}

void loop()
{
    /* 1. Read serial input */
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (g_cmd_pos > 0) {
                g_cmd_buf[g_cmd_pos] = '\0';
                process_command(g_cmd_buf);
                g_cmd_pos = 0;
                Serial.print("> ");
            }
        } else if (c == 0x08 || c == 0x7F) {
            /* Backspace / DEL — drop last char so typed corrections don't
             * leak into commands (VINWRITE length check, etc.). */
            if (g_cmd_pos > 0) g_cmd_pos--;
        } else if (g_cmd_pos < sizeof(g_cmd_buf) - 1) {
            g_cmd_buf[g_cmd_pos++] = c;
        }
    }

    /* 2. Poll CAN RX → ISO-TP */
    if (g_can_initialized) {
        poll_can_rx();
        isotp_poll(&g_isotp_link);
    }

    /* 3. Auto TesterPresent (UDS 3E to specific ECU) */
    if (g_can_initialized && g_auto_tester) {
        uint32_t now = millis();
        if ((now - g_last_tester_ms) >= TESTER_PRESENT_INTERVAL) {
            g_last_tester_ms = now;
            uds_tester_present(&g_isotp_link);
        }
    }

    /* 4. Auto 0x0101 bus broadcast TP (keeps all nodes alive) */
    send_broadcast_tp_if_due();

    /* 5. Update LED blink */
    led_update();
}
