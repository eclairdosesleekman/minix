/*
 * Regression test for CLICK2ABS() on amd64.
 *
 * phys_clicks is a 32-bit page number, but the resulting physical byte
 * address is 64-bit.  The conversion must widen before shifting.
 */
#include <stdio.h>

#include <minix/const.h>
#include <minix/type.h>

static const struct {
	phys_clicks clicks;
	phys_bytes expected;
} cases[] = {
	{ (phys_clicks)0,        (phys_bytes)0x000000000ULL },
	{ (phys_clicks)0x0fffff, (phys_bytes)0x00fffff000ULL },
	{ (phys_clicks)0x100000, (phys_bytes)0x0100000000ULL },
	{ (phys_clicks)0x100001, (phys_bytes)0x0100001000ULL },
	{ (phys_clicks)0xffffff, (phys_bytes)0x0ffffff000ULL },
	{ (phys_clicks)0x1000000,(phys_bytes)0x1000000000ULL }
};

int
main(void)
{
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		phys_bytes actual = CLICK2ABS(cases[i].clicks);

		if (actual != cases[i].expected) {
			printf("CLICK2ABS(%lu) = 0x%lx, expected 0x%lx\n",
			    (unsigned long)cases[i].clicks, actual,
			    cases[i].expected);
			return 1;
		}
	}

	return 0;
}
