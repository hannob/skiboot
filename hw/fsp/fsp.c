/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Service Processor handling code
 *
 * XXX This mixes PSI and FSP and currently only supports
 * P7/P7+ PSI and FSP1
 *
 * If we are going to support P8 PSI and FSP2, we probably want
 * to split the PSI support from the FSP support proper first.
 */
#include <stdarg.h>
#include <processor.h>
#include <io.h>
#include <fsp.h>
#include <lock.h>
#include <interrupts.h>
#include <gx.h>
#include <device.h>
#include <trace.h>
#include <timebase.h>
#include <cpu.h>
#include <fsp-elog.h>
#include <opal.h>
#include <opal-msg.h>

DEFINE_LOG_ENTRY(OPAL_RC_FSP_POLL_TIMEOUT, OPAL_PLATFORM_ERR_EVT, OPAL_FSP,
		 OPAL_PLATFORM_FIRMWARE, OPAL_ERROR_PANIC, OPAL_NA, NULL);

#define FSP_TRACE_MSG
#define FSP_TRACE_EVENT

#define FSP_MAX_IOPATH	4

enum fsp_path_state {
	fsp_path_bad,
	fsp_path_backup,
	fsp_path_active,
};

struct fsp_iopath {
	enum fsp_path_state	state;
	void			*fsp_regs;
	struct psi		*psi;
};

enum fsp_mbx_state {
	fsp_mbx_idle,		/* Mailbox ready to send */
	fsp_mbx_send,		/* Mailbox sent, waiting for ack */
	fsp_mbx_crit_op,	/* Critical operation in progress */
	fsp_mbx_prep_for_reset,	/* Prepare for reset sent */
	fsp_mbx_hir_seq_done,	/* HIR sequence done, link forced down */
	fsp_mbx_err,		/* Mailbox in error state, waiting for r&r */
	fsp_mbx_rr,		/* Mailbox in r&r */
};

struct fsp {
	struct fsp		*link;
	unsigned int		index;
	enum fsp_mbx_state	state;
	struct fsp_msg		*pending;

	unsigned int		iopath_count;
	int			active_iopath;	/* -1: no active IO path */
	struct fsp_iopath	iopath[FSP_MAX_IOPATH];
};

static struct fsp *first_fsp;
static struct fsp *active_fsp;
static u16 fsp_curseq = 0x8000;
static u64 *fsp_tce_table;

#define FSP_INBOUND_SIZE	0x00100000UL
static void *fsp_inbound_buf = NULL;
static u32 fsp_inbound_off;

static struct lock fsp_lock = LOCK_UNLOCKED;

static u64 fsp_cmdclass_resp_bitmask;
static u64 timeout_timer;

static u64 fsp_hir_timeout;

#define FSP_CRITICAL_OP_TIMEOUT		128
#define FSP_DRCR_CLEAR_TIMEOUT		128

/* LID numbers. For now we hijack some of pHyp's own until i figure
 * out the whole business with the MasterLID
 */
#define KERNEL_LID_PHYP			0x80a00701
#define KERNEL_LID_OPAL			0x80f00101
#define INITRAMFS_LID_OPAL		0x80f00102

/*
 * We keep track on last logged values for some things to print only on
 * value changes, but also to releive pressure on the tracer which
 * doesn't do a very good job at detecting repeats when called from
 * many different CPUs
 */
static u32 disr_last_print;
static u32 drcr_last_print;
static u32 hstate_last_print;

void fsp_handle_resp(struct fsp_msg *msg);

struct fsp_cmdclass {
	int timeout;
	bool busy;
	struct list_head msgq;
	struct list_head clientq;
	struct list_head rr_queue;	/* To queue up msgs during R/R */
	u64 timesent;
};

static struct fsp_cmdclass fsp_cmdclass_rr;

static struct fsp_cmdclass fsp_cmdclass[FSP_MCLASS_LAST - FSP_MCLASS_FIRST + 1]
= {
#define DEF_CLASS(_cl, _to) [_cl - FSP_MCLASS_FIRST] = { .timeout = _to }
	DEF_CLASS(FSP_MCLASS_SERVICE,		16),
	DEF_CLASS(FSP_MCLASS_PCTRL_MSG,		16),
	DEF_CLASS(FSP_MCLASS_PCTRL_ABORTS,	16),
	DEF_CLASS(FSP_MCLASS_ERR_LOG,		16),
	DEF_CLASS(FSP_MCLASS_CODE_UPDATE,	40),
	DEF_CLASS(FSP_MCLASS_FETCH_SPDATA,	16),
	DEF_CLASS(FSP_MCLASS_FETCH_HVDATA,	16),
	DEF_CLASS(FSP_MCLASS_NVRAM,		16),
	DEF_CLASS(FSP_MCLASS_MBOX_SURV,		 2),
	DEF_CLASS(FSP_MCLASS_RTC,		16),
	DEF_CLASS(FSP_MCLASS_SMART_CHIP,	20),
	DEF_CLASS(FSP_MCLASS_INDICATOR,	       180),
	DEF_CLASS(FSP_MCLASS_HMC_INTFMSG,	16),
	DEF_CLASS(FSP_MCLASS_HMC_VT,		16),
	DEF_CLASS(FSP_MCLASS_HMC_BUFFERS,	16),
	DEF_CLASS(FSP_MCLASS_SHARK,		16),
	DEF_CLASS(FSP_MCLASS_MEMORY_ERR,	16),
	DEF_CLASS(FSP_MCLASS_CUOD_EVENT,	16),
	DEF_CLASS(FSP_MCLASS_HW_MAINT,		16),
	DEF_CLASS(FSP_MCLASS_VIO,		16),
	DEF_CLASS(FSP_MCLASS_SRC_MSG,		16),
	DEF_CLASS(FSP_MCLASS_DATA_COPY,		16),
	DEF_CLASS(FSP_MCLASS_TONE,		16),
	DEF_CLASS(FSP_MCLASS_VIRTUAL_NVRAM,	16),
	DEF_CLASS(FSP_MCLASS_TORRENT,		16),
	DEF_CLASS(FSP_MCLASS_NODE_PDOWN,	16),
	DEF_CLASS(FSP_MCLASS_DIAG,		16),
	DEF_CLASS(FSP_MCLASS_PCIE_LINK_TOPO,	16),
	DEF_CLASS(FSP_MCLASS_OCC,		16),
};

static void fsp_trace_msg(struct fsp_msg *msg, u8 dir __unused)
{
	union trace fsp __unused;
#ifdef FSP_TRACE_MSG
	size_t len = offsetof(struct trace_fsp_msg, data[msg->dlen]);

	fsp.fsp_msg.dlen = msg->dlen;
	fsp.fsp_msg.word0 = msg->word0;
	fsp.fsp_msg.word1 = msg->word1;
	fsp.fsp_msg.dir = dir;
	memcpy(fsp.fsp_msg.data, msg->data.bytes, msg->dlen);
	trace_add(&fsp, TRACE_FSP_MSG, len);
#endif /* FSP_TRACE_MSG */
	assert(msg->dlen <= sizeof(fsp.fsp_msg.data));
}

static struct fsp *fsp_get_active(void)
{
	/* XXX Handle transition between FSPs */
	return active_fsp;
}

static u64 fsp_get_class_bit(u8 class)
{
	/* Alias classes CE and CF as the FSP has a single queue */
	if (class == FSP_MCLASS_IPL)
		class = FSP_MCLASS_SERVICE;

	return 1ul << (class - FSP_MCLASS_FIRST);
}

static struct fsp_cmdclass *__fsp_get_cmdclass(u8 class)
{
	struct fsp_cmdclass *ret;

	/* RR class is special */
	if (class == FSP_MCLASS_RR_EVENT)
		return &fsp_cmdclass_rr;

	/* Bound check */
	if (class < FSP_MCLASS_FIRST || class > FSP_MCLASS_LAST)
		return NULL;

	/* Alias classes CE and CF as the FSP has a single queue */
	if (class == FSP_MCLASS_IPL)
		class = FSP_MCLASS_SERVICE;

	ret = &fsp_cmdclass[class - FSP_MCLASS_FIRST];

	/* Unknown class */
	if (ret->timeout == 0)
		return NULL;

	return ret;
}

static struct fsp_cmdclass *fsp_get_cmdclass(struct fsp_msg *msg)
{
	u8 c = msg->word0 & 0xff;

	return __fsp_get_cmdclass(c);
}

static struct fsp_msg *__fsp_allocmsg(void)
{
	return zalloc(sizeof(struct fsp_msg));
}

struct fsp_msg *fsp_allocmsg(bool alloc_response)
{
	struct fsp_msg *msg;

	msg = __fsp_allocmsg();
	if (!msg)
		return NULL;
	if (alloc_response)
		msg->resp = __fsp_allocmsg();
	return msg;
}

void __fsp_freemsg(struct fsp_msg *msg)
{
	free(msg);
}

void fsp_freemsg(struct fsp_msg *msg)
{
	if (msg->resp)
		__fsp_freemsg(msg->resp);
	__fsp_freemsg(msg);
}

void fsp_cancelmsg(struct fsp_msg *msg)
{
	bool need_unlock = false;
	struct fsp_cmdclass* cmdclass = fsp_get_cmdclass(msg);
	struct fsp *fsp = fsp_get_active();

	if (fsp->state != fsp_mbx_rr) {
		prerror("FSP: Message cancel allowed only when"
						"FSP is in reset\n");
		return;
	}

	if (!cmdclass)
		return;

	/* Recursive locking */
	need_unlock = lock_recursive(&fsp_lock);

	list_del(&msg->link);
	msg->state = fsp_msg_cancelled;

	if (need_unlock)
		unlock(&fsp_lock);
}

static void fsp_wreg(struct fsp *fsp, u32 reg, u32 val)
{
	struct fsp_iopath *iop;

	if (fsp->active_iopath < 0)
		return;
	iop = &fsp->iopath[fsp->active_iopath];
	if (iop->state == fsp_path_bad)
		return;
	out_be32(iop->fsp_regs + reg, val);
}

static u32 fsp_rreg(struct fsp *fsp, u32 reg)
{
	struct fsp_iopath *iop;

	if (fsp->active_iopath < 0)
		return 0xffffffff;
	iop = &fsp->iopath[fsp->active_iopath];
	if (iop->state == fsp_path_bad)
		return 0xffffffff;
	return in_be32(iop->fsp_regs + reg);
}

static void fsp_reg_dump(void)
{
#define FSP_DUMP_ONE(x)	\
	prlog(PR_DEBUG, "  %20s: %x\n", #x, fsp_rreg(fsp, x));

	struct fsp *fsp = fsp_get_active();

	if (!fsp)
		return;

	prlog(PR_DEBUG, "FSP #%d: Register dump (state=%d)\n",
	      fsp->index, fsp->state);
	FSP_DUMP_ONE(FSP_DRCR_REG);
	FSP_DUMP_ONE(FSP_DISR_REG);
	FSP_DUMP_ONE(FSP_MBX1_HCTL_REG);
	FSP_DUMP_ONE(FSP_MBX1_FCTL_REG);
	FSP_DUMP_ONE(FSP_MBX2_HCTL_REG);
	FSP_DUMP_ONE(FSP_MBX2_FCTL_REG);
	FSP_DUMP_ONE(FSP_SDES_REG);
	FSP_DUMP_ONE(FSP_HDES_REG);
	FSP_DUMP_ONE(FSP_HDIR_REG);
	FSP_DUMP_ONE(FSP_HDIM_SET_REG);
	FSP_DUMP_ONE(FSP_PDIR_REG);
	FSP_DUMP_ONE(FSP_PDIM_SET_REG);
	FSP_DUMP_ONE(FSP_SCRATCH0_REG);
	FSP_DUMP_ONE(FSP_SCRATCH1_REG);
	FSP_DUMP_ONE(FSP_SCRATCH2_REG);
	FSP_DUMP_ONE(FSP_SCRATCH3_REG);
}

static void fsp_notify_rr_state(u32 state)
{
	struct fsp_client *client, *next;
	struct fsp_cmdclass *cmdclass = __fsp_get_cmdclass(FSP_MCLASS_RR_EVENT);

	assert(cmdclass);
	list_for_each_safe(&cmdclass->clientq, client, next, link)
		client->message(state, NULL);
}

static void fsp_reset_cmdclass(void)
{
	int i;
	struct fsp_msg *msg;

	for (i = 0; i <= (FSP_MCLASS_LAST - FSP_MCLASS_FIRST); i++) {
		struct fsp_cmdclass *cmdclass = &fsp_cmdclass[i];
		cmdclass->busy = false;
		cmdclass->timesent = 0;

		/* We also need to reset the 'timeout' timers here */

		/* Make sure the message queue is empty */
		while(!list_empty(&cmdclass->msgq)) {
			msg = list_pop(&cmdclass->msgq, struct fsp_msg,
				       link);
			list_add_tail(&cmdclass->rr_queue, &msg->link);
		}
	}
}

static bool fsp_in_hir(struct fsp *fsp)
{
	switch (fsp->state) {
	case fsp_mbx_crit_op:
	case fsp_mbx_prep_for_reset:
		return true;
	default:
		return false;
	}
}

static bool fsp_in_reset(struct fsp *fsp)
{
	switch (fsp->state) {
	case fsp_mbx_hir_seq_done:	/* FSP reset triggered */
	case fsp_mbx_err:		/* Will be reset soon */
	case fsp_mbx_rr:		/* Mbx activity stopped pending reset */
		return true;
	default:
		return false;
	}
}

static bool fsp_hir_state_timeout(void)
{
	u64 now = mftb();

	if (tb_compare(now, fsp_hir_timeout) == TB_AAFTERB)
		return true;

	return false;
}

static void fsp_set_hir_timeout(u32 seconds)
{
	u64 now = mftb();
	fsp_hir_timeout = now + secs_to_tb(seconds);
}

static bool fsp_crit_op_in_progress(struct fsp *fsp)
{
	u32 disr = fsp_rreg(fsp, FSP_DISR_REG);

	if (disr & FSP_DISR_CRIT_OP_IN_PROGRESS)
		return true;

	return false;
}

/* Notify the FSP that it will be reset soon by writing to the DRCR */
static void fsp_prep_for_reset(struct fsp *fsp)
{
	u32 drcr = fsp_rreg(fsp, FSP_DRCR_REG);

	prlog(PR_TRACE, "FSP: Writing reset to DRCR\n");
	drcr_last_print = drcr;
	fsp_wreg(fsp, FSP_DRCR_REG, (drcr | FSP_PREP_FOR_RESET_CMD));
	fsp->state = fsp_mbx_prep_for_reset;
	fsp_set_hir_timeout(FSP_DRCR_CLEAR_TIMEOUT);
}

static void fsp_hir_poll(struct fsp *fsp, struct psi *psi)
{
	u32 drcr;

	switch (fsp->state) {
	case fsp_mbx_crit_op:
		if (fsp_crit_op_in_progress(fsp)) {
			if (fsp_hir_state_timeout())
				prerror("FSP: Critical operation timeout\n");
				/* XXX What do do next? Check with FSP folks */
		} else {
			fsp_prep_for_reset(fsp);
		}
		break;
	case fsp_mbx_prep_for_reset:
		drcr = fsp_rreg(fsp, FSP_DRCR_REG);

		if (drcr != drcr_last_print) {
			prlog(PR_TRACE, "FSP: DRCR changed, old = %x,"
			      " new = %x\n",
			      drcr_last_print, drcr);
			drcr_last_print = drcr;
		}

		if (drcr & FSP_DRCR_ACK_MASK) {
			if (fsp_hir_state_timeout()) {
				prerror("FSP: Ack timeout. Triggering reset\n");
				psi_reset_fsp(psi);
				fsp->state = fsp_mbx_hir_seq_done;
			}
		} else {
			prlog(PR_TRACE, "FSP: DRCR ack received."
			      " Triggering reset\n");
			psi_reset_fsp(psi);
			fsp->state = fsp_mbx_hir_seq_done;
		}
		break;
	default:
		break;
	}
}

/*
 * This is the main entry for the host initiated reset case.
 * This gets called when:
 *	a. Surveillance ack is not received in 120 seconds
 *	b. A mailbox command doesn't get a response within the stipulated time.
 */
static void __fsp_trigger_reset(void)
{
	struct fsp *fsp = fsp_get_active();
	u32 disr;

	/* Already in one of the error processing states */
	if (fsp_in_hir(fsp) || fsp_in_reset(fsp))
		return;

	prerror("FSP: fsp_trigger_reset() entry\n");

	drcr_last_print = 0;
	/*
	 * Check if we are allowed to reset the FSP. We aren't allowed to
	 * reset the FSP if the FSP_DISR_DBG_IN_PROGRESS is set.
	 */
	disr = fsp_rreg(fsp, FSP_DISR_REG);
	if (disr & FSP_DISR_DBG_IN_PROGRESS) {
		prerror("FSP: Host initiated reset disabled\n");
		return;
	}

	/*
	 * Check if some critical operation is in progress as indicated
	 * by FSP_DISR_CRIT_OP_IN_PROGRESS. Timeout is 128 seconds
	 */
	if (fsp_crit_op_in_progress(fsp)) {
		prlog(PR_NOTICE, "FSP: Critical operation in progress\n");
		fsp->state = fsp_mbx_crit_op;
		fsp_set_hir_timeout(FSP_CRITICAL_OP_TIMEOUT);
	} else
		fsp_prep_for_reset(fsp);
}

void fsp_trigger_reset(void)
{
	lock(&fsp_lock);
	__fsp_trigger_reset();
	unlock(&fsp_lock);
}

/*
 * Called when we trigger a HIR or when the FSP tells us via the DISR's
 * RR bit that one is impending. We should therefore stop all mbox activity.
 */
static void fsp_start_rr(struct fsp *fsp)
{
	struct fsp_iopath *iop;

	if (fsp->state == fsp_mbx_rr)
		return;

	/* We no longer have an active path on that FSP */
	if (fsp->active_iopath >= 0) {
		iop = &fsp->iopath[fsp->active_iopath];
		iop->state = fsp_path_bad;
		fsp->active_iopath = -1;
	}
	fsp->state = fsp_mbx_rr;
	disr_last_print = 0;
	hstate_last_print = 0;

	/*
	 * Mark all command classes as non-busy and clear their
	 * timeout, then flush all messages in our staging queue
	 */
	fsp_reset_cmdclass();

	/* Notify clients. We have to drop the lock here */
	unlock(&fsp_lock);
	fsp_notify_rr_state(FSP_RESET_START);
	lock(&fsp_lock);

	/*
	 * Unlike earlier, we don't trigger the PSI link polling
	 * from this point. We wait for the PSI interrupt to tell
	 * us the FSP is really down and then start the polling there.
	 */
}

static void fsp_trace_event(struct fsp *fsp, u32 evt,
			    u32 data0, u32 data1, u32 data2, u32 data3)
{
	union trace tfsp __unused;
#ifdef FSP_TRACE_EVENT
	size_t len = sizeof(struct trace_fsp_event);

	tfsp.fsp_evt.event = evt;
	tfsp.fsp_evt.fsp_state = fsp->state;
	tfsp.fsp_evt.data[0] = data0;
	tfsp.fsp_evt.data[1] = data1;
	tfsp.fsp_evt.data[2] = data2;
	tfsp.fsp_evt.data[3] = data3;
	trace_add(&tfsp, TRACE_FSP_EVENT, len);
#endif /* FSP_TRACE_EVENT */
}

static void fsp_handle_errors(struct fsp *fsp)
{
	u32 hstate;
	struct fsp_iopath *iop;
	struct psi *psi;
	u32 disr;

	if (fsp->active_iopath < 0) {
		prerror("FSP #%d: fsp_handle_errors() with no active IOP\n",
			fsp->index);
		return;
	}

	iop = &fsp->iopath[fsp->active_iopath];
	if (!iop->psi) {
		prerror("FSP: Active IOP with no PSI link !\n");
		return;
	}
	psi = iop->psi;

	/*
	 * If the link is not up, start R&R immediately, we do call
	 * psi_disable_link() in this case as while the link might
	 * not be up, it might still be enabled and the PSI layer
	 * "active" bit still set
	 */
	if (!psi_check_link_active(psi)) {
		/* Start R&R process */
		fsp_trace_event(fsp, TRACE_FSP_EVT_LINK_DOWN, 0, 0, 0, 0);
		prerror("FSP #%d: Link down, starting R&R\n", fsp->index);

		fsp_start_rr(fsp);
		return;
	}

	/* Link is up, check for other conditions */
	disr = fsp_rreg(fsp, FSP_DISR_REG);

	/* If in R&R, log values */
	if (disr != disr_last_print) {
		fsp_trace_event(fsp, TRACE_FSP_EVT_DISR_CHG, disr, 0, 0, 0);

		prlog(PR_TRACE, "FSP #%d: DISR stat change = 0x%08x\n",
		      fsp->index, disr);
		disr_last_print = disr;
	}

	/* On a deferred mbox error, trigger a HIR
	 * Note: We may never get here since the link inactive case is handled
	 * above and the other case is when the iop->psi is NULL, which is
	 * quite rare.
	 */
	if (fsp->state == fsp_mbx_err) {
		prerror("FSP #%d: Triggering HIR on mbx_err\n",
				fsp->index);
		fsp_trigger_reset();
		return;
	}

	/*
	 * If we get here as part of normal flow, the FSP is telling
	 * us that there will be an impending R&R, so we stop all mbox
	 * activity. The actual link down trigger is via a PSI
	 * interrupt that may arrive in due course.
	 */
	if (disr & FSP_DISR_FSP_IN_RR) {
		/*
		 * If we get here with DEBUG_IN_PROGRESS also set, the
		 * FSP is in debug and we should *not* reset it now
		 */
		if (disr & FSP_DISR_DBG_IN_PROGRESS)
			return;

		/*
		 * When the linux comes back up, we still see that bit
		 * set for a bit, so just move on, nothing to see here
		 */
		if (fsp->state == fsp_mbx_rr)
			return;

		prlog(PR_NOTICE, "FSP #%d: FSP in Reset."
		      " Waiting for PSI interrupt\n",
		      fsp->index);
		fsp_start_rr(fsp);
	}

	/*
	 * However, if any of Unit Check or Runtime Termintated or
	 * Flash Terminated bits is also set, the FSP is asking us
	 * to trigger a HIR so it can try to recover via the DRCR route.
	 */
	if (disr & FSP_DISR_HIR_TRIGGER_MASK) {
		fsp_trace_event(fsp, TRACE_FSP_EVT_SOFT_RR, disr, 0, 0, 0);

		if (disr & FSP_DISR_FSP_UNIT_CHECK)
			prlog(PR_DEBUG, "FSP: DISR Unit Check set\n");
		else if (disr & FSP_DISR_FSP_RUNTIME_TERM)
			prlog(PR_DEBUG, "FSP: DISR Runtime Terminate set\n");
		else if (disr & FSP_DISR_FSP_FLASH_TERM)
			prlog(PR_DEBUG, "FSP: DISR Flash Terminate set\n");
		prlog(PR_NOTICE, "FSP: Triggering host initiated reset"
		      " sequence\n");

		/* Clear all interrupt conditions */
		fsp_wreg(fsp, FSP_HDIR_REG, FSP_DBIRQ_ALL);

		/* Make sure this happened */
		fsp_rreg(fsp, FSP_HDIR_REG);

		fsp_trigger_reset();
		return;
	}

	/*
	 * We detect an R&R complete indication, acknolwedge it
	 */
	if (disr & FSP_DISR_FSP_RR_COMPLETE) {
		/*
		 * Acking this bit doens't make it go away immediately, so
		 * only do it while still in R&R state
		 */
		if (fsp->state == fsp_mbx_rr) {
			fsp_trace_event(fsp, TRACE_FSP_EVT_RR_COMPL, 0,0,0,0);

			prlog(PR_NOTICE, "FSP #%d: Detected R&R complete,"
			      " acking\n", fsp->index);

			/* Clear HDATA area */
			fsp_wreg(fsp, FSP_MBX1_HDATA_AREA, 0xff);

			/* Ack it (XDN) and clear HPEND & counts */
			fsp_wreg(fsp, FSP_MBX1_HCTL_REG,
				 FSP_MBX_CTL_PTS |
				 FSP_MBX_CTL_XDN |
				 FSP_MBX_CTL_HPEND |
				 FSP_MBX_CTL_HCSP_MASK |
				 FSP_MBX_CTL_DCSP_MASK);

			/*
			 * Mark the mbox as usable again so we can process
			 * incoming messages
			 */
			fsp->state = fsp_mbx_idle;

			/* Also clear R&R complete bit in DISR */
			fsp_wreg(fsp, FSP_DISR_REG, FSP_DISR_FSP_RR_COMPLETE);
		}
	}

	/*
	 * XXX
	 *
	 * Here we detect a number of errors, should we initiate
	 * and R&R ?
	 */

	hstate = fsp_rreg(fsp, FSP_HDES_REG);
	if (hstate != hstate_last_print) {
		fsp_trace_event(fsp, TRACE_FSP_EVT_HDES_CHG, hstate, 0, 0, 0);

		prlog(PR_DEBUG, "FSP #%d: HDES stat change = 0x%08x\n",
		      fsp->index, hstate);
		hstate_last_print = hstate;
	}

	if (hstate == 0xffffffff)
		return;

	/* Clear errors */
	fsp_wreg(fsp, FSP_HDES_REG, FSP_DBERRSTAT_CLR1);

	/*
	 * Most of those errors shouldn't have happened, we just clear
	 * the error state and return. In the long run, we might want
	 * to start retrying commands, switching FSPs or links, etc...
	 *
	 * We currently don't set our mailbox to a permanent error state.
	 */
	if (hstate & FSP_DBERRSTAT_ILLEGAL1)
		prerror("FSP #%d: Illegal command error !\n", fsp->index);

	if (hstate & FSP_DBERRSTAT_WFULL1)
		prerror("FSP #%d: Write to a full mbox !\n", fsp->index);

	if (hstate & FSP_DBERRSTAT_REMPTY1)
		prerror("FSP #%d: Read from an empty mbox !\n", fsp->index);

	if (hstate & FSP_DBERRSTAT_PAR1)
		prerror("FSP #%d: Parity error !\n", fsp->index);
}

/*
 * This is called by fsp_post_msg() to check if the mbox
 * is in a state that allows sending of a message
 *
 * Due to the various "interesting" contexts fsp_post_msg()
 * can be called from, including recursive locks from lock
 * error messages or console code, this should avoid doing
 * anything more complex than checking a bit of state.
 *
 * Specifically, we cannot initiate an R&R and call back into
 * clients etc... from this function.
 *
 * The best we can do is to se the mbox in error state and
 * handle it later during a poll or interrupts.
 */
static bool fsp_check_can_send(struct fsp *fsp)
{
	struct fsp_iopath *iop;
	struct psi *psi;

	/* Look for FSP in non-idle state */
	if (fsp->state != fsp_mbx_idle)
		return false;

	/* Look for an active IO path */
	if (fsp->active_iopath < 0)
		goto mbox_error;
	iop = &fsp->iopath[fsp->active_iopath];
	if (!iop->psi) {
		prerror("FSP: Active IOP with no PSI link !\n");
		goto mbox_error;
	}
	psi = iop->psi;

	/* Check if link has gone down. This will be handled later */
	if (!psi_check_link_active(psi)) {
		prerror("FSP #%d: Link seems to be down on send\n", fsp->index);
		goto mbox_error;
	}

	/* XXX Do we want to check for other error conditions ? */
	return true;

	/*
	 * An error of some case occurred, we'll handle it later
	 * from a more normal "poll" context
	 */
 mbox_error:
	fsp->state = fsp_mbx_err;
	return false;
}

static bool fsp_post_msg(struct fsp *fsp, struct fsp_msg *msg)
{
	u32 ctl, reg;
	int i, wlen;

	prlog(PR_INSANE, "FSP #%d: fsp_post_msg (w0: 0x%08x w1: 0x%08x)\n",
	    fsp->index, msg->word0, msg->word1);

	/* Note: We used to read HCTL here and only modify some of
	 * the bits in it. This was bogus, because we would write back
	 * the incoming bits as '1' and clear them, causing fsp_poll()
	 * to then miss them. Let's just start with 0, which is how
	 * I suppose the HW intends us to do.
	 */

	/* Set ourselves as busy */
	fsp->pending = msg;
	fsp->state = fsp_mbx_send;
	msg->state = fsp_msg_sent;

	/* We trace after setting the mailbox state so that if the
	 * tracing recurses, it ends up just queuing the message up
	 */
	fsp_trace_msg(msg, TRACE_FSP_MSG_OUT);

	/* Build the message in the mailbox */
	reg = FSP_MBX1_HDATA_AREA;
	fsp_wreg(fsp, reg, msg->word0); reg += 4;
	fsp_wreg(fsp, reg, msg->word1); reg += 4;
	wlen = (msg->dlen + 3) >> 2;
	for (i = 0; i < wlen; i++) {
		fsp_wreg(fsp, reg, msg->data.words[i]);
		reg += 4;
	}

	/* Write the header */
	fsp_wreg(fsp, FSP_MBX1_HHDR0_REG, (msg->dlen + 8) << 16);

	/* Write the control register */
	ctl = 4 << FSP_MBX_CTL_HCHOST_SHIFT;
	ctl |= (msg->dlen + 8) << FSP_MBX_CTL_DCHOST_SHIFT;
	ctl |= FSP_MBX_CTL_PTS | FSP_MBX_CTL_SPPEND;
	prlog(PR_INSANE, "    new ctl: %08x\n", ctl);
	fsp_wreg(fsp, FSP_MBX1_HCTL_REG, ctl);

	return true;
}

static void fsp_poke_queue(struct fsp_cmdclass *cmdclass)
{
	struct fsp *fsp = fsp_get_active();
	struct fsp_msg *msg;

	if (!fsp)
		return;
	if (!fsp_check_can_send(fsp))
		return;

	/* From here to the point where fsp_post_msg() sets fsp->state
	 * to !idle we must not cause any re-entrancy (no debug or trace)
	 * in a code path that may hit fsp_post_msg() (it's ok to do so
	 * if we are going to bail out), as we are committed to calling
	 * fsp_post_msg() and so a re-entrancy could cause us to do a
	 * double-send into the mailbox.
	 */
	if (cmdclass->busy || list_empty(&cmdclass->msgq))
		return;

	msg = list_top(&cmdclass->msgq, struct fsp_msg, link);
	assert(msg);
	cmdclass->busy = true;

	if (!fsp_post_msg(fsp, msg)) {
		prerror("FSP #%d: Failed to send message\n", fsp->index);
		cmdclass->busy = false;
		return;
	}
}

static void __fsp_fillmsg(struct fsp_msg *msg, u32 cmd_sub_mod,
			  u8 add_words, va_list list)
{
	bool response = !!(cmd_sub_mod & 0x1000000);
	u8 cmd = (cmd_sub_mod >> 16) & 0xff;
	u8 sub = (cmd_sub_mod >>  8) & 0xff;
	u8 mod =  cmd_sub_mod & 0xff;
	int i;

	msg->word0 = cmd & 0xff;
	msg->word1 = mod << 8 | sub;
	msg->response = response;
	msg->dlen = add_words << 2;

	for (i = 0; i < add_words; i++)
		msg->data.words[i] = va_arg(list, unsigned int);
	va_end(list);
}

extern void fsp_fillmsg(struct fsp_msg *msg, u32 cmd_sub_mod, u8 add_words, ...)
{
	va_list list;

	va_start(list, add_words);
	__fsp_fillmsg(msg, cmd_sub_mod, add_words, list);
	va_end(list);
}

struct fsp_msg *fsp_mkmsg(u32 cmd_sub_mod, u8 add_words, ...)
{
	struct fsp_msg *msg = fsp_allocmsg(!!(cmd_sub_mod & 0x1000000));
	va_list list;

	if (!msg) {
		prerror("FSP: Failed to allocate struct fsp_msg\n");
		return NULL;
	}

	va_start(list, add_words);
	__fsp_fillmsg(msg, cmd_sub_mod, add_words, list);
	va_end(list);

	return msg;
}

/*
 * IMPORTANT NOTE: This is *guaranteed* to not call the completion
 *                 routine recusrively for *any* fsp message, either the
 *                 queued one or a previous one. Thus it is *ok* to call
 *                 this function with a lock held which will itself be
 *                 taken by the completion function.
 *
 *                 Any change to this implementation must respect this
 *                 rule. This will be especially true of things like
 *                 reset/reload and error handling, if we fail to queue
 *                 we must just return an error, not call any completion
 *                 from the scope of fsp_queue_msg().
 */
int fsp_queue_msg(struct fsp_msg *msg, void (*comp)(struct fsp_msg *msg))
{
	struct fsp_cmdclass *cmdclass;
	struct fsp *fsp = fsp_get_active();
	bool need_unlock;
	u16 seq;
	int rc = 0;

	if (!fsp || !msg)
		return -1;

	/* Recursive locking */
	need_unlock = lock_recursive(&fsp_lock);

	/* Grab a new sequence number */
	seq = fsp_curseq;
	fsp_curseq = fsp_curseq + 1;
	if (fsp_curseq == 0)
		fsp_curseq = 0x8000;
	msg->word0 = (msg->word0 & 0xffff) | seq << 16;

	/* Set completion */
	msg->complete = comp;

	/* Clear response state */
	if (msg->resp)
		msg->resp->state = fsp_msg_unused;

	/* Queue the message in the appropriate queue */
	cmdclass = fsp_get_cmdclass(msg);
	if (!cmdclass) {
		prerror("FSP: Invalid msg in fsp_queue_msg w0/1=0x%08x/%08x\n",
			msg->word0, msg->word1);
		rc = -1;
		goto unlock;
	}

	msg->state = fsp_msg_queued;

	/*
	 * If we have initiated or about to initiate a reset/reload operation,
	 * we stash the message on the R&R backup queue. Otherwise, queue it
	 * normally and poke the HW
	 */
	if (fsp_in_hir(fsp) || fsp_in_reset(fsp))
		list_add_tail(&cmdclass->rr_queue, &msg->link);
	else {
		list_add_tail(&cmdclass->msgq, &msg->link);
		fsp_poke_queue(cmdclass);
	}

 unlock:
	if (need_unlock)
		unlock(&fsp_lock);

	return rc;
}

/* WARNING: This will drop the FSP lock !!! */
static void fsp_complete_msg(struct fsp_msg *msg)
{
	struct fsp_cmdclass *cmdclass = fsp_get_cmdclass(msg);
	void (*comp)(struct fsp_msg *msg);

	assert(cmdclass);

	prlog(PR_INSANE, "  completing msg,  word0: 0x%08x\n", msg->word0);

	comp = msg->complete;
	list_del_from(&cmdclass->msgq, &msg->link);
	cmdclass->busy = false;
	msg->state = fsp_msg_done;

	unlock(&fsp_lock);
	if (comp)
		(*comp)(msg);
	lock(&fsp_lock);
}

/* WARNING: This will drop the FSP lock !!! */
static void fsp_complete_send(struct fsp *fsp)
{
	struct fsp_msg *msg = fsp->pending;
	struct fsp_cmdclass *cmdclass = fsp_get_cmdclass(msg);

	assert(msg);
	assert(cmdclass);

	fsp->pending = NULL;

	prlog(PR_INSANE, "  completing send, word0: 0x%08x, resp: %d\n",
	    msg->word0, msg->response);

	if (msg->response) {
		u64 setbit = fsp_get_class_bit(msg->word0 & 0xff);
		msg->state = fsp_msg_wresp;
		fsp_cmdclass_resp_bitmask |= setbit;
		cmdclass->timesent = mftb();
	} else
		fsp_complete_msg(msg);
}

static void  fsp_alloc_inbound(struct fsp_msg *msg)
{
	u16 func_id = msg->data.words[0] & 0xffff;
	u32 len = msg->data.words[1];
	u32 tce_token = 0, act_len = 0;
	u8 rc = 0;
	void *buf;

	prlog(PR_DEBUG, "FSP: Allocate inbound buffer func: %04x len: %d\n",
	      func_id, len);

	lock(&fsp_lock);
	if ((fsp_inbound_off + len) > FSP_INBOUND_SIZE) {
		prerror("FSP: Out of space in buffer area !\n");
		rc = 0xeb;
		goto reply;
	}

	if (!fsp_inbound_buf) {
		fsp_inbound_buf = memalign(TCE_PSIZE, FSP_INBOUND_SIZE);
		if (!fsp_inbound_buf) {
			prerror("FSP: could not allocate fsp_inbound_buf!\n");
			rc = 0xeb;
			goto reply;
		}
	}

	buf = fsp_inbound_buf + fsp_inbound_off;
	tce_token = PSI_DMA_INBOUND_BUF + fsp_inbound_off;
	len = (len + 0xfff) & ~0xfff;
	fsp_inbound_off += len;
	fsp_tce_map(tce_token, buf, len);
	prlog(PR_DEBUG, "FSP:  -> buffer at 0x%p, TCE: 0x%08x, alen: 0x%x\n",
	      buf, tce_token, len);
	act_len = len;

 reply:
	unlock(&fsp_lock);
	fsp_queue_msg(fsp_mkmsg(FSP_RSP_ALLOC_INBOUND | rc,
				3, 0, tce_token, act_len), fsp_freemsg);
}

void *fsp_inbound_buf_from_tce(u32 tce_token)
{
	u32 offset = tce_token - PSI_DMA_INBOUND_BUF;

	if (tce_token < PSI_DMA_INBOUND_BUF || offset >= fsp_inbound_off) {
		prerror("FSP: TCE token 0x%x out of bounds\n", tce_token);
		return NULL;
	}
	return fsp_inbound_buf + offset;
}

static void fsp_repost_queued_msgs_post_rr(void)
{
	struct fsp_msg *msg;
	int i;

	for (i = 0; i <= (FSP_MCLASS_LAST - FSP_MCLASS_FIRST); i++) {
		struct fsp_cmdclass *cmdclass = &fsp_cmdclass[i];
		bool poke = false;

		while(!list_empty(&cmdclass->rr_queue)) {
			msg = list_pop(&cmdclass->rr_queue,
				       struct fsp_msg, link);
			list_add_tail(&cmdclass->msgq, &msg->link);
			poke = true;
		}
		if (poke)
			fsp_poke_queue(cmdclass);
	}
}

static bool fsp_local_command(u32 cmd_sub_mod, struct fsp_msg *msg)
{
	u32 cmd = 0;
	u32 rsp_data = 0;

	switch(cmd_sub_mod) {
	case FSP_CMD_CONTINUE_IPL:
		/* We get a CONTINUE_IPL as a response to OPL */
		prlog(PR_NOTICE, "FSP: Got CONTINUE_IPL !\n");
		ipl_state |= ipl_got_continue;
		return true;

	case FSP_CMD_HV_STATE_CHG:
		prlog(PR_NOTICE, "FSP: Got HV state change request to %d\n",
		      msg->data.bytes[0]);

		/* Send response synchronously for now, we might want to
		 * deal with that sort of stuff asynchronously if/when
		 * we add support for auto-freeing of messages
		 */
		fsp_queue_msg(fsp_mkmsg(FSP_RSP_HV_STATE_CHG, 0), fsp_freemsg);
		return true;

	case FSP_CMD_SP_NEW_ROLE:
		/* FSP is assuming a new role */
		prlog(PR_INFO, "FSP: FSP assuming new role\n");
		fsp_queue_msg(fsp_mkmsg(FSP_RSP_SP_NEW_ROLE, 0), fsp_freemsg);
		ipl_state |= ipl_got_new_role;
		return true;

	case FSP_CMD_SP_QUERY_CAPS:
		prlog(PR_INFO, "FSP: FSP query capabilities\n");
		/* XXX Do something saner. For now do a synchronous
	         * response and hard code our capabilities
		 */
		fsp_queue_msg(fsp_mkmsg(FSP_RSP_SP_QUERY_CAPS, 4,
					0x3ff80000, 0, 0, 0), fsp_freemsg);
		ipl_state |= ipl_got_caps;
		return true;
	case FSP_CMD_FSP_FUNCTNAL:
		prlog(PR_INFO, "FSP: Got FSP Functional\n");
		ipl_state |= ipl_got_fsp_functional;
		return true;
	case FSP_CMD_ALLOC_INBOUND:
		fsp_alloc_inbound(msg);
		return true;
	case FSP_CMD_SP_RELOAD_COMP:
		prlog(PR_INFO, "FSP: SP says Reset/Reload complete\n");
		if (msg->data.bytes[3] & PPC_BIT8(0)) {
			fsp_fips_dump_notify(msg->data.words[1],
					     msg->data.words[2]);

			if (msg->data.bytes[3] & PPC_BIT8(1))
				prlog(PR_DEBUG, "      PLID is %x\n",
				      msg->data.words[3]);
		}
		if (msg->data.bytes[3] & PPC_BIT8(2)) {
			prlog(PR_DEBUG, "  A Reset/Reload was NOT done\n");
		} else {
			/* Notify clients that the FSP is back up */
			fsp_notify_rr_state(FSP_RELOAD_COMPLETE);
			fsp_repost_queued_msgs_post_rr();
		}
		return true;
	case FSP_CMD_CLOSE_HMC_INTF:
		/* Close the HMC interface */
		/* Though Sapphire does not support a HMC connection, the FSP
		 * sends this message when it is trying to open any new
		 * hypervisor session. So returning an error 0x51.
		 */
		cmd = FSP_RSP_CLOSE_HMC_INTF | FSP_STAUS_INVALID_HMC_ID;
		rsp_data = msg->data.bytes[0] << 24 | msg->data.bytes[1] << 16;
		rsp_data &= 0xffff0000;
		fsp_queue_msg(fsp_mkmsg(cmd, 1, rsp_data), fsp_freemsg);
		return true;
	}
	return false;
}


/* This is called without the FSP lock */
static void fsp_handle_command(struct fsp_msg *msg)
{
	struct fsp_cmdclass *cmdclass = fsp_get_cmdclass(msg);
	struct fsp_client *client, *next;
	u32 cmd_sub_mod;

	if (!cmdclass) {
		prerror("FSP: Got message for unknown class %x\n",
			msg->word0 & 0xff);
		goto free;
	}

	cmd_sub_mod =  (msg->word0 & 0xff) << 16;
	cmd_sub_mod |= (msg->word1 & 0xff) << 8;
	cmd_sub_mod |= (msg->word1 >> 8) & 0xff;
	
	/* Some commands are handled locally */
	if (fsp_local_command(cmd_sub_mod, msg))
		goto free;

	/* The rest go to clients */
	list_for_each_safe(&cmdclass->clientq, client, next, link) {
		if (client->message(cmd_sub_mod, msg))
			goto free;
	}

	prerror("FSP: Unhandled message %06x\n", cmd_sub_mod);

	/* We don't know whether the message expected some kind of
	 * response, so we send one anyway
	 */
	fsp_queue_msg(fsp_mkmsg((cmd_sub_mod & 0xffff00) | 0x008020, 0),
		      fsp_freemsg);
 free:
	fsp_freemsg(msg);
}

static void __fsp_fill_incoming(struct fsp *fsp, struct fsp_msg *msg,
				int dlen, u32 w0, u32 w1)
{
	unsigned int wlen, i, reg;

	msg->dlen = dlen - 8;
	msg->word0 = w0;
	msg->word1 = w1;
	wlen = (dlen + 3) >> 2;
	reg = FSP_MBX1_FDATA_AREA + 8;
	for (i = 0; i < wlen; i++) {
		msg->data.words[i] = fsp_rreg(fsp, reg);
		reg += 4;
	}

	/* Ack it (XDN) and clear HPEND & counts */
	fsp_wreg(fsp, FSP_MBX1_HCTL_REG,
		 FSP_MBX_CTL_PTS |
		 FSP_MBX_CTL_XDN |
		 FSP_MBX_CTL_HPEND |
		 FSP_MBX_CTL_HCSP_MASK |
		 FSP_MBX_CTL_DCSP_MASK);

	fsp_trace_msg(msg, TRACE_FSP_MSG_IN);
}

static void __fsp_drop_incoming(struct fsp *fsp)
{
	/* Ack it (XDN) and clear HPEND & counts */
	fsp_wreg(fsp, FSP_MBX1_HCTL_REG,
		 FSP_MBX_CTL_PTS |
		 FSP_MBX_CTL_XDN |
		 FSP_MBX_CTL_HPEND |
		 FSP_MBX_CTL_HCSP_MASK |
		 FSP_MBX_CTL_DCSP_MASK);
}

/* WARNING: This will drop the FSP lock */
static void fsp_handle_incoming(struct fsp *fsp)
{
	struct fsp_msg *msg;
	u32 h0, w0, w1;
	unsigned int dlen;
	bool special_response = false;

	h0 = fsp_rreg(fsp, FSP_MBX1_FHDR0_REG);
	dlen = (h0 >> 16) & 0xff;

	w0 = fsp_rreg(fsp, FSP_MBX1_FDATA_AREA);
	w1 = fsp_rreg(fsp, FSP_MBX1_FDATA_AREA + 4);

	prlog(PR_INSANE, "  Incoming: w0: 0x%08x, w1: 0x%08x, dlen: %d\n",
	    w0, w1, dlen);

	/* Some responses are expected out of band */
	if ((w0 & 0xff) == FSP_MCLASS_HMC_INTFMSG  &&
	    ((w1 & 0xff) == 0x8a || ((w1 & 0xff) == 0x8b)))
		special_response = true;

	/* Check for response bit */
	if (w1 & 0x80 && !special_response) {
		struct fsp_cmdclass *cmdclass = __fsp_get_cmdclass(w0 & 0xff);
		struct fsp_msg *req;

		if (!cmdclass) {
			prerror("FSP: Got response for unknown class %x\n",
				w0 & 0xff);
			__fsp_drop_incoming(fsp);
			return;
		}

		if (!cmdclass->busy || list_empty(&cmdclass->msgq)) {
			prerror("FSP #%d: Got orphan response! w0 = 0x%08x w1 = 0x%08x\n",
					fsp->index, w0, w1);
			__fsp_drop_incoming(fsp);
			return;
		}
		req = list_top(&cmdclass->msgq, struct fsp_msg, link);

		/* Check if the response seems to match the message */
		if (req->state != fsp_msg_wresp ||
		    (req->word0 & 0xff) != (w0 & 0xff) ||
		    (req->word1 & 0xff) != (w1 & 0x7f)) {
			__fsp_drop_incoming(fsp);
			prerror("FSP #%d: Response doesn't match pending msg. w0 = 0x%08x w1 = 0x%08x\n",
				fsp->index, w0, w1);
			return;
		} else {
			u64 resetbit = ~fsp_get_class_bit(req->word0 & 0xff);
			fsp_cmdclass_resp_bitmask &= resetbit;
			cmdclass->timesent = 0;
		}

		/* Allocate response if needed XXX We need to complete
		 * the original message with some kind of error here ?
		 */
		if (!req->resp) {
			req->resp = __fsp_allocmsg();
			if (!req->resp) {
				__fsp_drop_incoming(fsp);
				prerror("FSP #%d: Failed to allocate response\n",
					fsp->index);
				return;
			}
		}

		/* Populate and complete (will drop the lock) */
		req->resp->state = fsp_msg_response;
		__fsp_fill_incoming(fsp, req->resp, dlen, w0, w1);
		fsp_complete_msg(req);
		return;
	}

	/* Allocate an incoming message */
	msg = __fsp_allocmsg();
	if (!msg) {
		__fsp_drop_incoming(fsp);
		prerror("FSP #%d: Failed to allocate incoming msg\n",
			fsp->index);
		return;
	}
	msg->state = fsp_msg_incoming;
	__fsp_fill_incoming(fsp, msg, dlen, w0, w1);

	/* Handle FSP commands. This can recurse into fsp_queue_msg etc.. */
	unlock(&fsp_lock);
	fsp_handle_command(msg);
	lock(&fsp_lock);
}

static void fsp_check_queues(struct fsp *fsp)
{
	int i;

	/* XXX In the long run, we might want to have a queue of
	 * classes waiting to be serviced to speed this up, either
	 * that or a bitmap.
	 */
	for (i = 0; i <= (FSP_MCLASS_LAST - FSP_MCLASS_FIRST); i++) {
		struct fsp_cmdclass *cmdclass = &fsp_cmdclass[i];

		if (fsp->state != fsp_mbx_idle)
			break;
		if (cmdclass->busy || list_empty(&cmdclass->msgq))
			continue;
		fsp_poke_queue(cmdclass);
	}
}

static void __fsp_poll(bool interrupt)
{
	struct fsp_iopath *iop;
	struct fsp *fsp = fsp_get_active();
	u32 ctl, hdir = 0;
	bool psi_irq;

	/*
	 * The tracer isn't terribly efficient at detecting dups
	 * especially when coming from multiple CPUs so we do our
	 * own change-detection locally
	 */
	static u32 hdir_last_trace;
	static u32 ctl_last_trace;
	static bool psi_irq_last_trace;
	static bool irq_last_trace;

	if (!fsp)
		return;

	/* Crazy interrupt handling scheme:
	 *
	 * In order to avoid "losing" interrupts when polling the mbox
	 * we only clear interrupt conditions when called as a result of
	 * an interrupt.
	 *
	 * That way, if a poll clears, for example, the HPEND condition,
	 * the interrupt remains, causing a dummy interrupt later on
	 * thus allowing the OS to be notified of a state change (ie it
	 * doesn't need every poll site to monitor every state change).
	 *
	 * However, this scheme is complicated by the fact that we need
	 * to clear the interrupt condition after we have cleared the
	 * original condition in HCTL, and we might have long stale
	 * interrupts which we do need to eventually get rid of. However
	 * clearing interrupts in such a way is racy, so we need to loop
	 * and re-poll HCTL after having done so or we might miss an
	 * event. It's a latency risk, but unlikely and probably worth it.
	 */

 again:
	if (fsp->active_iopath < 0) {
		/* That should never happen */
		if (interrupt && (fsp->state != fsp_mbx_rr))
			prerror("FSP: Interrupt with no working IO path\n");
		return;
	}
	iop = &fsp->iopath[fsp->active_iopath];

	/* Handle host initiated resets */
	if (fsp_in_hir(fsp)) {
		fsp_hir_poll(fsp, iop->psi);
		return;
	}

	/* Check for error state and handle R&R completion */
	fsp_handle_errors(fsp);

	/*
	 * The above might have triggered and R&R, check that we
	 * are still functional
	 */
	if ((fsp->active_iopath < 0) || fsp_in_hir(fsp))
		return;
	iop = &fsp->iopath[fsp->active_iopath];

	/* Read interrupt status (we may or may not use it) */
	hdir = fsp_rreg(fsp, FSP_HDIR_REG);

	/* Read control now as well so we can trace them */
	ctl = fsp_rreg(fsp, FSP_MBX1_HCTL_REG);

	/* Ditto with PSI irq state */
	psi_irq = psi_poll_fsp_interrupt(iop->psi);

	/* Trace it if anything changes */
	if (hdir != hdir_last_trace || ctl != ctl_last_trace ||
	    interrupt != irq_last_trace || psi_irq != psi_irq_last_trace) {
		fsp_trace_event(fsp, TRACE_FSP_EVT_POLL_IRQ,
				interrupt, hdir, ctl, psi_irq);

		hdir_last_trace = hdir;
		ctl_last_trace = ctl;
		irq_last_trace = interrupt;
		psi_irq_last_trace = psi_irq;
	}

	/*
	 * We *MUST* ignore the MBOX2 bits here. While MBOX2 cannot generate
	 * interrupt, it might still latch some bits here (and we found cases
	 * where the MBOX2 XUP would be set). If that happens, clearing HDIR
	 * never works (the bit gets set again immediately) because we don't
	 * clear the condition in HTCL2 and thus we loop forever.
	 */
	hdir &= FSP_DBIRQ_MBOX1;

	/*
	 * Sanity check: If an interrupt is pending and we are in polling
	 * mode, check that the PSI side is also pending. If some bit is
	 * set, just clear and move on.
	 */
	if (hdir && !interrupt && !psi_irq) {
		prerror("FSP: WARNING ! HDIR 0x%08x but no PSI irq !\n", hdir);
		fsp_wreg(fsp, FSP_HDIR_REG, hdir);
	}

	/*
	 * We should never have the mbox in error state here unless it
	 * was fine until some printf inside fsp_handle_errors() caused
	 * the console to poke the FSP which detected a branch new error
	 * in the process. Let's be safe rather than sorry and handle that
	 * here
	 */
	if (fsp_in_hir(fsp) || fsp->state == fsp_mbx_err) {
		prerror("FSP: Late error state detection\n");
		goto again;
	}

	/*
	 * If we are in an R&R state with an active IO path, we
	 * shouldn't be getting interrupts. If we do, just clear
	 * the condition and print a message
	 */
	if (fsp->state == fsp_mbx_rr) {
		if (interrupt) {
			prerror("FSP: Interrupt in RR state [HDIR=0x%08x]\n",
				hdir);
			fsp_wreg(fsp, FSP_HDIR_REG, hdir);
		}
		return;
	}

	/* Poll FSP CTL */
	if (ctl & (FSP_MBX_CTL_XUP | FSP_MBX_CTL_HPEND))
		prlog(PR_INSANE, "FSP #%d: poll, ctl: %x\n", fsp->index, ctl);

	/* Do we have a pending message waiting to complete ? */
	if (ctl & FSP_MBX_CTL_XUP) {
		fsp_wreg(fsp, FSP_MBX1_HCTL_REG, FSP_MBX_CTL_XUP);
		if (fsp->state == fsp_mbx_send) {
			/* mbox is free */
			fsp->state = fsp_mbx_idle;

			/* Complete message (will break the lock) */
			fsp_complete_send(fsp);

			/* Lock can have been broken, so ctl is now
			 * potentially invalid, let's recheck
			 */
			goto again;
		} else {
			prerror("FSP #%d: Got XUP with no pending message !\n",
				fsp->index);
		}
	}

	if (fsp->state == fsp_mbx_send) {
		/* XXX Handle send timeouts!!! */
	}

	/* Is there an incoming message ? This will break the lock as well */
	if (ctl & FSP_MBX_CTL_HPEND)
		fsp_handle_incoming(fsp);

	/* Note: Lock may have been broken above, thus ctl might be invalid
	 * now, don't use it any further.
	 */

	/* Check for something else to send */
	if (fsp->state == fsp_mbx_idle)
		fsp_check_queues(fsp);

	/* Clear interrupts, and recheck HCTL if any occurred */
	if (interrupt && hdir) {
		fsp_wreg(fsp, FSP_HDIR_REG, hdir);
		goto again;
	}
}

void fsp_interrupt(void)
{
	lock(&fsp_lock);
	__fsp_poll(true);
	unlock(&fsp_lock);
}

int fsp_sync_msg(struct fsp_msg *msg, bool autofree)
{
	int rc;

	rc = fsp_queue_msg(msg, NULL);
	if (rc)
		goto bail;

	while(fsp_msg_busy(msg))
		opal_run_pollers();

	switch(msg->state) {
	case fsp_msg_done:
		rc = 0;
		break;
	case fsp_msg_timeout:
		rc = -1; /* XXX to improve */
		break;
	default:
		rc = -1; /* Should not happen... (assert ?) */
	}

	if (msg->resp)
		rc = (msg->resp->word1 >> 8) & 0xff;
 bail:
	if (autofree)
		fsp_freemsg(msg);
	return rc;
}

void fsp_register_client(struct fsp_client *client, u8 msgclass)
{
	struct fsp_cmdclass *cmdclass = __fsp_get_cmdclass(msgclass);

	if (!fsp_present())
		return;
	assert(cmdclass);
	list_add_tail(&cmdclass->clientq, &client->link);
}

void fsp_unregister_client(struct fsp_client *client, u8 msgclass)
{
	struct fsp_cmdclass *cmdclass = __fsp_get_cmdclass(msgclass);

	if (!fsp_present())
		return;
	assert(cmdclass);
	list_del_from(&cmdclass->clientq, &client->link);
}

static int fsp_init_mbox(struct fsp *fsp)
{
	unsigned int i;
	u32 reg;

	/*
	 * Note: The documentation contradicts itself as to
	 * whether the HDIM bits should be set or cleared to
	 * enable interrupts
	 *
	 * This seems to work...
	 */

	/* Mask all interrupts */
	fsp_wreg(fsp, FSP_HDIM_CLR_REG, FSP_DBIRQ_ALL);

	/* Clear all errors */
	fsp_wreg(fsp, FSP_HDES_REG, FSP_DBERRSTAT_CLR1 | FSP_DBERRSTAT_CLR2);

	/* Initialize data area as the doco says */
	for (i = 0; i < 0x40; i += 4)
		fsp_wreg(fsp, FSP_MBX1_HDATA_AREA + i, 0);

	/*
	 * Clear whatever crap may remain in HDCR. Do not write XDN as that
	 * would be interpreted incorrectly as an R&R completion which
	 * we aren't ready to send yet !
	 */
	fsp_wreg(fsp, FSP_MBX1_HCTL_REG, FSP_MBX_CTL_XUP | FSP_MBX_CTL_HPEND |
		 FSP_MBX_CTL_HCSP_MASK | FSP_MBX_CTL_DCSP_MASK |
		 FSP_MBX_CTL_PTS);

	/* Clear all pending interrupts */
	fsp_wreg(fsp, FSP_HDIR_REG, FSP_DBIRQ_ALL);

	/* Enable all mbox1 interrupts */
	fsp_wreg(fsp, FSP_HDIM_SET_REG, FSP_DBIRQ_MBOX1);

	/* Decode what FSP we are connected to */
	reg = fsp_rreg(fsp, FSP_SCRATCH0_REG);
	if (reg & PPC_BIT32(0)) {		/* Is it a valid connection */
		if (reg & PPC_BIT32(3))
			prlog(PR_INFO, "FSP: Connected to FSP-B\n");
		else
			prlog(PR_INFO, "FSP: Connected to FSP-A\n");
	}

	return 0;
}

/* We use a single fixed TCE table for all PSI interfaces */
static void fsp_init_tce_table(void)
{
	fsp_tce_table = (u64 *)PSI_TCE_TABLE_BASE;

	/* Memset the larger table even if we only use the smaller
	 * one on P7
	 */
	memset(fsp_tce_table, 0, PSI_TCE_TABLE_SIZE_P8);
}

void fsp_tce_map(u32 offset, void *addr, u32 size)
{
	u64 raddr = (u64)addr;

	assert(!(offset & 0xfff));
	assert(!(raddr  & 0xfff));
	assert(!(size   & 0xfff));

	size   >>= 12;
	offset >>= 12;

	while(size--) {
		fsp_tce_table[offset++] = raddr | 0x3;
		raddr += 0x1000;
	}
}

void fsp_tce_unmap(u32 offset, u32 size)
{
	assert(!(offset & 0xfff));
	assert(!(size   & 0xfff));

	size   >>= 12;
	offset >>= 12;

	while(size--)
		fsp_tce_table[offset++] = 0;
}

static struct fsp *fsp_find_by_index(int index)
{
	struct fsp *fsp = first_fsp;

	do {
		if (fsp->index == index)
			return fsp;
	} while (fsp->link != first_fsp);

	return NULL;
}

static void fsp_init_links(struct dt_node *fsp_node)
{
	const struct dt_property *linksprop;
	int i, index;
	struct fsp *fsp;
	struct fsp_iopath *fiop;

	linksprop = dt_find_property(fsp_node, "ibm,psi-links");
	index = dt_prop_get_u32(fsp_node, "reg");
	fsp = fsp_find_by_index(index);
	if (!fsp) {
		prerror("FSP: FSP with index %d not found\n", index);
		return;
	}

	fsp->state = fsp_mbx_idle;

	/* Iterate all links */
	for (i = 0; i < fsp->iopath_count; i++) {
		u64 reg;
		u32 link;

		link = ((const u32 *)linksprop->prop)[i];
		fiop = &fsp->iopath[i];
		fiop->psi = psi_find_link(link);
		if (fiop->psi == NULL) {
			prerror("FSP #%d: Couldn't find PSI link\n",
				fsp->index);
			continue;
		}

		prlog(PR_DEBUG, "FSP #%d: Found PSI HB link to chip %d\n",
		      fsp->index, link);

		psi_fsp_link_in_use(fiop->psi);

		/* Get the FSP register window */
		reg = in_be64(fiop->psi->regs + PSIHB_FSPBAR);
		fiop->fsp_regs = (void *)(reg | (1ULL << 63) |
				dt_prop_get_u32(fsp_node, "reg-offset"));
	}
}

static void fsp_update_links_states(struct fsp *fsp)
{
	struct fsp_iopath *fiop;
	unsigned int i;

	/* Iterate all links */
	for (i = 0; i < fsp->iopath_count; i++) {
		fiop = &fsp->iopath[i];
		if (!fiop->psi)
			continue;
		if (!fiop->psi->working)
			fiop->state = fsp_path_bad;
		else if (fiop->psi->active) {
			fsp->active_iopath = i;
			fiop->state = fsp_path_active;
		} else
			fiop->state = fsp_path_backup;
	}

	if (fsp->active_iopath >= 0) {
		if (!active_fsp || (active_fsp != fsp))
			active_fsp = fsp;

		fsp_inbound_off = 0;
		fiop = &fsp->iopath[fsp->active_iopath];
		psi_init_for_fsp(fiop->psi);
		fsp_init_mbox(fsp);
		psi_enable_fsp_interrupt(fiop->psi);
	}
}

void fsp_reinit_fsp(void)
{
	struct fsp *fsp;

	/* Notify all FSPs to check for an updated link state */
	for (fsp = first_fsp; fsp; fsp = fsp->link)
		fsp_update_links_states(fsp);
}

static void fsp_create_fsp(struct dt_node *fsp_node)
{
	const struct dt_property *linksprop;
	struct fsp *fsp;
	int count, index;

	index = dt_prop_get_u32(fsp_node, "reg");
	prlog(PR_INFO, "FSP #%d: Found in device-tree, setting up...\n",
	      index);

	linksprop = dt_find_property(fsp_node, "ibm,psi-links");
	if (!linksprop || linksprop->len < 4) {
		prerror("FSP #%d: No links !\n", index);
		return;
	}

	fsp = zalloc(sizeof(struct fsp));
	if (!fsp) {
		prerror("FSP #%d: Can't allocate memory !\n", index);
		return;
	}

	fsp->index = index;
	fsp->active_iopath = -1;

	count = linksprop->len / 4;
	prlog(PR_DEBUG, "FSP #%d: Found %d IO PATH\n", index, count);
	if (count > FSP_MAX_IOPATH) {
		prerror("FSP #%d: WARNING, limited to %d IO PATH\n",
			index, FSP_MAX_IOPATH);
		count = FSP_MAX_IOPATH;
	}
	fsp->iopath_count = count;

	fsp->link = first_fsp;
	first_fsp = fsp;

	fsp_init_links(fsp_node);
	fsp_update_links_states(fsp);
}

static void fsp_opal_poll(void *data __unused)
{
	if (try_lock(&fsp_lock)) {
		__fsp_poll(false);
		unlock(&fsp_lock);
	}
}

static bool fsp_init_one(const char *compat)
{
	struct dt_node *fsp_node;
	bool inited = false;

	dt_for_each_compatible(dt_root, fsp_node, compat) {
		if (!inited) {
			int i;
	
			/* Initialize the per-class msg queues */
			for (i = 0;
			     i <= (FSP_MCLASS_LAST - FSP_MCLASS_FIRST); i++) {
				list_head_init(&fsp_cmdclass[i].msgq);
				list_head_init(&fsp_cmdclass[i].clientq);
				list_head_init(&fsp_cmdclass[i].rr_queue);
			}

			/* Init the queues for RR notifier cmdclass */
			list_head_init(&fsp_cmdclass_rr.msgq);
			list_head_init(&fsp_cmdclass_rr.clientq);
			list_head_init(&fsp_cmdclass_rr.rr_queue);

			/* Register poller */
			opal_add_poller(fsp_opal_poll, NULL);

			inited = true;
		}

		/* Create the FSP data structure */
		fsp_create_fsp(fsp_node);
	}

	return inited;
}

void fsp_init(void)
{
	prlog(PR_DEBUG, "FSP: Looking for FSP...\n");

	fsp_init_tce_table();

	if (!fsp_init_one("ibm,fsp1") && !fsp_init_one("ibm,fsp2")) {
		prlog(PR_DEBUG, "FSP: No FSP on this machine\n");
		return;
	}
}

bool fsp_present(void)
{
	return first_fsp != NULL;
}

static void fsp_timeout_poll(void *data __unused)
{
	u64 now = mftb();
	u64 timeout_val = 0;
	u64 cmdclass_resp_bitmask = fsp_cmdclass_resp_bitmask;
	struct fsp_cmdclass *cmdclass = NULL;
	struct fsp_msg *req = NULL;
	u32 index = 0;

	if (timeout_timer == 0)
		timeout_timer = now + secs_to_tb(30);

	/* The lowest granularity for a message timeout is 30 secs.
	 * So every 30secs, check if there is any message
	 * waiting for a response from the FSP
	 */
	if ((tb_compare(now, timeout_timer) == TB_AAFTERB) ||
		(tb_compare(now, timeout_timer) == TB_AEQUALB))
		timeout_timer = now + secs_to_tb(30);
	else
		return;

	while (cmdclass_resp_bitmask) {
		u64 time_sent = 0;
		u64 time_to_comp = 0;

		if (!(cmdclass_resp_bitmask & 0x1))
			goto next_bit;

		cmdclass = &fsp_cmdclass[index];
		timeout_val = secs_to_tb((cmdclass->timeout) * 60);
		time_sent = cmdclass->timesent;
		time_to_comp = now - cmdclass->timesent;

		/* Now check if the response has timed out */
		if (tb_compare(time_to_comp, timeout_val) == TB_AAFTERB) {
			u64 resetbit = 0;
			u32 w0, w1;
			enum fsp_msg_state mstate;

			/* Take the FSP lock now and re-check */
			lock(&fsp_lock);
			if (!(fsp_cmdclass_resp_bitmask & (1 << index)) ||
			    time_sent != cmdclass->timesent) {
				unlock(&fsp_lock);
				goto next_bit;
			}
			req = list_top(&cmdclass->msgq,	struct fsp_msg, link);
			w0 = req->word0;
			w1 = req->word1;
			mstate = req->state;
			prlog(PR_WARNING, "FSP: Response from FSP timed out,"
			      " word0 = %x, word1 = %x state: %d\n",
			      w0, w1, mstate);
			fsp_reg_dump();
			resetbit = ~fsp_get_class_bit(req->word0 & 0xff);
			fsp_cmdclass_resp_bitmask &= resetbit;
			cmdclass->timesent = 0;
			if (req->resp)
				req->resp->state = fsp_msg_timeout;
			fsp_complete_msg(req);
			__fsp_trigger_reset();
			unlock(&fsp_lock);
			log_simple_error(&e_info(OPAL_RC_FSP_POLL_TIMEOUT),
					 "FSP: Response from FSP timed out, word0 = %x,"
					 "word1 = %x state: %d\n", w0, w1, mstate);
		}
	next_bit:
		cmdclass_resp_bitmask = cmdclass_resp_bitmask >> 1;
		index++;
	}
}

void fsp_opl(void)
{
	struct dt_node *iplp;

	if (!fsp_present())
		return;

	/* Send OPL */
	ipl_state |= ipl_opl_sent;
	fsp_sync_msg(fsp_mkmsg(FSP_CMD_OPL, 0), true);
	while(!(ipl_state & ipl_got_continue))
		opal_run_pollers();

	/* Send continue ACK */
	fsp_sync_msg(fsp_mkmsg(FSP_CMD_CONTINUE_ACK, 0), true);

	/* Wait for various FSP messages */
	prlog(PR_INFO, "INIT: Waiting for FSP to advertize new role...\n");
	while(!(ipl_state & ipl_got_new_role))
		opal_run_pollers();
	prlog(PR_INFO, "INIT: Waiting for FSP to request capabilities...\n");
	while(!(ipl_state & ipl_got_caps))
		opal_run_pollers();

	/* Initiate the timeout poller */
	opal_add_poller(fsp_timeout_poll, NULL);

	/* Tell FSP we are in standby */
	prlog(PR_INFO, "INIT: Sending HV Functional: Standby...\n");
	fsp_sync_msg(fsp_mkmsg(FSP_CMD_HV_FUNCTNAL, 1, 0x01000000), true);

	/* Wait for FSP functional */
	prlog(PR_INFO, "INIT: Waiting for FSP functional\n");
	while(!(ipl_state & ipl_got_fsp_functional))
		opal_run_pollers();

	/* Tell FSP we are in running state */
	prlog(PR_INFO, "INIT: Sending HV Functional: Runtime...\n");
	fsp_sync_msg(fsp_mkmsg(FSP_CMD_HV_FUNCTNAL, 1, 0x02000000), true);

	/*
	 * For the factory reset case, FSP sends us the PCI Bus
	 * Reset request. We don't have to do anything special with
	 * PCI bus numbers here; just send the Power Down message
	 * with modifier 0x02 to FSP.
	 */
	iplp = dt_find_by_path(dt_root, "ipl-params/ipl-params");
	if (iplp && dt_find_property(iplp, "pci-busno-reset-ipl")) {
		prlog(PR_DEBUG, "INIT: PCI Bus Reset requested."
		      " Sending Power Down\n");
		fsp_sync_msg(fsp_mkmsg(FSP_CMD_POWERDOWN_PCIRS, 0), true);
	}

	/*
	 * Tell FSP we are in running state with all partitions.
	 *
	 * This is need otherwise the FSP will not reset it's reboot count
	 * on failures. Ideally we should send that when we know the
	 * OS is up but we don't currently have a very good way to do
	 * that so this will do as a stop-gap
	 */
	prlog(PR_NOTICE, "INIT: Sending HV Functional: Runtime all partitions\n");
	fsp_sync_msg(fsp_mkmsg(FSP_CMD_HV_FUNCTNAL, 1, 0x04000000), true);
}

uint32_t fsp_adjust_lid_side(uint32_t lid_no)
{
	struct dt_node *iplp;
	const char *side = NULL;

	iplp = dt_find_by_path(dt_root, "ipl-params/ipl-params");
	if (iplp)
		side = dt_prop_get_def(iplp, "cec-ipl-side", NULL);
	if (!side || !strcmp(side, "temp"))
		lid_no |= ADJUST_T_SIDE_LID_NO;
	return lid_no;
}

int fsp_fetch_data(uint8_t flags, uint16_t id, uint32_t sub_id,
		   uint32_t offset, void *buffer, size_t *length)
{
	uint32_t total, remaining = *length;
	uint64_t baddr;
	uint64_t balign, boff, bsize;
	struct fsp_msg *msg;
	static struct lock fsp_fetch_lock = LOCK_UNLOCKED;

	*length = total = 0;

	if (!fsp_present())
		return -ENODEV;

	prlog(PR_DEBUG, "FSP: Fetch data id: %02x sid: %08x to %p"
	      "(0x%x bytes)\n",
	      id, sub_id, buffer, remaining);

	/*
	 * Use a lock to avoid multiple processors trying to fetch
	 * at the same time and colliding on the TCE space
	 */
	lock(&fsp_fetch_lock);

	while(remaining) {
		uint32_t chunk, taddr, woffset, wlen;
		uint8_t rc;

		/* Calculate alignment skew */
		baddr = (uint64_t)buffer;
		balign = baddr & ~0xffful;
		boff = baddr & 0xffful;

		/* Get a chunk */
		chunk = remaining;
		if (chunk > (PSI_DMA_FETCH_SIZE - boff))
			chunk = PSI_DMA_FETCH_SIZE - boff;
		bsize = ((boff + chunk) + 0xfff) & ~0xffful;

		prlog(PR_DEBUG, "FSP:  0x%08x bytes balign=%llx"
		      " boff=%llx bsize=%llx\n",
		      chunk, balign, boff, bsize);
		fsp_tce_map(PSI_DMA_FETCH, (void *)balign, bsize);
		taddr = PSI_DMA_FETCH + boff;
		msg = fsp_mkmsg(FSP_CMD_FETCH_SP_DATA, 6,
				flags << 16 | id, sub_id, offset,
				0, taddr, chunk);
		rc = fsp_sync_msg(msg, false);
		fsp_tce_unmap(PSI_DMA_FETCH, bsize);

		woffset = msg->resp->data.words[1];
		wlen = msg->resp->data.words[2];
		prlog(PR_DEBUG, "FSP:   -> rc=0x%02x off: %08x"
		      " twritten: %08x\n",
		      rc, woffset, wlen);
		fsp_freemsg(msg);

		/* XXX Is flash busy (0x3f) a reason for retry ? */
		if (rc != 0 && rc != 2) {
			unlock(&fsp_fetch_lock);
			return -EIO;
		}

		remaining -= wlen;
		total += wlen;
		buffer += wlen;
		offset += wlen;

		/* The doc seems to indicate that we get rc=2 if there's
		 * more data and rc=0 if we reached the end of file, but
		 * it looks like I always get rc=0, so let's consider
		 * an EOF if we got less than what we asked
		 */
		if (wlen < chunk)
			break;
	}
	unlock(&fsp_fetch_lock);

	*length = total;

	return 0;
}

/*
 * Asynchronous fsp fetch data call
 *
 * Note:
 *   buffer = PSI DMA address space
 */
int fsp_fetch_data_queue(uint8_t flags, uint16_t id, uint32_t sub_id,
			 uint32_t offset, void *buffer, size_t *length,
			 void (*comp)(struct fsp_msg *msg))
{
	struct fsp_msg *msg;
	uint32_t chunk = *length;

	if (!comp)
		return OPAL_PARAMETER;

	msg = fsp_mkmsg(FSP_CMD_FETCH_SP_DATA, 0x6, flags << 16 | id,
			sub_id, offset, 0, buffer, chunk);
	if (!msg) {
		prerror("FSP: allocation failed!\n");
		return OPAL_INTERNAL_ERROR;
	}
	if (fsp_queue_msg(msg, comp)) {
		fsp_freemsg(msg);
		prerror("FSP: Failed to queue fetch data message\n");
		return OPAL_INTERNAL_ERROR;
	}
	return OPAL_SUCCESS;
}

bool fsp_load_resource(enum resource_id id, void *buf, size_t *size)
{
	uint32_t lid_no, lid;
	size_t tmp_size;
	int rc;

	switch (id) {
	case RESOURCE_ID_KERNEL:
		lid_no = KERNEL_LID_OPAL;
		break;
	case RESOURCE_ID_INITRAMFS:
		lid_no = INITRAMFS_LID_OPAL;
		break;
	default:
		return false;
	}

retry:
	tmp_size = *size;

	printf("Trying to load OPAL LID %08x...\n", lid_no);
	lid = fsp_adjust_lid_side(lid_no);
	rc = fsp_fetch_data(0, FSP_DATASET_NONSP_LID, lid, 0, buf, &tmp_size);

	/* Fall back to a PHYP LID for kernel loads */
	if (rc && lid_no == KERNEL_LID_OPAL) {
		const char *ltype = dt_prop_get_def(dt_root, "lid-type", NULL);
		if (!ltype || strcmp(ltype, "opal")) {
			prerror("Failed to load in OPAL mode...\n");
			return false;
		}
		printf("Trying to load as PHYP LID...\n");
		lid_no = KERNEL_LID_PHYP;
		goto retry;
	}

	if (rc) {
		prerror("Failed to load LID\n");
		return false;
	}

	*size = tmp_size;
	return true;
}

void fsp_used_by_console(void)
{
	fsp_lock.in_con_path = true;

	/*
	 * Some other processor might hold it without having
	 * disabled the console locally so let's make sure that
	 * is over by taking/releasing the lock ourselves
	 */
	lock(&fsp_lock);
	unlock(&fsp_lock);
}
