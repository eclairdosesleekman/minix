/*
 * Regression test for the VM memory-map capacity.
 *
 * The kernel can publish up to MAXMEMMAP available ranges after splitting
 * ranges around reserved boot modules. VM must not silently truncate them.
 */
#include <minix/const.h>
#include <minix/param.h>

#if NR_MEMS < MAXMEMMAP
#error "NR_MEMS is smaller than MAXMEMMAP"
#endif

int
main(void)
{
	return 0;
}
