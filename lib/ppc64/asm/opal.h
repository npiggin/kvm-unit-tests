/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _ASMPPC64_OPAL_H_
#define _ASMPPC64_OPAL_H_

#define OPAL_SUCCESS				0

#define OPAL_CONSOLE_WRITE			1
#define OPAL_CONSOLE_READ			2
#define OPAL_CEC_POWER_DOWN			5
#define OPAL_POLL_EVENTS			10
#define OPAL_REINIT_CPUS			70
# define OPAL_REINIT_CPUS_HILE_BE		(1 << 0)
# define OPAL_REINIT_CPUS_HILE_LE		(1 << 1)

#endif
