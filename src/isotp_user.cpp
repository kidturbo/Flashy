/*
 * isotp_user.cpp — Implements the three callbacks required by isotp-c.
 *
 * These are declared in isotp_user.h (plain C prototypes) and called
 * from isotp.c, so we wrap them with extern "C".
 */

#include <Arduino.h>
#include <stdarg.h>
#include "can_driver.h"

extern "C" {

#include "isotp_user.h"

void isotp_user_debug(const char *message, ...)
{
#ifdef ISOTP_DEBUG
    char buf[128];
    va_list args;
    va_start(args, message);
    vsnprintf(buf, sizeof(buf), message, args);
    va_end(args);
    Serial.print("[ISO-TP] ");
    Serial.println(buf);
#else
    (void)message;
#endif
}

int isotp_user_send_can(const uint32_t arbitration_id,
                        const uint8_t *data, const uint8_t size)
{
    return can_send(arbitration_id, data, size);
}

uint32_t isotp_user_get_ms(void)
{
    return (uint32_t)millis();
}

} /* extern "C" */
