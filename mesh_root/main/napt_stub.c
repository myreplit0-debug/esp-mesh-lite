#include "sdkconfig.h"

/* If lwIP NAPT is not built, provide a no-op stub so link succeeds. */
#if !CONFIG_LWIP_NAPT
void ip_napt_table_clear(void) { /* no-op when NAT is not used */ }
#endif
