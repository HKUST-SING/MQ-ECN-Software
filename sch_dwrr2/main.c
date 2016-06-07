#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <net/netlink.h>
#include <linux/pkt_sched.h>
#include <net/sch_generic.h>
#include <net/pkt_sched.h>
#include <linux/ip.h>
#include <net/dsfield.h>
#include <net/inet_ecn.h>

#include "params.h"

struct dwrr_rate_cfg
{
	u64	rate_bps;
	u32	mult;
	u32	shift;
};

/**
 *	struct dwrr_class - a Class of Service (CoS) queue
 *	@qdisc: FIFO queue to store sk_buff
 *  	@alist: active linked list
 *
 *	@id: queue ID
 *	@deficit: deficit counter of this queue (bytes)
 *	@len_bytes: queue length in bytes
 *  	@start_time: time when this queue is inserted to active list
 *	@last_pkt_time: time when this queue transmits the last packet
 *	@quantum: quantum in bytes of this queue

 */
struct dwrr_class
{
	struct Qdisc		*qdisc;
	struct list_head	alist;

	int	id;
	u32	deficit;
	u32	len_bytes;
	s64	start_time;
	s64	last_pkt_time;
	u32	quantum;
};

/**
 *	struct dwrr_sched_data - DWRR scheduler
 *	@queues: multiple Class of Service (CoS) queues
 *	@rate: shaping rate
 *	@active: linked list to store active queues
 *	@watchdog: watchdog timer for token bucket rate limiter
 *
 *	@tokens: tokens in ns
 *	@sum_len_bytes: the total buffer occupancy (in bytes) of the switch port
 *	@time_ns: time check-point
 *	@round_time: estimation of round time in ns
 *	@last_idle_time: last time when the port is idle
 */
struct dwrr_sched_data
{
	struct dwrr_class	*queues;
	struct dwrr_rate_cfg	rate;
	struct list_head	active;
	struct qdisc_watchdog	watchdog;

	s64	tokens;
	u32	sum_len_bytes;
	s64	time_ns;
	s64	round_time;
	s64	last_idle_time;
};

/* Exponential Weighted Moving Average (EWMA) for s64 */
static inline s64 s64_ewma(s64 smooth, s64 sample, int weight, int shift)
{
	s64 val = smooth * weight;
	val += sample * ((1 << shift) - weight);
	return val >> shift;
}

/*
 * We use this function to account for the true number of bytes sent on wire.
 * 20 = frame check sequence(8B)+Interpacket gap(12B)
 * 4 = Frame check sequence (4B)
 * dwrr_min_pkt_bytes = Minimum Ethernet frame size (64B)
 */
static inline unsigned int skb_size(struct sk_buff *skb)
{
	return max_t(unsigned int, skb->len + 4, dwrr_min_pkt_bytes) + 20;
}

/* Borrow from ptb */
static inline void precompute_ratedata(struct dwrr_rate_cfg *r)
{
	r->shift = 0;
	r->mult = 1;

	if (r->rate_bps > 0)
	{
		r->shift = 15;
		r->mult = div64_u64(8LLU * NSEC_PER_SEC * (1 << r->shift),
				    r->rate_bps);
	}
}

/* Borrow from ptb: length (bytes) to time (nanosecond) */
static inline u64 l2t_ns(struct dwrr_rate_cfg *r, unsigned int len_bytes)
{
	return ((u64)len_bytes * r->mult) >> r->shift;
}

/* MQ-ECN ECN marking */
void dwrr_mq_ecn_marking(struct sk_buff *skb,
		      	 struct dwrr_sched_data *q,
		      	 struct dwrr_class *cl)
{
	u64 ecn_thresh_bytes, estimate_rate_bps;

	if (q->round_time > 0)
		estimate_rate_bps = div_u64((u64)cl->quantum << 33,
					    q->round_time);
	else
		estimate_rate_bps = q->rate.rate_bps;

	/* rate <= link capacity */
	estimate_rate_bps = min_t(u64, estimate_rate_bps, q->rate.rate_bps);
	ecn_thresh_bytes = div_u64(estimate_rate_bps * dwrr_port_thresh_bytes,
				   q->rate.rate_bps);

	if (cl->len_bytes > ecn_thresh_bytes)
		INET_ECN_set_ce(skb);

	if (dwrr_enable_debug == dwrr_enable)
		printk(KERN_INFO "queue %d quantum %u ECN threshold %llu\n",
	       	       cl->id,
		       cl->quantum,
		       ecn_thresh_bytes);
}

/* ECN marking: per-queue, per-port and MQ-ECN */
void dwrr_ecn_marking(struct sk_buff *skb,
		      struct dwrr_sched_data *q,
		      struct dwrr_class *cl)
{
	switch (dwrr_ecn_scheme)
	{
		/* Per-queue ECN marking */
		case dwrr_queue_ecn:
		{
			if (cl->len_bytes > dwrr_queue_thresh_bytes[cl->id])
				INET_ECN_set_ce(skb);
			break;
		}
		/* Per-port ECN marking */
		case dwrr_port_ecn:
		{
			if (q->sum_len_bytes > dwrr_port_thresh_bytes)
				INET_ECN_set_ce(skb);
			break;
		}
		/* MQ-ECN */
		case dwrr_mq_ecn:
		{
			dwrr_mq_ecn_marking(skb, q, cl);
			break;
		}
		default:
		{
			break;
		}
	}
}

static struct dwrr_class *dwrr_classify(struct sk_buff *skb, struct Qdisc *sch)
{
	struct dwrr_sched_data *q = qdisc_priv(sch);
	struct iphdr* iph = ip_hdr(skb);
	int i, dscp;

	if (unlikely(!(q->queues)))
		return NULL;

	/* Return queue[0] by default*/
	if (unlikely(!iph))
		return &(q->queues[0]);

	dscp = iph->tos >> 2;

	for (i = 0; i < dwrr_max_queues; i++)
	{
		if (dscp == dwrr_queue_dscp[i])
			return &(q->queues[i]);
	}

	return &(q->queues[0]);
}

/* We don't need this */
static struct sk_buff *dwrr_peek(struct Qdisc *sch)
{
	return NULL;
}

/* Choose the packet to schedule according to DWRR algorithm */
/*static struct sk_buff *dwrr_schedule(struct dwrr_sched_data *q,
				     struct dwrr_class *cl)
{
	struct sk_buff *skb = NULL;
	s64 sample;

	while (true)
	{
		cl = list_first_entry(&q->active,
				      struct dwrr_class,
				      alist);


		if (cl->qdisc->q.qlen == 0)
		{
			sample = cl->last_pkt_time - cl->start_time;
			q->round_time = s64_ewma(q->round_time,
						    sample,
						    dwrr_round_alpha,
						    dwrr_round_alpha_shift);
			list_del(&cl->alist);
			continue;
		}

		skb = cl->qdisc->ops->peek(cl->qdisc);
		if (unlikely(!skb))
			goto out;

		if (skb_size(skb) <= cl->deficit)
		{
			return skb;
		}
		else
		{
			sample = cl->last_pkt_time - cl->start_time;
			q->round_time = s64_ewma(q->round_time,
						    sample,
						    dwrr_round_alpha,
						    dwrr_round_alpha_shift);

			cl->start_time = cl->last_pkt_time;
			cl->quantum = dwrr_queue_quantum[cl->id];

			if (dwrr_enable_wrr == dwrr_enable)
				cl->deficit = cl->quantum;
			else
				cl->deficit += cl->quantum;

			list_move_tail(&cl->alist, &q->active);
		}
	}

out:
	return NULL;
}
*/

/* Decide whether the packet can be transmitted according to Token Bucket */
static s64 tbf_schedule(unsigned int len, struct dwrr_sched_data *q, s64 now)
{
	s64 pkt_ns, toks;

	toks = now - q->time_ns;
	toks = min_t(s64, toks, (s64)l2t_ns(&q->rate, dwrr_bucket_bytes));
	toks += q->tokens;

	pkt_ns = (s64)l2t_ns(&q->rate, len);

	return toks - pkt_ns;
}

static inline void print_round_time(s64 sample, s64 smooth)
{
	/* Print necessary information in debug mode */
	if (dwrr_enable_debug == dwrr_enable && dwrr_ecn_scheme == dwrr_mq_ecn)
	{
		printk(KERN_INFO "sample round time %llu\n", sample);
		printk(KERN_INFO "smooth round time %llu\n", smooth);
	}
}

static struct sk_buff *dwrr_dequeue(struct Qdisc *sch)
{
	struct dwrr_sched_data *q = qdisc_priv(sch);
	struct dwrr_class *cl = NULL;
	struct sk_buff *skb = NULL;
	s64 sample, result;
	s64 now = ktime_get_ns();
	s64 bucket_ns = (s64)l2t_ns(&q->rate, dwrr_bucket_bytes);
	unsigned int len;

	/* No active queue */
	if (list_empty(&q->active))
		return NULL;

	while (1)
	{
		cl = list_first_entry(&q->active, struct dwrr_class, alist);
		if (unlikely(!cl))
			return NULL;

		/* get head packet */
		skb = cl->qdisc->ops->peek(cl->qdisc);
		if (unlikely(!skb))
			return NULL;

		len = skb_size(skb);
		if (unlikely(len > dwrr_max_pkt_bytes))
			printk(KERN_INFO "Error: pkt length %u > MTU\n", len);

		/* If this packet can be scheduled by DWRR */
		if (len <= cl->deficit)
		{
			result = tbf_schedule(len, q, now);
			/* If we don't have enough tokens */
			if (result < 0)
			{
				/* For hrtimer absolute mode, we use now + t */
				qdisc_watchdog_schedule_ns(&q->watchdog,
							   now - result,
							   true);
				qdisc_qstats_overlimit(sch);
				return NULL;
			}

			skb = qdisc_dequeue_peeked(cl->qdisc);
			if (unlikely(!skb))
				return NULL;

			q->sum_len_bytes -= len;
			sch->q.qlen--;
			cl->len_bytes -= len;
			cl->deficit -= len;
			cl->last_pkt_time = now + l2t_ns(&q->rate, len);

			if (cl->qdisc->q.qlen == 0)
			{
				list_del(&cl->alist);
				sample = cl->last_pkt_time - cl->start_time,
				q->round_time = s64_ewma(q->round_time,
							sample, dwrr_round_alpha, dwrr_round_alpha_shift);

				/* Get start time of idle period */
				if (q->sum_len_bytes == 0)
					q->last_idle_time = now;

				print_round_time(sample, q->round_time);
			}

			/* Bucket */
			q->time_ns = now;
			q->tokens = min_t(s64, result, bucket_ns);
			qdisc_unthrottled(sch);
			qdisc_bstats_update(sch, skb);

			if (dwrr_enable_dequeue_ecn == dwrr_enable)
				dwrr_ecn_marking(skb, q, cl);

			return skb;
		}
		/* This packet can not be scheduled by DWRR */
		else
		{
			sample = cl->last_pkt_time - cl->start_time;
			q->round_time = s64_ewma(q->round_time,
						 sample,
						 dwrr_round_alpha, dwrr_round_alpha_shift);
			cl->start_time = cl->last_pkt_time;
			cl->quantum = dwrr_queue_quantum[cl->id];
			list_move_tail(&cl->alist, &q->active);

			/* WRR */
			if (dwrr_enable_wrr == dwrr_enable)
				cl->deficit = cl->quantum;
			/* DWRR */
			else
				cl->deficit += cl->quantum;

			print_round_time(sample, q->round_time);
		}
	}

	return NULL;
}

static bool dwrr_buffer_overfill(unsigned int len,
				 struct dwrr_class *cl,
				 struct dwrr_sched_data *q)
{
	/* per-port shared buffer */
	if (dwrr_buffer_mode == dwrr_shared_buffer &&
	    q->sum_len_bytes + len > dwrr_shared_buffer_bytes)
		return true;
	/* per-queue static buffer */
	else if (dwrr_buffer_mode == dwrr_static_buffer &&
		 cl->len_bytes + len > dwrr_queue_buffer_bytes[cl->id])
		return true;
	else
		return false;
}

static int dwrr_enqueue(struct sk_buff *skb, struct Qdisc *sch)
{
	struct dwrr_class *cl = NULL;
	unsigned int len = skb_size(skb);
	struct dwrr_sched_data *q = qdisc_priv(sch);
	s64 interval, interval_num = 0;
	int i, ret;

	if (q->sum_len_bytes == 0 &&
	    dwrr_ecn_scheme == dwrr_mq_ecn &&
     	    dwrr_idle_interval_ns > 0)
	{
		interval = ktime_get_ns() - q->last_idle_time;
		interval_num = div_s64(interval, dwrr_idle_interval_ns);
	}

	if (interval_num > 0 && interval_num <= dwrr_max_iteration)
	{
		for (i = 0; i < interval_num; i++)
			q->round_time = s64_ewma(q->round_time,
						 0,
						 dwrr_round_alpha, dwrr_round_alpha_shift);
	}
	else if (interval_num > dwrr_max_iteration)
	{
		q->round_time = 0;
	}

	cl = dwrr_classify(skb,sch);
	/* No appropriate queue or the switch buffer is overfilled */
	if (unlikely(!cl) || dwrr_buffer_overfill(len, cl, q))
	{
		qdisc_qstats_drop(sch);
		qdisc_qstats_drop(cl->qdisc);
		kfree_skb(skb);
		return NET_XMIT_DROP;
	}

	ret = qdisc_enqueue(skb, cl->qdisc);
	if (unlikely(ret != NET_XMIT_SUCCESS))
	{
		if (likely(net_xmit_drop_count(ret)))
		{
			qdisc_qstats_drop(sch);
			qdisc_qstats_drop(cl->qdisc);
		}
		return ret;
	}

	/* Update queue sizes */
	sch->q.qlen++;
	q->sum_len_bytes += len;
	cl->len_bytes += len;

	/* If the queue is empty, insert it to the linked list */
	if (cl->qdisc->q.qlen == 1)
	{
		cl->start_time = ktime_get_ns();
		cl->quantum = dwrr_queue_quantum[cl->id];
		cl->deficit = cl->quantum;
		list_add_tail(&(cl->alist), &(q->active));
	}

	dwrr_ecn_marking(skb, q, cl);
	return ret;
}

/* We don't need this */
static unsigned int dwrr_drop(struct Qdisc *sch)
{
	return 0;
}

/* We don't need this */
static int dwrr_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	return 0;
}

/* Release Qdisc resources */
static void dwrr_destroy(struct Qdisc *sch)
{
	struct dwrr_sched_data *q = qdisc_priv(sch);
	int i;

	if (likely(q->queues))
	{
		for (i = 0; i < dwrr_max_queues && (q->queues[i]).qdisc; i++)
			qdisc_destroy((q->queues[i]).qdisc);

		kfree(q->queues);
	}
	qdisc_watchdog_cancel(&q->watchdog);
}

static const struct nla_policy dwrr_policy[TCA_TBF_MAX + 1] = {
	[TCA_TBF_PARMS] = { .len = sizeof(struct tc_tbf_qopt) },
	[TCA_TBF_RTAB]	= { .type = NLA_BINARY, .len = TC_RTAB_SIZE },
	[TCA_TBF_PTAB]	= { .type = NLA_BINARY, .len = TC_RTAB_SIZE },
};

/* We only leverage TC netlink interface to configure rate */
static int dwrr_change(struct Qdisc *sch, struct nlattr *opt)
{
	int err;
	struct dwrr_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_TBF_PTAB + 1];
	struct tc_tbf_qopt *qopt;
	__u32 rate;

	err = nla_parse_nested(tb, TCA_TBF_PTAB, opt, dwrr_policy);
	if(err < 0)
		return err;

	err = -EINVAL;
	if (!tb[TCA_TBF_PARMS])
		goto done;

	qopt = nla_data(tb[TCA_TBF_PARMS]);
	rate = qopt->rate.rate;
	/* convert from bytes/s to b/s */
	q->rate.rate_bps = (u64)rate << 3;
	precompute_ratedata(&q->rate);
	err = 0;
	printk(KERN_INFO "sch_dwrr: rate %llu Mbps\n",q->rate.rate_bps/1000000);

 done:
	return err;
}

/* Initialize Qdisc */
static int dwrr_init(struct Qdisc *sch, struct nlattr *opt)
{
	int i;
	struct dwrr_sched_data *q = qdisc_priv(sch);
	struct Qdisc *child;

	if(sch->parent != TC_H_ROOT)
		return -EOPNOTSUPP;

	q->queues = kcalloc(dwrr_max_queues,
			    sizeof(struct dwrr_class),
			    GFP_KERNEL);
	if (unlikely(!(q->queues)))
		return -ENOMEM;

	q->tokens = 0;
	q->time_ns = ktime_get_ns();
	q->last_idle_time = ktime_get_ns();
	q->sum_len_bytes = 0;
	q->round_time = 0;
	qdisc_watchdog_init(&q->watchdog, sch);
	INIT_LIST_HEAD(&(q->active));

	for (i = 0;i < dwrr_max_queues; i++)
	{
		/* bfifo is in bytes */
		child = fifo_create_dflt(sch,
					&bfifo_qdisc_ops, dwrr_max_buffer_bytes);
		if (likely(child))
			(q->queues[i]).qdisc = child;
		else
			goto err;

		/* Initialize variables for dwrr_class */
		INIT_LIST_HEAD(&((q->queues[i]).alist));
		(q->queues[i]).id = i;
		(q->queues[i]).deficit = 0;
		(q->queues[i]).len_bytes = 0;
		(q->queues[i]).start_time = ktime_get_ns();
		(q->queues[i]).last_pkt_time = ktime_get_ns();
		(q->queues[i]).quantum = 0;
	}
	return dwrr_change(sch,opt);
err:
	dwrr_destroy(sch);
	return -ENOMEM;
}

static struct Qdisc_ops dwrr_ops __read_mostly = {
	.next		=	NULL,
	.cl_ops		=	NULL,
	.id		=	"tbf",
	.priv_size	=	sizeof(struct dwrr_sched_data),
	.init		=	dwrr_init,
	.destroy	=	dwrr_destroy,
	.enqueue	=	dwrr_enqueue,
	.dequeue	=	dwrr_dequeue,
	.peek		=	dwrr_peek,
	.drop		=	dwrr_drop,
	.change		=	dwrr_change,
	.dump		=	dwrr_dump,
	.owner = THIS_MODULE,
};

static int __init dwrr_module_init(void)
{
	if (unlikely(!dwrr_params_init()))
		return -1;

	printk(KERN_INFO "sch_dwrr: start working\n");
	return register_qdisc(&dwrr_ops);
}

static void __exit dwrr_module_exit(void)
{
	dwrr_params_exit();
	unregister_qdisc(&dwrr_ops);
	printk(KERN_INFO "sch_dwrr: stop working\n");
}

module_init(dwrr_module_init);
module_exit(dwrr_module_exit);
MODULE_LICENSE("GPL");
