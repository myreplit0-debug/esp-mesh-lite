#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

void uart_bridge_init(void);
/* write raw bytes (no newline added) */
void uart_bridge_write(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
