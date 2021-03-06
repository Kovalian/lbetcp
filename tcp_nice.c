/*
 * TCP Nice congestion control
 *
 * Based on the congestion detection/avoidance scheme described in
 *    Arun Venkataramani, Ravi Kokku and Mike Dahlin.
 *    "TCP Nice: A Mechanism for Background Transfers."
 *    ACM SIGOPS Operating Systems Review, 36(SI):329-343,
 *    2002. Available from:
 *	https://www.researchgate.net/profile/Ravi_Kokku/publication/
 *  2543144_TCP_Nice_A_Mechanism_for_Background_Transfers/links/
 *  00b7d52b0fb54c6fe4000000.pdf
 *
 * Based on TCP Vegas implementation by Stephen Hemminger.
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/inet_diag.h>

#include <net/tcp.h>

static int alpha = 1;
static int beta  = 3;
static int gamma = 1;

static int fraction = 50;
static int threshold = 20;

static int fraction_divisor = 0;

static int max_fwnd = 96;

module_param(alpha, int, 0644);
MODULE_PARM_DESC(alpha, "lower bound of packets in network");
module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "upper bound of packets in network");
module_param(gamma, int, 0644);
MODULE_PARM_DESC(gamma, "limit on increase (scale by 2)");
module_param(fraction, int, 0644);
MODULE_PARM_DESC(fraction, "fraction of cwnd to experience congestion before multiplicative decrease");
module_param(threshold, int, 0644);
MODULE_PARM_DESC(threshold, "delay threshold for congestion detector");
module_param(max_fwnd, int, 0644);
MODULE_PARM_DESC(max_fwnd, "highest permitted value of fractional_cwnd");

/* Nice variables */
struct nice {
	u32	beg_snd_nxt;	/* right edge during last RTT */
	u32	beg_snd_una;	/* left edge  during last RTT */
	u32	beg_snd_cwnd;	/* saves the size of the cwnd */
	u8	doing_nice_now;/* if true, do nice for this RTT */
	u16	cntRTT;		/* # of RTTs measured within last RTT */
	u32	minRTT;		/* min of RTTs measured within last RTT (in usec) */
	u32 maxRTT;		/* max of RTTs measured within last RTT (in usec) */
	u32	baseRTT;	/* the min of all nice RTT measurements seen (in usec) */
	u8  numCong;	/* number of congestion events detected by nice */
	u8	fractional_cwnd; /* denominator of the cwnd */
	u8	nice_timer;	/* keeps time for the fractional cwnd */
};

/* There are several situations when we must "re-start" Vegas:
 *
 *  o when a connection is established
 *  o after an RTO
 *  o after fast recovery
 *  o when we send a packet and there is no outstanding
 *    unacknowledged data (restarting an idle connection)
 *
 * In these circumstances we cannot do a Vegas calculation at the
 * end of the first RTT, because any calculation we do is using
 * stale info -- both the saved cwnd and congestion feedback are
 * stale.
 *
 * Instead we must wait until the completion of an RTT during
 * which we actually receive ACKs.
 */
static void nice_enable(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct nice *nice = inet_csk_ca(sk);

	/* Begin taking Vegas samples next time we send something. */
	nice->doing_nice_now = 1;

	/* Set the beginning of the next send window. */
	nice->beg_snd_nxt = tp->snd_nxt;

	nice->cntRTT = 0;
	nice->minRTT = 0x7fffffff;
}

/* Stop taking Vegas samples for now. */
static inline void nice_disable(struct sock *sk)
{
	struct nice *nice = inet_csk_ca(sk);

	nice->doing_nice_now = 0;
}

void tcp_nice_init(struct sock *sk)
{
	struct nice *nice = inet_csk_ca(sk);

	fraction_divisor = 100 / fraction; 

	/* Initialise the CWND denominator */
	nice->fractional_cwnd = 2; 
	nice->nice_timer = 0;

	nice->baseRTT = 0x7fffffff;
	nice_enable(sk);
}
EXPORT_SYMBOL_GPL(tcp_nice_init);

/* Do RTT sampling needed for Vegas.
 * Basically we:
 *   o min-filter RTT samples from within an RTT to get the current
 *     propagation delay + queuing delay (we are min-filtering to try to
 *     avoid the effects of delayed ACKs)
 *   o min-filter RTT samples from a much longer window (forever for now)
 *     to find the propagation delay (baseRTT)
 */
void tcp_nice_pkts_acked(struct sock *sk, u32 cnt, s32 rtt_us)
{
	struct nice *nice = inet_csk_ca(sk);
	u32 vrtt;

	if (rtt_us < 0)
		return;

	/* Never allow zero rtt or baseRTT */
	vrtt = rtt_us + 1;

	/* Filter to find propagation delay: */
	if (vrtt < nice->baseRTT)
		nice->baseRTT = vrtt;

	/* Initialise maxRTT to 2*minRTT */	
	if (nice->cntRTT == 0)
		nice->maxRTT = nice->baseRTT * 2;

	/* Find the min RTT during the last RTT to find
	 * the current prop. delay + queuing delay:
	 */
	nice->minRTT = min(nice->minRTT, vrtt);
	nice->maxRTT = max(nice->maxRTT, vrtt);
	nice->cntRTT++;

	if (vrtt > ((100UL - threshold) * nice->baseRTT + threshold * 
			nice->maxRTT) / 100UL) {
		nice->numCong++;
	}
}
EXPORT_SYMBOL_GPL(tcp_nice_pkts_acked);

void tcp_nice_state(struct sock *sk, u8 ca_state)
{
	if (ca_state == TCP_CA_Open)
		nice_enable(sk);
	else
		nice_disable(sk);
}
EXPORT_SYMBOL_GPL(tcp_nice_state);

/*
 * If the connection is idle and we are restarting,
 * then we don't want to do any Vegas calculations
 * until we get fresh RTT samples.  So when we
 * restart, we reset our Vegas state to a clean
 * slate. After we get acks for this flight of
 * packets, _then_ we can make Vegas calculations
 * again.
 */
void tcp_nice_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	if (event == CA_EVENT_CWND_RESTART ||
	    event == CA_EVENT_TX_START)
		tcp_nice_init(sk);
}
EXPORT_SYMBOL_GPL(tcp_nice_cwnd_event);

static inline u32 tcp_nice_ssthresh(struct tcp_sock *tp)
{
	return  min(tp->snd_ssthresh, tp->snd_cwnd-1);
}

static void tcp_reno_fractional_ca(struct sock *sk, u32 ack, u32 acked)
{
/* Determine what change Reno would apply and use it on the fractional CWND */
	struct tcp_sock *tp = tcp_sk(sk);
	struct nice *nice = inet_csk_ca(sk);
		
	int16_t cwnd_change;
	u32 cur_cwnd = tp->snd_cwnd;
	u32 cur_cwnd_cnt = tp->snd_cwnd_cnt;

	tcp_reno_cong_avoid(sk, ack, acked);

	cwnd_change = 2 * (tp->snd_cwnd - cur_cwnd);

	if (cwnd_change != 0) {
			nice->fractional_cwnd -= cwnd_change;
	}

	/* Restore previous CWND and let Nice continue */
	if (nice->fractional_cwnd > 2) {
		tp->snd_cwnd = cur_cwnd;
		tp->snd_cwnd_cnt = cur_cwnd_cnt;
	}
	else {
		nice->fractional_cwnd = 2;
	}
}

static void tcp_nice_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct nice *nice = inet_csk_ca(sk);

	if (nice->fractional_cwnd > 2 && nice->nice_timer == nice->fractional_cwnd) {
		/* Send two packets in this RTT then reset the timer */
		tp->snd_cwnd = 2;
		nice->nice_timer = 1;
	} else if (nice->fractional_cwnd > 2) {
		/* Waiting to send packets */
		tp->snd_cwnd = 0;
		nice->nice_timer++;
	}

	if (!nice->doing_nice_now) {
		if (tp->snd_cwnd <= 2 && nice->fractional_cwnd >= 2 && nice->fractional_cwnd
				<= max_fwnd) {
			tcp_reno_fractional_ca(sk, ack, acked);
		} else {
			/* Just do Reno */
			tcp_reno_cong_avoid(sk, ack, acked);
		}
		return;
	}

	if (after(ack, nice->beg_snd_nxt)) {
		/* Do the Vegas once-per-RTT cwnd adjustment. */

		/* Save the extent of the current window so we can use this
		 * at the end of the next RTT.
		 */
		nice->beg_snd_nxt  = tp->snd_nxt;

		/* We do the Vegas calculations only if we got enough RTT
		 * samples that we can be reasonably sure that we got
		 * at least one RTT sample that wasn't from a delayed ACK.
		 * If we only had 2 samples total,
		 * then that means we're getting only 1 ACK per RTT, which
		 * means they're almost certainly delayed ACKs.
		 * If  we have 3 samples, we should be OK.
		 */

		if (nice->cntRTT <= 2) {
			/* We don't have enough RTT samples to do the Vegas
			 * calculation, so we'll behave like Reno.
			 */
 			if (tp->snd_cwnd <= 2 && nice->fractional_cwnd >= 2 && nice->fractional_cwnd
 					<= max_fwnd) {
 				tcp_reno_fractional_ca(sk, ack, acked);
 			} else {
 				/* Just do Reno */
 				tcp_reno_cong_avoid(sk, ack, acked);
 			}
		} else {
			u32 rtt, diff;
			u64 target_cwnd;

			/* We have enough RTT samples, so, using the Vegas
			 * algorithm, we determine if we should increase or
			 * decrease cwnd, and by how much.
			 */

			/* Pluck out the RTT we are using for the Vegas
			 * calculations. This is the min RTT seen during the
			 * last RTT. Taking the min filters out the effects
			 * of delayed ACKs, at the cost of noticing congestion
			 * a bit later.
			 */
			rtt = nice->minRTT;

			/* Calculate the cwnd we should have, if we weren't
			 * going too fast.
			 *
			 * This is:
			 *     (actual rate in segments) * baseRTT
			 */
			target_cwnd = (u64)tp->snd_cwnd * nice->baseRTT;
			do_div(target_cwnd, rtt);

			/* Calculate the difference between the window we had,
			 * and the window we would like to have. This quantity
			 * is the "Diff" from the Arizona Vegas papers.
			 */
			diff = tp->snd_cwnd * (rtt-nice->baseRTT) / nice->baseRTT;

			if (diff > gamma && tcp_in_slow_start(tp)) {
				/* Going too fast. Time to slow down
				 * and switch to congestion avoidance.
				 */

				/* Set cwnd to match the actual rate
				 * exactly:
				 *   cwnd = (actual rate) * baseRTT
				 * Then we add 1 because the integer
				 * truncation robs us of full link
				 * utilization.
				 */
				tp->snd_cwnd = min(tp->snd_cwnd, (u32)target_cwnd+1);
				tp->snd_ssthresh = tcp_nice_ssthresh(tp);
				nice->numCong = 0;

			} else if (tcp_in_slow_start(tp)) {
				/* Slow start.  */
				tcp_slow_start(tp, acked);
			} else if (nice->numCong > tp->snd_cwnd / fraction_divisor) {
				/* Nice detected too many congestion events
				 * perform multiplicative window reduction.
				 */
				if (tp->snd_cwnd > 2 && nice->fractional_cwnd == 2) {
					tp->snd_cwnd = tp->snd_cwnd / 2;
				} else if (nice->fractional_cwnd <= max_fwnd) {
					nice->fractional_cwnd *= 4; 
				}
				
				nice->numCong = 0; // Reset multiplicative decrease counter.
		    } else {
				/* Congestion avoidance. */

				/* Figure out where we would like cwnd
				 * to be.
				 */
				if (diff > beta) {
					/* The old window was too fast, so
					 * we slow down.
					 */
					if (tp->snd_cwnd > 2 && nice->fractional_cwnd == 2) {
						tp->snd_cwnd--;
					} else if (nice->fractional_cwnd <= max_fwnd) {
						nice->fractional_cwnd+=2;
					}

					tp->snd_ssthresh
						= tcp_nice_ssthresh(tp);
				} else if (diff < alpha) {
					/* We don't have enough extra packets
					 * in the network, so speed up.
					 */
					if (tp->snd_cwnd >= 2 && nice->fractional_cwnd == 2) {
						tp->snd_cwnd++;
					} else if (nice->fractional_cwnd <= max_fwnd) {
						nice->fractional_cwnd-=2;
					}
				} else {
					/* Sending just as fast as we
					 * should be.
					 */
				}
			}

			if (tp->snd_cwnd < 2 && nice->fractional_cwnd == 2)
				tp->snd_cwnd = 2;
			else if (tp->snd_cwnd > tp->snd_cwnd_clamp)
				tp->snd_cwnd = tp->snd_cwnd_clamp;

			tp->snd_ssthresh = tcp_current_ssthresh(sk);
		}

		/* Wipe the slate clean for the next RTT. */
		nice->cntRTT = 0;
		nice->minRTT = 0x7fffffff;
		nice->maxRTT = 0;
		nice->numCong = 0;
	}
	/* Use normal slow start */
	else if (tcp_in_slow_start(tp))
		tcp_slow_start(tp, acked);
}

/* Extract info for Tcp socket info provided via netlink. */
size_t tcp_nice_get_info(struct sock *sk, u32 ext, int *attr,
			  union tcp_cc_info *info)
{
	const struct nice *ca = inet_csk_ca(sk);

	if (ext & (1 << (INET_DIAG_VEGASINFO - 1))) {
		info->vegas.tcpv_enabled = ca->doing_nice_now,
		info->vegas.tcpv_rttcnt = ca->cntRTT,
		info->vegas.tcpv_rtt = ca->baseRTT,
		info->vegas.tcpv_minrtt = ca->minRTT,

		*attr = INET_DIAG_VEGASINFO;
		return sizeof(struct tcpvegas_info);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(tcp_nice_get_info);

static struct tcp_congestion_ops tcp_nice __read_mostly = {
	.init		= tcp_nice_init,
	.ssthresh	= tcp_reno_ssthresh,
	.cong_avoid	= tcp_nice_cong_avoid,
	.pkts_acked	= tcp_nice_pkts_acked,
	.set_state	= tcp_nice_state,
	.cwnd_event	= tcp_nice_cwnd_event,
	.get_info	= tcp_nice_get_info,

	.owner		= THIS_MODULE,
	.name		= "nice",
};

static int __init tcp_nice_register(void)
{
	BUILD_BUG_ON(sizeof(struct nice) > ICSK_CA_PRIV_SIZE);
	tcp_register_congestion_control(&tcp_nice);
	return 0;
}

static void __exit tcp_nice_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_nice);
}

module_init(tcp_nice_register);
module_exit(tcp_nice_unregister);

MODULE_AUTHOR("Kevin Ong, Stephen Hemminger");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP Nice");
