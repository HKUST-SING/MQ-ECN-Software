#ifndef __PARAMS_H__
#define __PARAMS_H__

#include <linux/types.h>

/* Our module has at most 8 queues */
#define dwrr_max_queues 8
/*
 * 1538 = MTU (1500B) + Ethernet header(14B) + Frame check sequence (4B) +
 * Frame check sequence(8B) + Interpacket gap(12B)
 */
#define dwrr_max_pkt_bytes 1538
/*
 * Ethernet packets with less than the minimum 64 bytes
 * (header (14B) + user data + FCS (4B)) are padded to 64 bytes.
 */
#define dwrr_min_pkt_bytes 64
/* Maximum (per queue/per port shared) buffer size (2MB) */
#define dwrr_max_buffer_bytes 2000000
/* Per port shared buffer management policy */
#define	dwrr_shared_buffer 0
/* Per port static buffer management policy */
#define	dwrr_static_buffer 1

/* Disable ECN marking */
#define	dwrr_disable_ecn 0
/* Per queue ECN marking */
#define	dwrr_queue_ecn 1
/* Per port ECN marking */
#define dwrr_port_ecn 2
/* MQ-ECN */
#define dwrr_mq_ecn 3

#define dwrr_max_iteration 10

#define dwrr_round_alpha_shift 10

/* The number of global (rather than 'per-queue') parameters */
#define dwrr_global_params 10
/* The total number of parameters (per-queue and global parameters) */
#define dwrr_total_params (dwrr_global_params + 4 * dwrr_max_queues)

#define dwrr_disable 0
#define dwrr_enable 1

/* Global parameters */
/* Enable debug mode or not */
extern int dwrr_enable_debug;
/* Buffer management mode: shared (0) or static (1)*/
extern int dwrr_buffer_mode;
/* Per port shared buffer (bytes) */
extern int dwrr_shared_buffer_bytes;
/* Bucket size in bytes*/
extern int dwrr_bucket_bytes;
/* Per port ECN marking threshold (bytes) */
extern int dwrr_port_thresh_bytes;
/* ECN marking scheme */
extern int dwrr_ecn_scheme;
/* Alpha for round time estimation */
extern int dwrr_round_alpha;
/* Idle time interval */
extern int dwrr_idle_interval_ns;
/* Enable WRR or not */
extern int dwrr_enable_wrr;
/* Enable dequeue ECN marking or not */
extern int dwrr_enable_dequeue_ecn;

/* Per-queue parameters */
/* Per queue ECN marking threshold (bytes) */
extern int dwrr_queue_thresh_bytes[dwrr_max_queues];
/* DSCP value for different queues */
extern int dwrr_queue_dscp[dwrr_max_queues];
/* Quantum for different queues*/
extern int dwrr_queue_quantum[dwrr_max_queues];
/* Per queue static reserved buffer (bytes) */
extern int dwrr_queue_buffer_bytes[dwrr_max_queues];

struct dwrr_param
{
	char name[64];
	int *ptr;
};

extern struct dwrr_param dwrr_params[dwrr_total_params + 1];

/* Intialize parameters and register sysctl */
bool dwrr_params_init(void);
/* Unregister sysctl */
void dwrr_params_exit(void);

#endif
