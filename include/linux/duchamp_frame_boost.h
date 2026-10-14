/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_DUCHAMP_FRAME_BOOST_H
#define _LINUX_DUCHAMP_FRAME_BOOST_H
#include <linux/kconfig.h>
#include <linux/cpumask.h>
#if IS_REACHABLE(CONFIG_OPLUS_FEATURE_FRAME_BOOST)
unsigned long duchamp_fbg_sugov_util(const struct cpumask *cpus,
		unsigned long util, unsigned long capacity);
#else
static inline unsigned long duchamp_fbg_sugov_util(const struct cpumask *cpus,
		unsigned long util, unsigned long capacity) { return util; }
#endif
#endif
