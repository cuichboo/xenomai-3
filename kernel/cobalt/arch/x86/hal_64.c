/**
 *   @ingroup hal
 *   @file
 *
 *   Adeos-based Real-Time Abstraction Layer for x86_64.
 *   Derived from the Xenomai/i386 HAL.
 *
 *   Copyright (C) 2007 Philippe Gerum.
 *
 *   Xenomai is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License as
 *   published by the Free Software Foundation, Inc., 675 Mass Ave,
 *   Cambridge MA 02139, USA; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   Xenomai is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *   02111-1307, USA.
 *
 *   x86_64-specific HAL services.
 */
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/console.h>
#include <linux/bitops.h>
#include <asm/system.h>
#include <asm/hardirq.h>
#include <asm/desc.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/unistd.h>
#include <asm/xenomai/hal.h>
#include <stdarg.h>

unsigned long rthal_timer_calibrate(void)
{
	unsigned long v, flags;
	rthal_time_t t, dt;
	int i;

	flags = rthal_critical_enter(NULL);

	t = rthal_rdtsc();

	for (i = 0; i < 20; i++) {
		v = apic_read(APIC_TMICT);
		apic_write(APIC_TMICT, v);
	}

	dt = (rthal_rdtsc() - t) / 2;

	rthal_critical_exit(flags);

	/*
	 * Reset the max trace, since it contains the calibration time
	 * now.
	 */
	ipipe_trace_max_reset();

	return rthal_imuldiv(dt, 20, RTHAL_CLOCK_FREQ);
}

int rthal_arch_init(void)
{
	/* FIXME: 4Ghz barrier is close... */
	if (rthal_clockfreq_arg == 0)
		rthal_clockfreq_arg = rthal_get_clockfreq();

	return 0;
}

void rthal_arch_cleanup(void)
{
	printk(KERN_INFO "Xenomai: hal/x86_64 stopped.\n");
}

EXPORT_SYMBOL_GPL(rthal_arch_init);
EXPORT_SYMBOL_GPL(rthal_arch_cleanup);
