/* kernel_registry.cpp — build the KERNELS[] table from auto-generated
 * headers and expose lookup/select API to the rest of Flashy.
 *
 * Auto-generated pieces:
 *   kernels_public.h   — one #include per committed kernel + X-macro list
 *   kernels_private.h  — same for user-local kernels (gitignored)
 *   kernels_generated/<id>.h — per-kernel blob + metadata macros
 *
 * We use X-macros to unroll the lists without dynamic memory.
 */
#include <Arduino.h>
#include <string.h>
#include "kernel_registry.h"

/* These two headers are auto-generated and may be empty (no kernels found).
 * Fallback guards keep the build green even then. */
#include "kernels_public.h"
#if __has_include("kernels_private.h")
#include "kernels_private.h"
#else
#define KERNEL_PRIVATE_LIST(X) /* empty */
#endif

#ifndef KERNEL_PUBLIC_LIST
#define KERNEL_PUBLIC_LIST(X) /* empty */
#endif

/* Translate upload format string literals into enum values at compile time.
 * The per-kernel headers #define KERNEL_<ID>_UPLOAD_RD as a string literal
 * ("size3", "size4", "addr4"). We wrap each kernel's enum selection in
 * inline helpers. */
static inline kernel_rd_fmt_t rd_from_str(const char *s) {
    if (!s) return KERNEL_RD_SIZE3;
    if (strcmp(s, "size4") == 0) return KERNEL_RD_SIZE4;
    if (strcmp(s, "addr4") == 0) return KERNEL_RD_ADDR4;
    return KERNEL_RD_SIZE3;
}
static inline kernel_td_fmt_t td_from_str(const char *s) {
    if (!s) return KERNEL_TD_ADDR4;
    if (strcmp(s, "sized") == 0) return KERNEL_TD_SIZED;
    return KERNEL_TD_ADDR4;
}

/* Build an entry struct from a per-kernel header's macros.
 * SRC = KERNEL_SRC_PUBLIC or KERNEL_SRC_PRIVATE. */
#define MAKE_ENTRY(ID_UPPER, SRC) { \
    KERNEL_##ID_UPPER##_ID, \
    KERNEL_##ID_UPPER##_TARGET, \
    KERNEL_##ID_UPPER##_DISPLAY, \
    (SRC), \
    KERNEL_##ID_UPPER##_BLOB, \
    KERNEL_##ID_UPPER##_SIZE, \
    KERNEL_##ID_UPPER##_LOAD_ADDR, \
    rd_from_str(KERNEL_##ID_UPPER##_UPLOAD_RD), \
    td_from_str(KERNEL_##ID_UPPER##_UPLOAD_TD), \
    KERNEL_##ID_UPPER##_BLOCK_SEQ, \
    (bool)KERNEL_##ID_UPPER##_USE_TXE, \
    (bool)KERNEL_##ID_UPPER##_AUTO_JUMP, \
    KERNEL_##ID_UPPER##_BOOT_DELAY_MS, \
    KERNEL_##ID_UPPER##_PROBE_SVC, \
    KERNEL_##ID_UPPER##_PROBE_PID, \
    KERNEL_##ID_UPPER##_EXPECTED_SIG, \
},

static const kernel_entry_t KERNELS[] = {
#define X(ID_UPPER) MAKE_ENTRY(ID_UPPER, KERNEL_SRC_PUBLIC)
    KERNEL_PUBLIC_LIST(X)
#undef X
#define X(ID_UPPER) MAKE_ENTRY(ID_UPPER, KERNEL_SRC_PRIVATE)
    KERNEL_PRIVATE_LIST(X)
#undef X
};

#undef MAKE_ENTRY

static const int KERNEL_COUNT_VAL =
    sizeof(KERNELS) / sizeof(KERNELS[0]);

static const kernel_entry_t *g_selected = nullptr;

int kernel_count(void) {
    return KERNEL_COUNT_VAL;
}

const kernel_entry_t *kernel_at(int index) {
    if (index < 0 || index >= KERNEL_COUNT_VAL) return nullptr;
    return &KERNELS[index];
}

const kernel_entry_t *kernel_find_by_id(const char *id) {
    if (!id) return nullptr;
    for (int i = 0; i < KERNEL_COUNT_VAL; i++) {
        if (strcmp(KERNELS[i].id, id) == 0) return &KERNELS[i];
    }
    return nullptr;
}

const kernel_entry_t *kernel_find_default(const char *target) {
    if (!target) return nullptr;
    /* Prefer public entries first (stable defaults), then private. */
    for (int i = 0; i < KERNEL_COUNT_VAL; i++) {
        if (KERNELS[i].src == KERNEL_SRC_PUBLIC
            && strcmp(KERNELS[i].target, target) == 0) {
            return &KERNELS[i];
        }
    }
    for (int i = 0; i < KERNEL_COUNT_VAL; i++) {
        if (strcmp(KERNELS[i].target, target) == 0) return &KERNELS[i];
    }
    return nullptr;
}

const kernel_entry_t *kernel_selected(void) {
    return g_selected;
}

const kernel_entry_t *kernel_select_by_id(const char *id) {
    const kernel_entry_t *e = kernel_find_by_id(id);
    if (e) g_selected = e;
    return e;
}

void kernel_registry_klist(void) {
    if (KERNEL_COUNT_VAL == 0) {
        Serial.println("KLIST: no kernels registered (check Cernels/ + build log)");
        return;
    }
    Serial.println("KLIST:");
    Serial.println("  ID                        TARGET  SRC      SIZE   DISPLAY");
    for (int i = 0; i < KERNEL_COUNT_VAL; i++) {
        const kernel_entry_t *e = &KERNELS[i];
        bool sel = (g_selected == e);
        Serial.print(sel ? " *" : "  ");
        /* ID padded to 24 */
        char idbuf[25];
        int n = snprintf(idbuf, sizeof(idbuf), "%-24s", e->id);
        (void)n;
        Serial.print(idbuf);
        Serial.print(" ");
        /* target padded to 6 */
        char tbuf[7];
        snprintf(tbuf, sizeof(tbuf), "%-6s", e->target);
        Serial.print(tbuf);
        Serial.print(" ");
        Serial.print(e->src == KERNEL_SRC_PUBLIC  ? "public " :
                     e->src == KERNEL_SRC_PRIVATE ? "private" : "???    ");
        Serial.print(" ");
        /* size right-padded to 6 */
        char sbuf[8];
        snprintf(sbuf, sizeof(sbuf), "%5u ", (unsigned)e->blob_size);
        Serial.print(sbuf);
        Serial.print(" ");
        Serial.println(e->display_name ? e->display_name : "");
    }
    Serial.print("Total: ");
    Serial.println(KERNEL_COUNT_VAL);
    if (g_selected) {
        Serial.print("Selected: "); Serial.println(g_selected->id);
    } else {
        Serial.println("Selected: (none — will default to first public match on use)");
    }
}
