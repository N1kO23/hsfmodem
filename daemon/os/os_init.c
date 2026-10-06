/* SPDX-License-Identifier: MIT */
/*
 * OS layer start-up and shutdown.
 */
#include "hsf_os.h"

static bool initialised;

int hsf_os_init(const struct hsf_os_config *cfg)
{
	struct hsf_os_config def = HSF_OS_CONFIG_DEFAULT;
	int err;

	if (initialised)
		return 0;
	if (!cfg)
		cfg = &def;
	hsf_trace_level = cfg->trace;
	hsf_time_init();
	hsf_threads_configure_rt(cfg->realtime, cfg->rt_priority);
	err = hsf_physmem_init(cfg->bios_source);
	if (err)
		return err;
	if (hsf_timers_init()) {
		hsf_physmem_shutdown();
		return -1;
	}
	initialised = true;
	hsf_log(HSF_LOG_DEBUG, "OS layer ready: TSC %u MHz", hsf_cpu_mhz());
	return 0;
}

void hsf_os_shutdown(void)
{
	if (!initialised)
		return;
	hsf_timers_shutdown();
	hsf_threads_shutdown();
	hsf_physmem_shutdown();
	initialised = false;
}
