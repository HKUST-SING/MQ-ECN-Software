#include "params.h"
#include <linux/sysctl.h>
#include <linux/string.h>


/* Enable debug mode or not. By default, we disable debug mode. */
int dwrr_enable_debug = dwrr_disable;
/*
 * Buffer management mode: shared (0) or static (1).
 * By default, we enable shread buffer.
 */
int dwrr_buffer_mode = dwrr_shared_buffer;
/* Per port shared buffer (bytes) */
int dwrr_shared_buffer_bytes = dwrr_max_buffer_bytes;
/* Bucket size in bytes. By default, we use 2.5KB for 1G network. */
int dwrr_bucket_bytes = 2500;
/*
 * Per port ECN marking threshold (bytes).
 * By default, we use 32KB for 1G network.
 */
int dwrr_port_thresh_bytes = 32000;
/* ECN marking scheme. By default, we perform per queue ECN/RED marking. */
int dwrr_ecn_scheme = dwrr_queue_ecn;
/* Alpha for round time estimation. It is 0.75 by default. */
int dwrr_round_alpha = (3 << dwrr_round_alpha_shift) / 4;
/* Idle time slot. It is 12us by default */
int dwrr_idle_interval_ns = 12000;
/* By default, we disable WRR */
int dwrr_enable_wrr = dwrr_disable;
/* By default, we perform enqueue ECN marking. */
int dwrr_enable_dequeue_ecn = dwrr_disable;

int dwrr_enable_min = dwrr_disable;
int dwrr_enable_max = dwrr_enable;
int dwrr_buffer_mode_min = dwrr_shared_buffer;
int dwrr_buffer_mode_max = dwrr_static_buffer;
int dwrr_ecn_scheme_min = dwrr_disable_ecn;
int dwrr_ecn_scheme_max = dwrr_mq_ecn;
int dwrr_round_alpha_min = 0;
int dwrr_round_alpha_max = 1 << dwrr_round_alpha_shift;
int dwrr_dscp_min = 0;
int dwrr_dscp_max = (1 << 6) - 1;
int dwrr_quantum_min = dwrr_max_pkt_bytes;
int dwrr_quantum_max = 200 << 10;

/* Per queue ECN marking threshold (bytes) */
int dwrr_queue_thresh_bytes[dwrr_max_queues];
/* DSCP value for different queues*/
int dwrr_queue_dscp[dwrr_max_queues];
/* Quantum for different queues*/
int dwrr_queue_quantum[dwrr_max_queues];
/* Per queue minimum guarantee buffer (bytes) */
int dwrr_queue_buffer_bytes[dwrr_max_queues];

/*
 * All parameters that can be configured through sysctl.
 * We have dwrr_global_params + 4 * dwrr_max_queues parameters in total.
 */
struct dwrr_param dwrr_params[dwrr_total_params + 1] =
{
	/* Global parameters */
	{"enable_debug",	&dwrr_enable_debug},
	{"buffer_mode",		&dwrr_buffer_mode},
	{"shared_buffer",	&dwrr_shared_buffer_bytes},
	{"bucket", 		&dwrr_bucket_bytes},
	{"port_thresh", 	&dwrr_port_thresh_bytes},
	{"ecn_scheme", 		&dwrr_ecn_scheme},
	{"round_alpha", 	&dwrr_round_alpha},
	{"idle_interval_ns",	&dwrr_idle_interval_ns},
	{"enable_wrr",		&dwrr_enable_wrr},
	{"enable_dequeue_ecn",	&dwrr_enable_dequeue_ecn},
};

struct ctl_table dwrr_params_table[dwrr_total_params + 1];

struct ctl_path dwrr_params_path[] =
{
	{ .procname = "dwrr" },
	{ },
};

struct ctl_table_header *dwrr_sysctl = NULL;

bool dwrr_params_init(void)
{
	int i, index;
	memset(dwrr_params_table, 0, sizeof(dwrr_params_table));

	for (i = 0; i < dwrr_max_queues; i++)
	{
		/* Per queue ECN marking threshold*/
		index = dwrr_global_params + i;
		snprintf(dwrr_params[index].name, 63, "queue_thresh_%d", i);
		dwrr_params[index].ptr = &dwrr_queue_thresh_bytes[i];
		dwrr_queue_thresh_bytes[i] = dwrr_port_thresh_bytes;

		/* Per-queue DSCP */
		index = dwrr_global_params + i + dwrr_max_queues;
		snprintf(dwrr_params[index].name, 63, "queue_dscp_%d", i);
		dwrr_params[index].ptr = &dwrr_queue_dscp[i];
		dwrr_queue_dscp[i] = i;

		/* Per-queue Quantum */
		index = dwrr_global_params + i + 2 * dwrr_max_queues;
		snprintf(dwrr_params[index].name, 63, "queue_quantum_%d", i);
		dwrr_params[index].ptr = &dwrr_queue_quantum[i];
		dwrr_queue_quantum[i] = dwrr_max_pkt_bytes;

		/* Per-queue buffer size */
		index = dwrr_global_params + i + 3 * dwrr_max_queues;
		snprintf(dwrr_params[index].name, 63, "queue_buffer_%d", i);
		dwrr_params[index].ptr = &dwrr_queue_buffer_bytes[i];
		dwrr_queue_buffer_bytes[i] = dwrr_max_buffer_bytes;
	}

	/* End of the parameters */
	dwrr_params[dwrr_global_params + 4 * dwrr_max_queues].ptr = NULL;

	for (i = 0; i < dwrr_global_params + 4 * dwrr_max_queues; i++)
	{
		struct ctl_table *entry = &dwrr_params_table[i];

		/* Initialize entry (ctl_table) */
		entry->procname = dwrr_params[i].name;
		entry->data = dwrr_params[i].ptr;
		entry->mode = 0644;

		/* enable_debug, enable_wrr and enable_dequeue_ecn */
		if (i == 0 || i == 8 || i == 9)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &dwrr_enable_min;
			entry->extra2 = &dwrr_enable_max;
		}
		/* buffer_mode */
		else if (i == 1)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &dwrr_buffer_mode_min;
			entry->extra2 = &dwrr_buffer_mode_max;
		}
		/* ecn_scheme */
		else if (i == 5)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &dwrr_ecn_scheme_min;
			entry->extra2 = &dwrr_ecn_scheme_max;
		}
		/* round_alpha */
		else if (i == 6)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &dwrr_round_alpha_min;
			entry->extra2 = &dwrr_round_alpha_max;
		}
		/* Per-queue DSCP */
		else if (i >= dwrr_global_params + dwrr_max_queues &&
			 i < dwrr_global_params + 2 * dwrr_max_queues)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &dwrr_dscp_min;
			entry->extra2 = &dwrr_dscp_max;
		}
		/* Per-queue quantum */
		else if (i >= dwrr_global_params + 2 * dwrr_max_queues &&
			 i < dwrr_global_params + 3 * dwrr_max_queues)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &dwrr_quantum_min;
			entry->extra2 = &dwrr_quantum_max;
		}
		else
		{
			entry->proc_handler = &proc_dointvec;
		}
		entry->maxlen=sizeof(int);
	}

	dwrr_sysctl = register_sysctl_paths(dwrr_params_path,
					    dwrr_params_table);

	if (likely(dwrr_sysctl))
		return true;
	else
		return false;

}

void dwrr_params_exit()
{
	if (likely(dwrr_sysctl))
		unregister_sysctl_table(dwrr_sysctl);
}
