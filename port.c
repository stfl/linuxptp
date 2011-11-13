/**
 * @file port.c
 * @note Copyright (C) 2011 Richard Cochran <richardcochran@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "bmc.h"
#include "clock.h"
#include "msg.h"
#include "port.h"
#include "print.h"
#include "util.h"

#define PTP_VERSION 2

#define LOG_MIN_DELAY_REQ_INTERVAL      0 /* allow Delay_Req every 1 sec */
#define LOG_ANNOUNCE_INTERVAL           1 /* every 2 sec */
#define ANNOUNCE_RECEIPT_TIMEOUT        3 /* wait for 3 missing announce */
#define LOG_SYNC_INTERVAL               0 /* every 1 sec */
#define LOG_MIN_PDELAY_REQ_INTERVAL     2 /* allow PDelay_Req every 4 sec */

struct port {
	char *name;
	struct clock *clock;
	struct transport *transport;
	enum timestamp_type timestamping;
	struct fdarray fda;
	struct foreign_clock *best;
	struct ptp_message *last_follow_up;
	struct ptp_message *last_sync;
	struct ptp_message *delay_req;
	UInteger16 seqnum;
	/* portDS */
	struct PortIdentity portIdentity;
	enum port_state     state; /*portState*/
	Integer8            logMinDelayReqInterval;
	TimeInterval        peerMeanPathDelay;
	Integer8            logAnnounceInterval;
	UInteger8           announceReceiptTimeout;
	Integer8            logSyncInterval;
	Enumeration8        delayMechanism;
	Integer8            logMinPdelayReqInterval;
	unsigned int        versionNumber; /*UInteger4*/
	/* foreignMasterDS */
	LIST_HEAD(fm, foreign_clock) foreign_masters;
};

#define portnum(p) (p->portIdentity.portNumber)

#define NSEC2SEC 1000000000LL

static int announce_compare(struct ptp_message *m1, struct ptp_message *m2)
{
	struct announce_msg *a = &m1->announce, *b = &m2->announce;
	int len =
		sizeof(a->grandmasterPriority1) +
		sizeof(a->grandmasterClockQuality) +
		sizeof(a->grandmasterPriority2) +
		sizeof(a->grandmasterIdentity) +
		sizeof(a->stepsRemoved);

	return memcmp(&a->grandmasterPriority1, &b->grandmasterPriority1, len);
}

static void announce_to_dataset(struct ptp_message *m, struct clock *c,
				struct dataset *out)
{
	struct announce_msg *a = &m->announce;
	out->priority1    = a->grandmasterPriority1;
	out->identity     = a->grandmasterIdentity;
	out->quality      = a->grandmasterClockQuality;
	out->priority2    = a->grandmasterPriority2;
	out->stepsRemoved = a->stepsRemoved;
	out->sender       = m->header.sourcePortIdentity;
	out->receiver     = clock_parent_identity(c);
}

static int msg_current(struct ptp_message *m, struct timespec now)
{
	int64_t t1, t2, tmo;
	t1 = m->ts.host.tv_sec * NSEC2SEC + m->ts.host.tv_nsec;
	t2 = now.tv_sec * NSEC2SEC + now.tv_nsec;
	tmo = 4 * (1 << m->header.logMessageInterval) * NSEC2SEC;
	return t2 - t1 < tmo;
}

static int msg_source_equal(struct ptp_message *m1, struct foreign_clock *fc)
{
	struct PortIdentity *id1, *id2;
	id1 = &m1->header.sourcePortIdentity;
	id2 = &fc->dataset.sender;
	return 0 == memcmp(id1, id2, sizeof(*id1));
}

static int pid_eq(struct PortIdentity *a, struct PortIdentity *b)
{
	return 0 == memcmp(a, b, sizeof(*a));
}

static void fc_clear(struct foreign_clock *fc)
{
	struct ptp_message *m;

	while (fc->n_messages) {
		m = TAILQ_LAST(&fc->messages, messages);
		TAILQ_REMOVE(&fc->messages, m, list);
		fc->n_messages--;
		msg_put(m);
	}
}

static void fc_prune(struct foreign_clock *fc)
{
	struct timespec now;
	struct ptp_message *m;

	clock_gettime(CLOCK_MONOTONIC, &now);

	while (fc->n_messages > FOREIGN_MASTER_THRESHOLD) {
		m = TAILQ_LAST(&fc->messages, messages);
		TAILQ_REMOVE(&fc->messages, m, list);
		fc->n_messages--;
		msg_put(m);
	}

	while (!TAILQ_EMPTY(&fc->messages)) {
		m = TAILQ_LAST(&fc->messages, messages);
		if (msg_current(m, now))
			break;
		TAILQ_REMOVE(&fc->messages, m, list);
		fc->n_messages--;
		msg_put(m);
	}
}

/*
 * Returns non-zero if the announce message is different than last.
 */
static int add_foreign_master(struct port *p, struct ptp_message *m)
{
	struct foreign_clock *fc;
	struct ptp_message *tmp;
	int broke_threshold = 0, diff = 0;

	LIST_FOREACH(fc, &p->foreign_masters, list) {
		if (msg_source_equal(m, fc))
			break;
	}
	if (!fc) {
		pr_info("port %hu: new foreign master %s", portnum(p),
			pid2str(&m->header.sourcePortIdentity));

		fc = malloc(sizeof(*fc));
		if (!fc) {
			pr_err("low memory, failed to add foreign master");
			return 0;
		}
		memset(fc, 0, sizeof(*fc));
		LIST_INSERT_HEAD(&p->foreign_masters, fc, list);
		fc->port = p;
		fc->dataset.sender = m->header.sourcePortIdentity;
		/* We do not count this first message, see 9.5.3(b) */
		return 0;
	}

	/*
	 * If this message breaks the threshold, that is an important change.
	 */
	fc_prune(fc);
	if (FOREIGN_MASTER_THRESHOLD - 1 == fc->n_messages)
		broke_threshold = 1;

	/*
	 * Okay, go ahead and add this announcement.
	 */
	msg_get(m);
	fc->n_messages++;
	TAILQ_INSERT_HEAD(&fc->messages, m, list);

	/*
	 * Test if this announcement contains changed information.
	 */
	if (fc->n_messages > 1) {
		tmp = TAILQ_NEXT(m, list);
		diff = announce_compare(m, tmp);
	}

	return broke_threshold || diff;
}

static int port_clr_tmo(int fd)
{
	struct itimerspec tmo = {
		{0, 0}, {0, 0}
	};
	return timerfd_settime(fd, 0, &tmo, NULL);
}

static int port_set_announce_tmo(struct port *p)
{
	struct itimerspec tmo = {
		{0, 0}, {0, 0}
	};

	tmo.it_value.tv_sec =
		p->announceReceiptTimeout * (1 << p->logAnnounceInterval);

	return timerfd_settime(p->fda.fd[FD_ANNOUNCE_TIMER], 0, &tmo, NULL);
}

static int port_set_delay_tmo(struct port *p)
{
	struct itimerspec tmo = {
		{0, 0}, {0, 0}
	};

	tmo.it_value.tv_sec = 1 << (p->logMinDelayReqInterval + 1);

	return timerfd_settime(p->fda.fd[FD_DELAY_TIMER], 0, &tmo, NULL);
}

static int port_delay_request(struct port *p)
{
	struct ptp_message *msg;
	int cnt, pdulen;

	msg = msg_allocate();
	if (!msg)
		return -1;
	memset(msg, 0, sizeof(*msg));

	pdulen = sizeof(struct delay_req_msg);
	msg->hwts.type = p->timestamping;

	msg->header.tsmt               = DELAY_REQ;
	msg->header.ver                = PTP_VERSION;
	msg->header.messageLength      = pdulen;
	msg->header.domainNumber       = clock_domain_number(p->clock);
	msg->header.sourcePortIdentity = p->portIdentity;
	msg->header.sequenceId         = p->seqnum++;
	msg->header.control            = CTL_DELAY_REQ;
	msg->header.logMessageInterval = 0x7f;

	if (msg_pre_send(msg))
		goto out;

	cnt = p->transport->send(&p->fda, 1, msg, pdulen, &msg->hwts);
	if (cnt <= 0)
		goto out;

	if (p->delay_req)
		msg_put(p->delay_req);

	p->delay_req = msg;
	return 0;
out:
	msg_put(msg);
	return -1;
}

static int port_initialize(struct port *p)
{
	int fd1, fd2;

	p->logMinDelayReqInterval  = LOG_MIN_DELAY_REQ_INTERVAL;
	p->peerMeanPathDelay       = 0;
	p->logAnnounceInterval     = LOG_ANNOUNCE_INTERVAL;
	p->announceReceiptTimeout  = ANNOUNCE_RECEIPT_TIMEOUT;
	p->logSyncInterval         = LOG_SYNC_INTERVAL;
	p->logMinPdelayReqInterval = LOG_MIN_PDELAY_REQ_INTERVAL;

	fd1 = timerfd_create(CLOCK_MONOTONIC, 0);
	if (fd1 < 0) {
		pr_err("timerfd_create: %s", strerror(errno));
		goto no_timer1;
	}
	fd2 = timerfd_create(CLOCK_MONOTONIC, 0);
	if (fd2 < 0) {
		pr_err("timerfd_create: %s", strerror(errno));
		goto no_timer2;
	}
	if (p->transport->open(p->name, &p->fda, p->timestamping))
		goto no_tropen;

	p->fda.fd[FD_ANNOUNCE_TIMER] = fd1;
	p->fda.cnt++;
	p->fda.fd[FD_DELAY_TIMER] = fd2;
	p->fda.cnt++;

	if (port_set_announce_tmo(p))
		goto no_tmo;

	clock_install_fda(p->clock, p, p->fda);
	return 0;

no_tmo:
	p->transport->close(&p->fda);
no_tropen:
	close(fd2);
no_timer2:
	close(fd1);
no_timer1:
	return -1;
}

/*
 * Returns non-zero if the announce message is different than last.
 */
static int update_current_master(struct port *p, struct ptp_message *m)
{
	struct foreign_clock *fc = p->best;
	struct ptp_message *tmp;

	if (!msg_source_equal(m, fc))
		return add_foreign_master(p, m);

	port_set_announce_tmo(p);
	fc_prune(fc);
	msg_get(m);
	fc->n_messages++;
	TAILQ_INSERT_HEAD(&fc->messages, m, list);
	if (fc->n_messages > 1) {
		tmp = TAILQ_NEXT(m, list);
		return announce_compare(m, tmp);
	}
	return 0;
}

struct dataset *port_best_foreign(struct port *port)
{
	return port->best ? &port->best->dataset : NULL;
}

/* message processing routines */

/*
 * Returns non-zero if the announce message is both qualified and different.
 */
static int process_announce(struct port *p, struct ptp_message *m)
{
	int result = 0;
	switch (p->state) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
		break;
	case PS_LISTENING:
	case PS_PRE_MASTER:
	case PS_MASTER:
	case PS_GRAND_MASTER:
	case PS_PASSIVE:
		result = add_foreign_master(p, m);
		break;
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		result = update_current_master(p, m);
		break;
	}
	return result;
}

static int process_delay_req(struct port *p, struct ptp_message *m)
{
	struct ptp_message *msg;
	int cnt, err = 0, pdulen;

	if (p->state != PS_MASTER && p->state != PS_GRAND_MASTER)
		return -1;

	msg = msg_allocate();
	if (!msg)
		return -1;
	memset(msg, 0, sizeof(*msg));

	pdulen = sizeof(struct delay_resp_msg);
	msg->hwts.type = p->timestamping;

	msg->header.tsmt               = DELAY_RESP;
	msg->header.ver                = PTP_VERSION;
	msg->header.messageLength      = pdulen;
	msg->header.domainNumber       = m->header.domainNumber;
	msg->header.correction         = m->header.correction;
	msg->header.sourcePortIdentity = p->portIdentity;
	msg->header.sequenceId         = m->header.sequenceId;
	msg->header.control            = CTL_DELAY_RESP;
	msg->header.logMessageInterval = p->logMinDelayReqInterval;

	msg->delay_resp.receiveTimestamp.seconds_lsb = htonl(m->hwts.ts.tv_sec);
	msg->delay_resp.receiveTimestamp.seconds_msb = htons(0);
	msg->delay_resp.receiveTimestamp.nanoseconds = htonl(m->hwts.ts.tv_nsec);

	msg->delay_resp.requestingPortIdentity = m->header.sourcePortIdentity;

	if (msg_pre_send(msg)) {
		err = -1;
		goto out;
	}
	cnt = p->transport->send(&p->fda, 0, msg, pdulen, NULL);
	if (cnt <= 0)
		err = -1;
out:
	msg_put(msg);
	return err;
}

static void process_delay_resp(struct port *p, struct ptp_message *m)
{
	struct delay_req_msg *req;
	struct delay_resp_msg *rsp = &m->delay_resp;

	if (!p->delay_req)
		return;

	req = &p->delay_req->delay_req;

	if (p->state != PS_UNCALIBRATED && p->state != PS_SLAVE)
		return;
	if (!pid_eq(&rsp->requestingPortIdentity, &req->hdr.sourcePortIdentity))
		return;
	if (rsp->hdr.sequenceId != ntohs(req->hdr.sequenceId))
		return;

	clock_path_delay(p->clock, p->delay_req->hwts.ts, m->ts.pdu,
			 m->header.correction);

	if (p->logMinDelayReqInterval != rsp->hdr.logMessageInterval) {
		// TODO - validate the input.
		p->logMinDelayReqInterval = rsp->hdr.logMessageInterval;
		pr_info("port %hu: minimum delay request interval 2^%d",
			portnum(p), p->logMinDelayReqInterval);
	}
}

static void process_follow_up(struct port *p, struct ptp_message *m)
{
	struct ptp_message *syn;
	struct PortIdentity master, *pid;
	switch (p->state) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
	case PS_LISTENING:
	case PS_PRE_MASTER:
	case PS_MASTER:
	case PS_GRAND_MASTER:
	case PS_PASSIVE:
		return;
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		break;
	}
	master = clock_parent_identity(p->clock);
	if (memcmp(&master, &m->header.sourcePortIdentity, sizeof(master)))
		return;
	/*
	 * Handle out of order packets. The network stack might
	 * provide the follow up _before_ the sync message. After all,
	 * they can arrive on two different ports. In addition, time
	 * stamping in PHY devices might delay the event packets.
	 */
	syn = p->last_sync;
	if (!syn || syn->header.sequenceId != m->header.sequenceId) {
		if (p->last_follow_up)
			msg_put(p->last_follow_up);
		msg_get(m);
		p->last_follow_up = m;
		return;
	}

	pid = &syn->header.sourcePortIdentity;
	if (memcmp(pid, &m->header.sourcePortIdentity, sizeof(*pid)))
		return;

	clock_synchronize(p->clock, syn->hwts.ts, m->ts.pdu,
			  syn->header.correction, m->header.correction);
}

static void process_sync(struct port *p, struct ptp_message *m)
{
	struct ptp_message *fup;
	struct PortIdentity master;
	switch (p->state) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
	case PS_LISTENING:
	case PS_PRE_MASTER:
	case PS_MASTER:
	case PS_GRAND_MASTER:
	case PS_PASSIVE:
		return;
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		break;
	}
	master = clock_parent_identity(p->clock);
	if (memcmp(&master, &m->header.sourcePortIdentity, sizeof(master))) {
		return;
	}

	// TODO - add asymmetry value to correctionField.

	if (one_step(m)) {
		clock_synchronize(p->clock, m->hwts.ts, m->ts.pdu,
				  m->header.correction, 0);
		return;
	}
	/*
	 * Check if follow up arrived first.
	 */
	fup = p->last_follow_up;
	if (fup && fup->header.sequenceId == m->header.sequenceId) {
		clock_synchronize(p->clock, m->hwts.ts, fup->ts.pdu,
				  m->header.correction, fup->header.correction);
		return;
	}
	/*
	 * Remember this sync for two step operation.
	 */
	if (p->last_sync)
		msg_put(p->last_sync);
	msg_get(m);
	p->last_sync = m;
}

/* public methods */

void port_close(struct port *p)
{
	p->transport->close(&p->fda);
	close(p->fda.fd[FD_ANNOUNCE_TIMER]);
	close(p->fda.fd[FD_DELAY_TIMER]);
	free(p);
}

struct foreign_clock *port_compute_best(struct port *p)
{
	struct foreign_clock *fc;
	struct ptp_message *tmp;

	p->best = NULL;

	LIST_FOREACH(fc, &p->foreign_masters, list) {
		tmp = TAILQ_FIRST(&fc->messages);
		if (!tmp)
			continue;

		fc_prune(fc);

		if (fc->n_messages < FOREIGN_MASTER_THRESHOLD)
			continue;

		announce_to_dataset(tmp, p->clock, &fc->dataset);

		if (!p->best)
			p->best = fc;
		else if (dscmp(&fc->dataset, &p->best->dataset) > 0)
			p->best = fc;
		else
			fc_clear(fc);
	}

	return p->best;
}

void port_dispatch(struct port *p, enum fsm_event event)
{
	enum port_state next = ptp_fsm(p->state, event);

	if (PS_INITIALIZING == next) {
		/*
		 * This is a special case. Since we initialize the
		 * port immediately, we can skip right to listening
		 * state if all goes well.
		 */
		p->state = port_initialize(p) ? PS_FAULTY : PS_LISTENING;
		return;
	}

	if (next == p->state)
		return;

	pr_info("port %hu: %s to %s on %s", portnum(p),
		ps_str[p->state], ps_str[next], ev_str[event]);

	switch (next) {
	case PS_INITIALIZING:
	case PS_FAULTY:
	case PS_DISABLED:
		port_clr_tmo(p->fda.fd[FD_ANNOUNCE_TIMER]);
		port_clr_tmo(p->fda.fd[FD_DELAY_TIMER]);
		break;
	case PS_LISTENING:
		port_set_announce_tmo(p);
		port_clr_tmo(p->fda.fd[FD_DELAY_TIMER]);
		break;
	case PS_PRE_MASTER:
	case PS_MASTER:
	case PS_GRAND_MASTER:
		port_clr_tmo(p->fda.fd[FD_ANNOUNCE_TIMER]);
		port_clr_tmo(p->fda.fd[FD_DELAY_TIMER]);
		break;
	case PS_PASSIVE:
		port_set_announce_tmo(p);
		port_clr_tmo(p->fda.fd[FD_DELAY_TIMER]);
		break;
	case PS_UNCALIBRATED:
	case PS_SLAVE:
		port_set_announce_tmo(p);
		port_set_delay_tmo(p);
		break;
	};
	p->state = next;
}

enum fsm_event port_event(struct port *p, int fd_index)
{
	enum fsm_event event = EV_NONE;
	struct ptp_message *msg;
	int cnt, fd = p->fda.fd[fd_index];

	switch (fd_index) {
	case FD_ANNOUNCE_TIMER:
		pr_debug("port %hu: announce timeout", portnum(p));
		if (p->best)
			fc_clear(p->best);
		port_set_announce_tmo(p);
		return EV_ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES;

	case FD_DELAY_TIMER:
		pr_debug("port %hu: delay timeout", portnum(p));
		port_set_delay_tmo(p);
		return port_delay_request(p) ? EV_FAULT_DETECTED : EV_NONE;
	}

	msg = msg_allocate();
	if (!msg)
		return EV_FAULT_DETECTED;

	msg->hwts.type = p->timestamping;

	cnt = p->transport->recv(fd, msg, sizeof(*msg), &msg->hwts);
	if (cnt <= 0)
		return EV_FAULT_DETECTED;

	if (msg_post_recv(msg, cnt)) {
		pr_err("port %hu: bad message", portnum(p));
		return EV_NONE;
	}

	switch (msg_type(msg)) {
	case SYNC:
		process_sync(p, msg);
		break;
	case DELAY_REQ:
		process_delay_req(p, msg);
		break;
	case PDELAY_REQ:
	case PDELAY_RESP:
		break;
	case FOLLOW_UP:
		process_follow_up(p, msg);
		break;
	case DELAY_RESP:
		process_delay_resp(p, msg);
		break;
	case PDELAY_RESP_FOLLOW_UP:
		break;
	case ANNOUNCE:
		if (process_announce(p, msg))
			event = EV_STATE_DECISION_EVENT;
		break;
	case SIGNALING:
	case MANAGEMENT:
		break;
	}

	msg_put(msg);
	return event;
}

struct port *port_open(char *name,
		       enum transport_type transport,
		       enum timestamp_type timestamping,
		       int number,
		       enum delay_mechanism dm,
		       struct clock *clock)
{
	struct port *p = malloc(sizeof(*p));
	if (!p)
		return NULL;

	memset(p, 0, sizeof(*p));

	p->name = name;
	p->clock = clock;
	p->transport = transport_find(transport);
	if (!p->transport) {
		free(p);
		return NULL;
	}
	p->timestamping = timestamping;
	p->portIdentity.clockIdentity = clock_identity(clock);
	p->portIdentity.portNumber = number;
	p->state = PS_INITIALIZING;
	p->delayMechanism = dm;
	p->versionNumber = PTP_VERSION;

	return p;
}

enum port_state port_state(struct port *port)
{
	return port->state;
}
