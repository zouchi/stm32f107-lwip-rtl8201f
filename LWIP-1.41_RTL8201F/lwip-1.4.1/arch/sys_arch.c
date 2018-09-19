#include "lwip/debug.h"
#include "lwip/def.h"
#include "lwip/sys.h"
#include "lwip/mem.h"
#include "lwip/mem.h"
#include "arch/sys_arch.h"

extern u32_t LWipTime;
u32_t sys_now(void)
{
	return LWipTime;
}




