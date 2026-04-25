/* kernel_registry.h — runtime table of available kernels.
 *
 * Built at compile time from the auto-generated kernels_public.h +
 * kernels_private.h (emitted by tools/build_kernel_registry.py as a
 * PlatformIO pre-build step).
 *
 * Runtime users (main.cpp) get:
 *   - kernel_count()                 — how many kernels compiled in
 *   - kernel_at(i)                   — entry by index
 *   - kernel_find_by_id(id)          — lookup by folder-name ID
 *   - kernel_find_default(target)    — first public entry for a target
 *   - kernel_selected()              — currently selected entry (or NULL)
 *   - kernel_select_by_id(id)        — change selection
 */
#ifndef KERNEL_REGISTRY_H
#define KERNEL_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    KERNEL_SRC_PUBLIC  = 0,
    KERNEL_SRC_PRIVATE = 1,
} kernel_src_t;

typedef enum {
    KERNEL_RD_SIZE3 = 0,   /* $34 with 3-byte size */
    KERNEL_RD_SIZE4 = 1,   /* $34 with 4-byte size */
    KERNEL_RD_ADDR4 = 2,   /* $34 with 4-byte load address (legacy GM) */
} kernel_rd_fmt_t;

typedef enum {
    KERNEL_TD_ADDR4 = 0,   /* $36 with 4-byte address + block_seq */
    KERNEL_TD_SIZED = 1,   /* length-prefixed payload */
} kernel_td_fmt_t;

typedef struct {
    const char *id;                 /* folder-name ID, e.g. "t42_read"     */
    const char *target;             /* e.g. "T42", "T87A", "E92"           */
    const char *display_name;       /* free-form string (user's preference)*/
    kernel_src_t src;               /* PUBLIC or PRIVATE                   */
    const uint8_t *blob;            /* kernel bytes                        */
    uint32_t blob_size;             /* kernel length                       */
    uint32_t load_addr;             /* ECU RAM destination                 */
    kernel_rd_fmt_t rd_fmt;
    kernel_td_fmt_t td_fmt;
    uint8_t block_seq;              /* $36 block sequence byte             */
    bool use_txe;                   /* send $37 TransferExit after $36     */
    bool auto_jump;                 /* bootloader jumps on $36 complete    */
    uint16_t boot_delay_ms;         /* wait after upload before probe      */
    uint8_t probe_svc;              /* 0 = no probe expected               */
    uint8_t probe_pid;
    const char *expected_sig;       /* NULL = skip sig check               */
} kernel_entry_t;

int kernel_count(void);
const kernel_entry_t *kernel_at(int index);
const kernel_entry_t *kernel_find_by_id(const char *id);
const kernel_entry_t *kernel_find_default(const char *target);
const kernel_entry_t *kernel_selected(void);

/* Returns NULL if id not found; otherwise new selected entry. */
const kernel_entry_t *kernel_select_by_id(const char *id);

/* Serial command helpers */
void kernel_registry_klist(void);   /* prints table to Serial */

#endif /* KERNEL_REGISTRY_H */
