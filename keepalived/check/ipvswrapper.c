/* 
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 * 
 * Part:        IPVS Kernel wrapper. Use setsockopt call to add/remove
 *              server to/from the loadbalanced server pool.
 *  
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *              
 *               This program is distributed in the hope that it will be useful,
 *               but WITHOUT ANY WARRANTY; without even the implied warranty of
 *               MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *               See the GNU General Public License for more details.
 *
 *               This program is free software; you can redistribute it and/or
 *               modify it under the terms of the GNU General Public License
 *               as published by the Free Software Foundation; either version
 *               2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2010 Alexandre Cassen, <acassen@freebox.fr>
 */

#include "ipvswrapper.h"
#include "check_data.h"
#include "list.h"
#include "utils.h"
#include "memory.h"
#include "logger.h"

/* local helpers functions */
static int parse_timeout(char *, unsigned *);
static int string_to_number(const char *, int, int);
static int modprobe_ipvs(void);

/* fetch virtual server group from group name */
virtual_server_group *
ipvs_get_group_by_name(char *gname, list l)
{
	element e;
	virtual_server_group *vsg;

	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vsg = ELEMENT_DATA(e);
		if (!strcmp(vsg->gname, gname))
			return vsg;
	}
	return NULL;
}

#ifdef _KRNL_2_4_			/* KERNEL 2.4 IPVS handling */

/* Global module def IPVS rules */
static struct ip_vs_rule_user *urule;

/* Initialization helpers */
int
ipvs_start(void)
{
	log_message(LOG_DEBUG, "Initializing ipvs 2.4");
	/* Init IPVS kernel channel */
	if (ipvs_init()) {
		/* try to insmod the ip_vs module if ipvs_init failed */
		if (modprobe_ipvs() || ipvs_init()) {
			log_message(LOG_INFO,
			       "IPVS : Can't initialize ipvs: %s",
		 	       ipvs_strerror(errno));
			return IPVS_ERROR;
		}
	}

	/* Allocate global user rules */
	urule = (struct ip_vs_rule_user *) MALLOC(sizeof (struct ip_vs_rule_user));
	return IPVS_SUCCESS;
}

void
ipvs_stop(void)
{
	/* Clean up the room */
	FREE(urule);
	ipvs_close();
}

static int
ipvs_talk(int cmd)
{
	int result;
	if (result = ipvs_command(cmd, urule))
		if ((cmd == IP_VS_SO_SET_EDITDEST) &&
		    (errno == ENOENT))
			result = ipvs_command(IP_VS_SO_SET_ADDDEST, urule);
	if (result)
		log_message(LOG_INFO, "IPVS : %s", ipvs_strerror(errno));
	return IPVS_SUCCESS;
}

int
ipvs_syncd_cmd(int cmd, char *ifname, int state, int syncid)
{
#ifdef _HAVE_IPVS_SYNCD_

	memset(urule, 0, sizeof (struct ip_vs_rule_user));

	/* prepare user rule */
	urule->state = state;
	urule->syncid = syncid;
	if (ifname != NULL)
		strncpy(urule->mcast_ifn, ifname, IP_VS_IFNAME_MAXLEN);

	/* Talk to the IPVS channel */
	return ipvs_talk(cmd);

#else
	log_message(LOG_INFO, "IPVS : Sync daemon not supported");
	return IPVS_ERROR;
#endif
}

/* IPVS group range rule */
static int
ipvs_group_range_cmd(int cmd, virtual_server_group_entry *vsg_entry)
{
	uint32_t addr_ip;
	int err = 0;

	/* Parse the whole range */
	for (addr_ip = SVR_IP(vsg_entry);
	     ((addr_ip >> 24) & 0xFF) <= vsg_entry->range;
	     addr_ip += 0x01000000) {
		urule->vaddr = addr_ip;
		urule->vport = SVR_PORT(vsg_entry);

		/* Talk to the IPVS channel */
		err = ipvs_talk(cmd);
	}

	return err;
}

/* set IPVS group rules */
static int
ipvs_group_cmd(int cmd, list vs_group, real_server * rs, char * vsgname)
{
	virtual_server_group *vsg = ipvs_get_group_by_name(vsgname, vs_group);
	virtual_server_group_entry *vsg_entry;
	list l;
	element e;
	int err = 1;

	/* return if jointure fails */
	if (!vsg) return -1;

	/* visit addr_ip list */
	l = vsg->addr_ip;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vsg_entry = ELEMENT_DATA(e);
		urule->vaddr = SVR_IP(vsg_entry);
		urule->vport = SVR_PORT(vsg_entry);

		/* Talk to the IPVS channel */
		if (IPVS_ALIVE(cmd, vsg_entry, rs)) {
			err = ipvs_talk(cmd);
			IPVS_SET_ALIVE(cmd, vsg_entry);
		}
	}

	/* visit vfwmark list */
	l = vsg->vfwmark;
	urule->vaddr = 0;
	urule->vport = 0;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vsg_entry = ELEMENT_DATA(e);
		urule->vfwmark = vsg_entry->vfwmark;

		/* Talk to the IPVS channel */
		if (IPVS_ALIVE(cmd, vsg_entry, rs)) {
			err = ipvs_talk(cmd);
			IPVS_SET_ALIVE(cmd, vsg_entry);
		}
	}

	/* visit range list */
	l = vsg->range;
	urule->vfwmark = 0;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vsg_entry = ELEMENT_DATA(e);

		/* Talk to the IPVS channel */
		if (IPVS_ALIVE(cmd, vsg_entry, rs)) {
			err = ipvs_group_range_cmd(cmd, vsg_entry);
			IPVS_SET_ALIVE(cmd, vsg_entry);
		}
	}

	return err;
}

/* Fill IPVS rule with root vs infos */
void
ipvs_set_rule(int cmd, virtual_server * vs, real_server * rs)
{
	/* Clean up target rule */
	memset(urule, 0, sizeof (struct ip_vs_rule_user));

	strncpy(urule->sched_name, vs->sched, IP_VS_SCHEDNAME_MAXLEN);
	urule->weight = 1;
	urule->conn_flags = vs->loadbalancing_kind;
	urule->netmask = ((u_int32_t) 0xffffffff);
	urule->protocol = vs->service_type;

	if (!parse_timeout(vs->timeout_persistence, &urule->timeout))
		log_message(LOG_INFO,
		       "IPVS : Virtual service [%s:%d] illegal timeout.",
		       inet_ntop2(SVR_IP(vs)), ntohs(SVR_PORT(vs)));

	if (urule->timeout != 0 || vs->granularity_persistence)
		urule->vs_flags = IP_VS_SVC_F_PERSISTENT;

	if (cmd == IP_VS_SO_SET_ADD || cmd == IP_VS_SO_SET_DEL)
		if (vs->granularity_persistence)
			urule->netmask = vs->granularity_persistence;

	/* SVR specific */
	if (rs) {
		if (cmd == IP_VS_SO_SET_ADDDEST
		    || cmd == IP_VS_SO_SET_DELDEST
		    || cmd == IP_VS_SO_SET_EDITDEST) {
			urule->weight = rs->weight;
			urule->daddr = SVR_IP(rs);
			urule->dport = SVR_PORT(rs);
		}
	}
}

/* Set/Remove a RS from a VS */
int
ipvs_cmd(int cmd, list vs_group, virtual_server * vs, real_server * rs)
{
	int err = 0;

	/* Prepare target rule */
	ipvs_set_rule(cmd, vs, rs);

	/* Does the service use inhibit flag ? */
	if (cmd == IP_VS_SO_SET_DELDEST && rs->inhibit) {
		urule->weight = 0;
		cmd = IP_VS_SO_SET_EDITDEST;
	}
	if (cmd == IP_VS_SO_SET_ADDDEST && rs->inhibit && rs->set)
		cmd = IP_VS_SO_SET_EDITDEST;

	/* Set flag */
	if (cmd == IP_VS_SO_SET_ADDDEST && !rs->set)
		rs->set = 1;
	if (cmd == IP_VS_SO_SET_DELDEST && rs->set)
		rs->set = 0;

	/* Set vs rule and send to kernel */
	if (vs->vsgname) {
		err = ipvs_group_cmd(cmd, vs_group, rs, vs->vsgname);
	} else {
		if (vs->vfwmark) {
			urule->vfwmark = vs->vfwmark;
		} else {
			urule->vaddr = SVR_IP(vs);
			urule->vport = SVR_PORT(vs);
		}

		/* Talk to the IPVS channel */
		err = ipvs_talk(cmd);
	}

	return err;
}

/* Remove a specific vs group entry */
int
ipvs_group_remove_entry(virtual_server *vs, virtual_server_group_entry *vsge)
{
	real_server *rs;
	int err = 0;
	element e;
	list l = vs->rs;

	/* Clean target rules */
	memset(urule, 0, sizeof (struct ip_vs_rule_user));

	/* Process realserver queue */
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		rs = ELEMENT_DATA(e);

		if (rs->alive) {
			/* Prepare the IPVS rule */
			if (urule->daddr) {
				/* Setting IPVS rule with vs root rs */
				ipvs_set_rule(IP_VS_SO_SET_DELDEST, vs, rs);
			} else {
				urule->weight = rs->weight;
				urule->daddr = SVR_IP(rs);
				urule->dport = SVR_PORT(rs);
			}

			/* Set vs rule */
			if (vsge->range) {
				ipvs_group_range_cmd(IP_VS_SO_SET_DELDEST, vsge);
			} else {
				urule->vfwmark = vsge->vfwmark;
				urule->vaddr = SVR_IP(vsge);
				urule->vport = SVR_PORT(vsge);

				/* Talk to the IPVS channel */
				err = ipvs_talk(IP_VS_SO_SET_DELDEST);
			}
		}
	}

	/* Remove VS entry */
	if (vsge->range)
		err = ipvs_group_range_cmd(IP_VS_SO_SET_DEL, vsge);
	else
		err = ipvs_talk(IP_VS_SO_SET_DEL);
	return err;
}

#else					/* KERNEL 2.6 IPVS handling */

/* Global module def IPVS rules */
static struct ip_vs_service_user *srule;
static struct ip_vs_dest_user *drule;
static struct ip_vs_daemon_user *daemonrule;

/* Initialization helpers */
int
ipvs_start(void)
{
	log_message(LOG_DEBUG, "Initializing ipvs 2.6");
	/* Initialize IPVS module */
	if (ipvs_init()) {
		if (modprobe_ipvs() || ipvs_init()) {
			log_message(LOG_INFO, "IPVS: Can't initialize ipvs: %s",
			       ipvs_strerror(errno));
			return IPVS_ERROR;
		}
	}

	/* Allocate global user rules */
	srule = (struct ip_vs_service_user *) MALLOC(sizeof (struct ip_vs_service_user));
	drule = (struct ip_vs_dest_user *) MALLOC(sizeof (struct ip_vs_dest_user));
	daemonrule = (struct ip_vs_daemon_user *) MALLOC(sizeof (struct ip_vs_daemon_user));
	return IPVS_SUCCESS;
}

void
ipvs_stop(void)
{
	/* Clean up the room */
	FREE(srule);
	FREE(drule);
	FREE(daemonrule);
	ipvs_close();
}

/* Send user rules to IPVS module */
static void
ipvs_talk(int cmd)
{
	int result = -1;

	switch (cmd) {
		case IP_VS_SO_SET_STARTDAEMON:
			result = ipvs_start_daemon(daemonrule);
			break;
		case IP_VS_SO_SET_STOPDAEMON:
			result = ipvs_stop_daemon(daemonrule);
			break;
		case IP_VS_SO_SET_ADD:
			result = ipvs_add_service(srule);
			break;
		case IP_VS_SO_SET_DEL:
			result = ipvs_del_service(srule);
			break;
		case IP_VS_SO_SET_EDIT:
			result = ipvs_update_service(srule);
			break;
		case IP_VS_SO_SET_ZERO:
			result = ipvs_zero_service(srule);
			break;
		case IP_VS_SO_SET_ADDDEST:
			result = ipvs_add_dest(srule, drule);
			break;
		case IP_VS_SO_SET_DELDEST:
			result = ipvs_del_dest(srule, drule);
			break;
		case IP_VS_SO_SET_EDITDEST:
			if ((result = ipvs_update_dest(srule, drule)) &&
			    (errno == ENOENT))
				result = ipvs_add_dest(srule, drule);
			break;
	}

	if (result)
		log_message(LOG_INFO, "IPVS: %s", ipvs_strerror(errno));
}

int
ipvs_syncd_cmd(int cmd, char *ifname, int state, int syncid)
{
	memset(daemonrule, 0, sizeof (struct ip_vs_daemon_user));

	/* prepare user rule */
	daemonrule->state = state;
	daemonrule->syncid = syncid;
	if (ifname != NULL)
		strncpy(daemonrule->mcast_ifn, ifname, IP_VS_IFNAME_MAXLEN);

	/* Talk to the IPVS channel */
	ipvs_talk(cmd);
	return IPVS_SUCCESS;
}

/* IPVS group range rule */
static void
ipvs_group_range_cmd(int cmd, virtual_server_group_entry *vsg_entry)
{
	uint32_t addr_ip;

	/* Parse the whole range */
	for (addr_ip = SVR_IP(vsg_entry);
	     ((addr_ip >> 24) & 0xFF) <= vsg_entry->range;
	     addr_ip += 0x01000000) {
		srule->addr = addr_ip;
		srule->port = SVR_PORT(vsg_entry);

		/* Talk to the IPVS channel */
		ipvs_talk(cmd);
	}
}

/* set IPVS group rules */
static void
ipvs_group_cmd(int cmd, list vs_group, real_server * rs, char * vsgname)
{
	virtual_server_group *vsg = ipvs_get_group_by_name(vsgname, vs_group);
	virtual_server_group_entry *vsg_entry;
	list l;
	element e;

	/* return if jointure fails */
	if (!vsg) return;

	/* visit addr_ip list */
	l = vsg->addr_ip;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vsg_entry = ELEMENT_DATA(e);
		srule->addr = SVR_IP(vsg_entry);
		srule->port = SVR_PORT(vsg_entry);

		/* Talk to the IPVS channel */
		if (IPVS_ALIVE(cmd, vsg_entry, rs)) {
			ipvs_talk(cmd);
			IPVS_SET_ALIVE(cmd, vsg_entry);
		}
	}

	/* visit vfwmark list */
	l = vsg->vfwmark;
	srule->addr = 0;
	srule->port = 0;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vsg_entry = ELEMENT_DATA(e);
		srule->fwmark = vsg_entry->vfwmark;

		/* Talk to the IPVS channel */
		if (IPVS_ALIVE(cmd, vsg_entry, rs)) {
			ipvs_talk(cmd);
			IPVS_SET_ALIVE(cmd, vsg_entry);
		}
	}

	/* visit range list */
	l = vsg->range;
	srule->fwmark = 0;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vsg_entry = ELEMENT_DATA(e);

		/* Talk to the IPVS channel */
		if (IPVS_ALIVE(cmd, vsg_entry, rs)) {
			ipvs_group_range_cmd(cmd, vsg_entry);
			IPVS_SET_ALIVE(cmd, vsg_entry);
		}
	}
}

/* Fill IPVS rule with root vs infos */
void
ipvs_set_rule(int cmd, virtual_server * vs, real_server * rs)
{
	/* Clean target rule */
	memset(drule, 0, sizeof (struct ip_vs_dest_user));

	drule->weight = 1;
	drule->u_threshold = 0;
	drule->l_threshold = 0;
	drule->conn_flags = vs->loadbalancing_kind;
	strncpy(srule->sched_name, vs->sched, IP_VS_SCHEDNAME_MAXLEN);
	srule->netmask = ((u_int32_t) 0xffffffff);
	srule->protocol = vs->service_type;

	if (!parse_timeout(vs->timeout_persistence, &srule->timeout))
		log_message(LOG_INFO,
		       "IPVS : Virtual service [%s:%d] illegal timeout.",
		       inet_ntop2(SVR_IP(vs)), ntohs(SVR_PORT(vs)));

	if (srule->timeout != 0 || vs->granularity_persistence)
		srule->flags = IP_VS_SVC_F_PERSISTENT;

	if (cmd == IP_VS_SO_SET_ADD || cmd == IP_VS_SO_SET_DEL)
		if (vs->granularity_persistence)
			srule->netmask = vs->granularity_persistence;

	/* SVR specific */
	if (rs) {
		if (cmd == IP_VS_SO_SET_ADDDEST
		    || cmd == IP_VS_SO_SET_DELDEST
		    || cmd == IP_VS_SO_SET_EDITDEST) {
			drule->weight = rs->weight;
			drule->addr = SVR_IP(rs);
			drule->port = SVR_PORT(rs);
			drule->u_threshold = rs->u_threshold;
			drule->l_threshold = rs->l_threshold;
		}
	}
}

/* Set/Remove a RS from a VS */
int
ipvs_cmd(int cmd, list vs_group, virtual_server * vs, real_server * rs)
{
	/* Allocate the room */
	memset(srule, 0, sizeof (struct ip_vs_service_user));
	ipvs_set_rule(cmd, vs, rs);

	/* Does the service use inhibit flag ? */
	if (cmd == IP_VS_SO_SET_DELDEST && rs->inhibit) {
		drule->weight = 0;
		cmd = IP_VS_SO_SET_EDITDEST;
	}
	if (cmd == IP_VS_SO_SET_ADDDEST && rs->inhibit && rs->set)
		cmd = IP_VS_SO_SET_EDITDEST;

	/* Set flag */
	if (cmd == IP_VS_SO_SET_ADDDEST && !rs->set)
		rs->set = 1;
	if (cmd == IP_VS_SO_SET_DELDEST && rs->set)
		rs->set = 0;

	/* Set vs rule and send to kernel */
	if (vs->vsgname) {
		ipvs_group_cmd(cmd, vs_group, rs, vs->vsgname);
	} else {
		if (vs->vfwmark) {
			srule->fwmark = vs->vfwmark;
		} else {
			srule->addr = SVR_IP(vs);
			srule->port = SVR_PORT(vs);
		}

		/* Talk to the IPVS channel */
		ipvs_talk(cmd);
	}

	return IPVS_SUCCESS;
}

/* Remove a specific vs group entry */
int
ipvs_group_remove_entry(virtual_server *vs, virtual_server_group_entry *vsge)
{
	real_server *rs;
	element e;
	list l = vs->rs;

	/* Clean target rules */
	memset(srule, 0, sizeof (struct ip_vs_service_user));
	memset(drule, 0, sizeof (struct ip_vs_dest_user));

	/* Process realserver queue */
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		rs = ELEMENT_DATA(e);

		if (rs->alive) {
			/* Prepare the IPVS rule */
			if (!drule->addr) {
				/* Setting IPVS rule with vs root rs */
				ipvs_set_rule(IP_VS_SO_SET_DELDEST, vs, rs);
			} else {
				drule->weight = rs->weight;
				drule->addr = SVR_IP(rs);
				drule->port = SVR_PORT(rs);
			}

			/* Set vs rule */
			if (vsge->range) {
				ipvs_group_range_cmd(IP_VS_SO_SET_DELDEST, vsge);
			} else {
				srule->fwmark = vsge->vfwmark;
				srule->addr = SVR_IP(vsge);
				srule->port = SVR_PORT(vsge);
				drule->u_threshold = rs->u_threshold;
				drule->l_threshold = rs->l_threshold;

				/* Talk to the IPVS channel */
				ipvs_talk(IP_VS_SO_SET_DELDEST);
			}
		}
	}

	/* Remove VS entry */
	if (vsge->range)
		ipvs_group_range_cmd(IP_VS_SO_SET_DEL, vsge);
	else
		ipvs_talk(IP_VS_SO_SET_DEL);
	return IPVS_SUCCESS;
}

#ifdef _WITH_SNMP_
/* Update statistics for a given virtual server. This includes
   statistics of real servers. The update is only done if we need
   refreshing. */
void
ipvs_update_stats(virtual_server *vs)
{
	element e, ge = NULL;
	real_server *rs;
	virtual_server_group *vsg = NULL;
	virtual_server_group_entry *vsg_entry = NULL;
	uint32_t addr_ip = 0;
	ipvs_service_entry_t * serv = NULL;
	struct ip_vs_get_dests * dests = NULL;
	int i;
#define UPDATE_STATS_INIT 1
#define UPDATE_STATS_VSG_IP 2
#define UPDATE_STATS_VSG_FWMARK 4
#define UPDATE_STATS_VSG_RANGE 6
#define UPDATE_STATS_VSG_RANGE_IP 7
#define UPDATE_STATS_END 99
	int state = UPDATE_STATS_INIT;

	if (time(NULL) - vs->lastupdated < STATS_REFRESH)
		return;
	vs->lastupdated = time(NULL);
	/* Reset stats */
	memset(&vs->stats, 0, sizeof(vs->stats));
	if (vs->s_svr) {
		memset(&vs->s_svr->stats, 0, sizeof(vs->s_svr->stats));
		vs->s_svr->activeconns =
			vs->s_svr->inactconns = vs->s_svr->persistconns = 0;
	}
	if (!LIST_ISEMPTY(vs->rs)) {
		for (e = LIST_HEAD(vs->rs); e; ELEMENT_NEXT(e)) {
			rs = ELEMENT_DATA(e);
			memset(&rs->stats, 0, sizeof(rs->stats));
			rs->activeconns = rs->inactconns = rs->persistconns = 0;
		}
	}
	/* FSM: at each transition, we process "serv" if it is not NULL */
	while (state != UPDATE_STATS_END) {
		serv = NULL;
		switch (state) {
		case UPDATE_STATS_INIT:
			/* We need to know the next state to reach */
			if (vs->vsgname) {
				if (!LIST_ISEMPTY(check_data->vs_group))
					vsg = ipvs_get_group_by_name(vs->vsgname,
								     check_data->vs_group);
				else
					vsg = NULL;
				if (!vsg)
					state = UPDATE_STATS_END;
				else {
					state = UPDATE_STATS_VSG_IP;
					ge = NULL;
				}
				continue;
			} else
				state = UPDATE_STATS_END;
				serv = ipvs_get_service(vs->vfwmark,
							vs->service_type,
							SVR_IP(vs),
							SVR_PORT(vs));
			break;
		case UPDATE_STATS_VSG_IP:
			if (!ge)
				ge = LIST_HEAD(vsg->addr_ip);
			else
				ELEMENT_NEXT(ge);
			if (!ge) {
				state = UPDATE_STATS_VSG_FWMARK;
				continue;
			}
			vsg_entry = ELEMENT_DATA(ge);
			serv = ipvs_get_service(0, vs->service_type,
						SVR_IP(vsg_entry),
						SVR_PORT(vsg_entry));
			break;
		case UPDATE_STATS_VSG_FWMARK:
			if (!ge)
				ge = LIST_HEAD(vsg->vfwmark);
			else
				ELEMENT_NEXT(ge);
			if (!ge) {
				state = UPDATE_STATS_VSG_RANGE;
				continue;
			}
			vsg_entry = ELEMENT_DATA(ge);
			serv = ipvs_get_service(vsg_entry->vfwmark,
						vs->service_type,
						0, 0);
			break;
		case UPDATE_STATS_VSG_RANGE:
			if (!ge)
				ge = LIST_HEAD(vsg->range);
			else
				ELEMENT_NEXT(ge);
			if (!ge) {
				state = UPDATE_STATS_END;
				continue;
			}
			vsg_entry = ELEMENT_DATA(ge);
			addr_ip = SVR_IP(vsg_entry);
			state = UPDATE_STATS_VSG_RANGE_IP;
			continue;
		case UPDATE_STATS_VSG_RANGE_IP:
			if (((addr_ip >> 24) & 0xFF) > vsg_entry->range) {
				state = UPDATE_STATS_VSG_RANGE;
				continue;
			}
			serv = ipvs_get_service(0,
						vs->service_type,
						addr_ip, SVR_PORT(vsg_entry));
			addr_ip += 0x01000000;
			break;
		}
		if (!serv) continue;
		
		/* Update virtual server stats */
#define ADD_TO_VSSTATS(X) vs->stats.X += serv->stats.X;
		ADD_TO_VSSTATS(conns);
		ADD_TO_VSSTATS(inpkts);
		ADD_TO_VSSTATS(outpkts);
		ADD_TO_VSSTATS(inbytes);
		ADD_TO_VSSTATS(outbytes);
		ADD_TO_VSSTATS(cps);
		ADD_TO_VSSTATS(inpps);
		ADD_TO_VSSTATS(outpps);
		ADD_TO_VSSTATS(inbps);
		ADD_TO_VSSTATS(outbps);

		/* Get real servers */
		dests = ipvs_get_dests(serv);
		if (!dests) {
			FREE(serv);
			return;
		}
		for (i = 0; i < dests->num_dests; i++) {
			rs = NULL;
			/* Is it the sorry server? */
			if (vs->s_svr &&
			    (ntohl(dests->entrytable[i].addr) ==
			     ntohl(vs->s_svr->addr_ip)) &&
			    (ntohs(vs->s_svr->addr_port) ==
			     ntohs(dests->entrytable[i].port)))
				rs = vs->s_svr;
			else if (!LIST_ISEMPTY(vs->rs))
				/* Search for a match in the list of real servers */
				for (e = LIST_HEAD(vs->rs); e; ELEMENT_NEXT(e)) {
					rs = ELEMENT_DATA(e);
					if ((ntohl(dests->entrytable[i].addr) ==
					     ntohl(rs->addr_ip)) &&
					    (ntohs(rs->addr_port) ==
					     ntohs(dests->entrytable[i].port)))
						break;
				}
			if (rs) {
#define ADD_TO_RSSTATS(X) rs->X += dests->entrytable[i].X
				ADD_TO_RSSTATS(activeconns);
				ADD_TO_RSSTATS(inactconns);
				ADD_TO_RSSTATS(persistconns);
				ADD_TO_RSSTATS(stats.conns);
				ADD_TO_RSSTATS(stats.inpkts);
				ADD_TO_RSSTATS(stats.outpkts);
				ADD_TO_RSSTATS(stats.inbytes);
				ADD_TO_RSSTATS(stats.outbytes);
				ADD_TO_RSSTATS(stats.cps);
				ADD_TO_RSSTATS(stats.inpps);
				ADD_TO_RSSTATS(stats.outpps);
				ADD_TO_RSSTATS(stats.inbps);
				ADD_TO_RSSTATS(stats.outbps);
			}
		}
		FREE(dests);
		FREE(serv);
	}
}

#endif /* _WITH_SNMP_ */
#endif

/*
 * Common IPVS functions
 */
void
ipvs_syncd_master(char *ifname, int syncid)
{
	ipvs_syncd_cmd(IPVS_STOPDAEMON, ifname, IPVS_BACKUP, syncid);
	ipvs_syncd_cmd(IPVS_STARTDAEMON, ifname, IPVS_MASTER, syncid);
}

void
ipvs_syncd_backup(char *ifname, int syncid)
{
	ipvs_syncd_cmd(IPVS_STOPDAEMON, ifname, IPVS_MASTER, syncid);
	ipvs_syncd_cmd(IPVS_STARTDAEMON, ifname, IPVS_BACKUP, syncid);
}

/*
 * Utility functions coming from Wensong code
 */

static int
parse_timeout(char *buf, unsigned *timeout)
{
	int i;

	if (buf == NULL) {
		*timeout = IP_VS_TEMPLATE_TIMEOUT;
		return 1;
	}

	if ((i = string_to_number(buf, 0, 86400 * 31)) == -1)
		return 0;

	*timeout = i * (IP_VS_TEMPLATE_TIMEOUT / (6*60));
	return 1;
}

static int
string_to_number(const char *s, int min, int max)
{
	int number;
	char *end;

	number = (int) strtol(s, &end, 10);
	if (*end == '\0' && end != s) {
		/*
		 * We parsed a number, let's see if we want this.
		 * If max <= min then ignore ranges
		 */
		if (max <= min || (min <= number && number <= max))
			return number;
		else
			return -1;
	} else
		return -1;
}

static int
modprobe_ipvs(void)
{
	char *argv[] = { "/sbin/modprobe", "-s", "-k", "--", "ip_vs", NULL };
	int child;
	int status;
	int rc;

	if (!(child = fork())) {
		execv(argv[0], argv);
		exit(1);
	}

	rc = waitpid(child, &status, 0);

	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		return 1;
	}

	return 0;
}
