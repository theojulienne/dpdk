/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */
#include <stdlib.h>
#include <stdbool.h>
#include <netinet/in.h>

#include <rte_bitops.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <ethdev_driver.h>
#include <ethdev_vdev.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_ip.h>
#include <rte_ip_frag.h>
#include <rte_devargs.h>
#include <rte_kvargs.h>
#include <bus_vdev_driver.h>
#include <rte_alarm.h>
#include <rte_cycles.h>
#include <rte_string_fns.h>

#include "rte_eth_bond.h"
#include "eth_bond_private.h"
#include "eth_bond_8023ad_private.h"

#define REORDER_PERIOD_MS 10
#define DEFAULT_POLLING_INTERVAL_10_MS (10)
#define BOND_MAX_MAC_ADDRS 16

#define HASH_L4_PORTS(h) ((h)->src_port ^ (h)->dst_port)

/* Table for statistics in mode 5 TLB */
static uint64_t tlb_last_obytets[RTE_MAX_ETHPORTS];

static inline size_t
get_vlan_offset(struct rte_ether_hdr *eth_hdr, uint16_t *proto)
{
	size_t vlan_offset = 0;

	if (rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN) == *proto ||
		rte_cpu_to_be_16(RTE_ETHER_TYPE_QINQ) == *proto) {
		struct rte_vlan_hdr *vlan_hdr =
			(struct rte_vlan_hdr *)(eth_hdr + 1);

		vlan_offset = sizeof(struct rte_vlan_hdr);
		*proto = vlan_hdr->eth_proto;

		if (rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN) == *proto) {
			vlan_hdr = vlan_hdr + 1;
			*proto = vlan_hdr->eth_proto;
			vlan_offset += sizeof(struct rte_vlan_hdr);
		}
	}
	return vlan_offset;
}

static uint16_t
bond_ethdev_rx_burst(void *queue, struct rte_mbuf **bufs, uint16_t nb_pkts)
{
	struct bond_dev_private *internals;

	uint16_t num_rx_total = 0;
	uint16_t member_count;
	uint16_t active_member;
	int i;

	/* Cast to structure, containing bonding device's port id and queue id */
	struct bond_rx_queue *bd_rx_q = (struct bond_rx_queue *)queue;
	internals = bd_rx_q->dev_private;
	member_count = internals->active_member_count;
	active_member = bd_rx_q->active_member;

	for (i = 0; i < member_count && nb_pkts; i++) {
		uint16_t num_rx_member;

		/*
		 * Offset of pointer to *bufs increases as packets are received
		 * from other members.
		 */
		num_rx_member =
			rte_eth_rx_burst(internals->active_members[active_member],
					 bd_rx_q->queue_id,
					 bufs + num_rx_total, nb_pkts);
		num_rx_total += num_rx_member;
		nb_pkts -= num_rx_member;
		if (++active_member >= member_count)
			active_member = 0;
	}

	if (++bd_rx_q->active_member >= member_count)
		bd_rx_q->active_member = 0;
	return num_rx_total;
}

static uint16_t
bond_ethdev_rx_burst_active_backup(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_pkts)
{
	struct bond_dev_private *internals;

	/* Cast to structure, containing bonding device's port id and queue id */
	struct bond_rx_queue *bd_rx_q = (struct bond_rx_queue *)queue;

	internals = bd_rx_q->dev_private;

	return rte_eth_rx_burst(internals->current_primary_port,
			bd_rx_q->queue_id, bufs, nb_pkts);
}

static inline uint8_t
is_lacp_packets(uint16_t ethertype, uint8_t subtype, struct rte_mbuf *mbuf)
{
	const uint16_t ether_type_slow_be =
		rte_be_to_cpu_16(RTE_ETHER_TYPE_SLOW);

	return !((mbuf->ol_flags & RTE_MBUF_F_RX_VLAN) ? mbuf->vlan_tci : 0) &&
		(ethertype == ether_type_slow_be &&
		(subtype == SLOW_SUBTYPE_MARKER || subtype == SLOW_SUBTYPE_LACP));
}

/*****************************************************************************
 * Flow director's setup for mode 4 optimization
 */

static struct rte_flow_item_eth flow_item_eth_type_8023ad = {
	.hdr.dst_addr.addr_bytes = { 0 },
	.hdr.src_addr.addr_bytes = { 0 },
	.hdr.ether_type = RTE_BE16(RTE_ETHER_TYPE_SLOW),
};

static struct rte_flow_item_eth flow_item_eth_mask_type_8023ad = {
	.hdr.dst_addr.addr_bytes = { 0 },
	.hdr.src_addr.addr_bytes = { 0 },
	.hdr.ether_type = 0xFFFF,
};

static struct rte_flow_item flow_item_8023ad[] = {
	{
		.type = RTE_FLOW_ITEM_TYPE_ETH,
		.spec = &flow_item_eth_type_8023ad,
		.last = NULL,
		.mask = &flow_item_eth_mask_type_8023ad,
	},
	{
		.type = RTE_FLOW_ITEM_TYPE_END,
		.spec = NULL,
		.last = NULL,
		.mask = NULL,
	}
};

const struct rte_flow_attr flow_attr_8023ad = {
	.group = 0,
	.priority = 0,
	.ingress = 1,
	.egress = 0,
	.reserved = 0,
};

int
bond_ethdev_8023ad_flow_verify(struct rte_eth_dev *bond_dev,
		uint16_t member_port) {
	struct rte_eth_dev_info member_info;
	struct rte_flow_error error;
	struct bond_dev_private *internals = bond_dev->data->dev_private;

	const struct rte_flow_action_queue lacp_queue_conf = {
		.index = 0,
	};

	const struct rte_flow_action actions[] = {
		{
			.type = RTE_FLOW_ACTION_TYPE_QUEUE,
			.conf = &lacp_queue_conf
		},
		{
			.type = RTE_FLOW_ACTION_TYPE_END,
		}
	};

	int ret = rte_flow_validate(member_port, &flow_attr_8023ad,
			flow_item_8023ad, actions, &error);
	if (ret < 0) {
		RTE_BOND_LOG(ERR, "%s: %s (member_port=%d queue_id=%d)",
				__func__, error.message, member_port,
				internals->mode4.dedicated_queues.rx_qid);
		return -1;
	}

	ret = rte_eth_dev_info_get(member_port, &member_info);
	if (ret != 0) {
		RTE_BOND_LOG(ERR,
			"%s: Error during getting device (port %u) info: %s",
			__func__, member_port, strerror(-ret));

		return ret;
	}

	if (member_info.max_rx_queues < bond_dev->data->nb_rx_queues ||
			member_info.max_tx_queues < bond_dev->data->nb_tx_queues) {
		RTE_BOND_LOG(ERR,
			"%s: Member %d capabilities doesn't allow allocating additional queues",
			__func__, member_port);
		return -1;
	}

	return 0;
}

int
bond_8023ad_slow_pkt_hw_filter_supported(uint16_t port_id) {
	struct rte_eth_dev *bond_dev = &rte_eth_devices[port_id];
	struct bond_dev_private *internals = bond_dev->data->dev_private;
	struct rte_eth_dev_info bond_info;
	uint16_t idx;
	int ret;

	/* Verify if all members in bonding supports flow director and */
	if (internals->member_count > 0) {
		ret = rte_eth_dev_info_get(bond_dev->data->port_id, &bond_info);
		if (ret != 0) {
			RTE_BOND_LOG(ERR,
				"%s: Error during getting device (port %u) info: %s",
				__func__, bond_dev->data->port_id,
				strerror(-ret));

			return ret;
		}

		internals->mode4.dedicated_queues.rx_qid = bond_info.nb_rx_queues;
		internals->mode4.dedicated_queues.tx_qid = bond_info.nb_tx_queues;

		for (idx = 0; idx < internals->member_count; idx++) {
			if (bond_ethdev_8023ad_flow_verify(bond_dev,
					internals->members[idx].port_id) != 0)
				return -1;
		}
	}

	return 0;
}

int
bond_ethdev_8023ad_flow_set(struct rte_eth_dev *bond_dev, uint16_t member_port) {

	struct rte_flow_error error;
	struct bond_dev_private *internals = bond_dev->data->dev_private;
	struct rte_flow_action_queue lacp_queue_conf = {
		.index = internals->mode4.dedicated_queues.rx_qid,
	};

	const struct rte_flow_action actions[] = {
		{
			.type = RTE_FLOW_ACTION_TYPE_QUEUE,
			.conf = &lacp_queue_conf
		},
		{
			.type = RTE_FLOW_ACTION_TYPE_END,
		}
	};

	internals->mode4.dedicated_queues.flow[member_port] = rte_flow_create(member_port,
			&flow_attr_8023ad, flow_item_8023ad, actions, &error);
	if (internals->mode4.dedicated_queues.flow[member_port] == NULL) {
		RTE_BOND_LOG(ERR, "bond_ethdev_8023ad_flow_set: %s "
				"(member_port=%d queue_id=%d)",
				error.message, member_port,
				internals->mode4.dedicated_queues.rx_qid);
		return -1;
	}

	return 0;
}

static bool
is_bond_mac_addr(const struct rte_ether_addr *ea,
		 const struct rte_ether_addr *mac_addrs, uint32_t max_mac_addrs)
{
	uint32_t i;

	for (i = 0; i < max_mac_addrs; i++) {
		/* skip zero address */
		if (rte_is_zero_ether_addr(&mac_addrs[i]))
			continue;

		if (rte_is_same_ether_addr(ea, &mac_addrs[i]))
			return true;
	}

	return false;
}

static inline uint16_t
rx_burst_8023ad(void *queue, struct rte_mbuf **bufs, uint16_t nb_pkts,
		bool dedicated_rxq)
{
	/* Cast to structure, containing bonding device's port id and queue id */
	struct bond_rx_queue *bd_rx_q = (struct bond_rx_queue *)queue;
	struct bond_dev_private *internals = bd_rx_q->dev_private;
	struct rte_eth_dev *bonding_eth_dev =
					&rte_eth_devices[internals->port_id];
	struct rte_ether_addr *bond_mac = bonding_eth_dev->data->mac_addrs;
	struct rte_ether_hdr *hdr;

	const uint16_t ether_type_slow_be =
		rte_be_to_cpu_16(RTE_ETHER_TYPE_SLOW);
	uint16_t num_rx_total = 0;	/* Total number of received packets */
	uint16_t members[RTE_MAX_ETHPORTS];
	uint16_t member_count, idx;

	uint8_t collecting;  /* current member collecting status */
	const uint8_t promisc = rte_eth_promiscuous_get(internals->port_id);
	const uint8_t allmulti = rte_eth_allmulticast_get(internals->port_id);
	uint8_t subtype;
	uint16_t i;
	uint16_t j;
	uint16_t k;

	/* Copy member list to protect against member up/down changes during tx
	 * bursting */
	member_count = internals->active_member_count;
	memcpy(members, internals->active_members,
			sizeof(internals->active_members[0]) * member_count);

	idx = bd_rx_q->active_member;
	if (idx >= member_count) {
		bd_rx_q->active_member = 0;
		idx = 0;
	}
	for (i = 0; i < member_count && num_rx_total < nb_pkts; i++) {
		j = num_rx_total;
		collecting = ACTOR_STATE(&bond_mode_8023ad_ports[members[idx]],
					 COLLECTING);

		/* Read packets from this member */
		num_rx_total += rte_eth_rx_burst(members[idx], bd_rx_q->queue_id,
				&bufs[num_rx_total], nb_pkts - num_rx_total);

		for (k = j; k < 2 && k < num_rx_total; k++)
			rte_prefetch0(rte_pktmbuf_mtod(bufs[k], void *));

		/* Handle slow protocol packets. */
		while (j < num_rx_total) {
			if (j + 3 < num_rx_total)
				rte_prefetch0(rte_pktmbuf_mtod(bufs[j + 3], void *));

			hdr = rte_pktmbuf_mtod(bufs[j], struct rte_ether_hdr *);
			subtype = ((struct slow_protocol_frame *)hdr)->slow_protocol.subtype;

			/* Remove packet from array if:
			 * - it is slow packet but no dedicated rxq is present,
			 * - member is not in collecting state,
			 * - bonding interface is not in promiscuous mode and
			 *   packet address isn't in mac_addrs array:
			 *   - packet is unicast,
			 *   - packet is multicast and bonding interface
			 *     is not in allmulti,
			 */
			if (unlikely(
				(!dedicated_rxq &&
				 is_lacp_packets(hdr->ether_type, subtype,
						 bufs[j])) ||
				!collecting ||
				(!promisc &&
				 !is_bond_mac_addr(&hdr->dst_addr, bond_mac,
						   BOND_MAX_MAC_ADDRS) &&
				 (rte_is_unicast_ether_addr(&hdr->dst_addr) ||
				  !allmulti)))) {
				if (hdr->ether_type == ether_type_slow_be) {
					bond_mode_8023ad_handle_slow_pkt(
					    internals, members[idx], bufs[j]);
				} else
					rte_pktmbuf_free(bufs[j]);

				/* Packet is managed by mode 4 or dropped, shift the array */
				num_rx_total--;
				if (j < num_rx_total) {
					memmove(&bufs[j], &bufs[j + 1], sizeof(bufs[0]) *
						(num_rx_total - j));
				}
			} else
				j++;
		}
		if (unlikely(++idx == member_count))
			idx = 0;
	}

	if (++bd_rx_q->active_member >= member_count)
		bd_rx_q->active_member = 0;

	return num_rx_total;
}

static uint16_t
bond_ethdev_rx_burst_8023ad(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_pkts)
{
	return rx_burst_8023ad(queue, bufs, nb_pkts, false);
}

static uint16_t
bond_ethdev_rx_burst_8023ad_fast_queue(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_pkts)
{
	return rx_burst_8023ad(queue, bufs, nb_pkts, true);
}

#if defined(RTE_LIBRTE_BOND_DEBUG_ALB) || defined(RTE_LIBRTE_BOND_DEBUG_ALB_L1)
uint32_t burstnumberRX;
uint32_t burst_number_TX;

#ifdef RTE_LIBRTE_BOND_DEBUG_ALB

static void
arp_op_name(uint16_t arp_op, char *buf, size_t buf_len)
{
	switch (arp_op) {
	case RTE_ARP_OP_REQUEST:
		strlcpy(buf, "ARP Request", buf_len);
		return;
	case RTE_ARP_OP_REPLY:
		strlcpy(buf, "ARP Reply", buf_len);
		return;
	case RTE_ARP_OP_REVREQUEST:
		strlcpy(buf, "Reverse ARP Request", buf_len);
		return;
	case RTE_ARP_OP_REVREPLY:
		strlcpy(buf, "Reverse ARP Reply", buf_len);
		return;
	case RTE_ARP_OP_INVREQUEST:
		strlcpy(buf, "Peer Identify Request", buf_len);
		return;
	case RTE_ARP_OP_INVREPLY:
		strlcpy(buf, "Peer Identify Reply", buf_len);
		return;
	default:
		break;
	}
	strlcpy(buf, "Unknown", buf_len);
	return;
}
#endif
#define MaxIPv4String	16
static void
ipv4_addr_to_dot(uint32_t be_ipv4_addr, char *buf, uint8_t buf_size)
{
	uint32_t ipv4_addr;

	ipv4_addr = rte_be_to_cpu_32(be_ipv4_addr);
	snprintf(buf, buf_size, "%d.%d.%d.%d", (ipv4_addr >> 24) & 0xFF,
		(ipv4_addr >> 16) & 0xFF, (ipv4_addr >> 8) & 0xFF,
		ipv4_addr & 0xFF);
}

#define MAX_CLIENTS_NUMBER	128
uint8_t active_clients;
struct client_stats_t {
	uint16_t port;
	uint32_t ipv4_addr;
	uint32_t ipv4_rx_packets;
	uint32_t ipv4_tx_packets;
};
struct client_stats_t client_stats[MAX_CLIENTS_NUMBER];

static void
update_client_stats(uint32_t addr, uint16_t port, uint32_t *TXorRXindicator)
{
	int i = 0;

	for (; i < MAX_CLIENTS_NUMBER; i++)	{
		if ((client_stats[i].ipv4_addr == addr) && (client_stats[i].port == port))	{
			/* Just update RX packets number for this client */
			if (TXorRXindicator == &burstnumberRX)
				client_stats[i].ipv4_rx_packets++;
			else
				client_stats[i].ipv4_tx_packets++;
			return;
		}
	}
	/* We have a new client. Insert him to the table, and increment stats */
	if (TXorRXindicator == &burstnumberRX)
		client_stats[active_clients].ipv4_rx_packets++;
	else
		client_stats[active_clients].ipv4_tx_packets++;
	client_stats[active_clients].ipv4_addr = addr;
	client_stats[active_clients].port = port;
	active_clients++;

}

#ifdef RTE_LIBRTE_BOND_DEBUG_ALB
#define MODE6_DEBUG(info, src_ip, dst_ip, eth_h, arp_op, port, burstnumber) \
	RTE_LOG_LINE(DEBUG, BOND,				\
		"%s port:%d SrcMAC:" RTE_ETHER_ADDR_PRT_FMT " SrcIP:%s " \
		"DstMAC:" RTE_ETHER_ADDR_PRT_FMT " DstIP:%s %s %d", \
		info,							\
		port,							\
		RTE_ETHER_ADDR_BYTES(&eth_h->src_addr),                  \
		src_ip,							\
		RTE_ETHER_ADDR_BYTES(&eth_h->dst_addr),                  \
		dst_ip,							\
		arp_op, ++burstnumber)
#endif

static void
mode6_debug(const char __rte_unused *info,
	struct rte_ether_hdr *eth_h, uint16_t port,
	uint32_t __rte_unused *burstnumber)
{
	struct rte_ipv4_hdr *ipv4_h;
#ifdef RTE_LIBRTE_BOND_DEBUG_ALB
	struct rte_arp_hdr *arp_h;
	char dst_ip[16];
	char ArpOp[24];
	char buf[16];
#endif
	char src_ip[16];

	uint16_t ether_type = eth_h->ether_type;
	uint16_t offset = get_vlan_offset(eth_h, &ether_type);

#ifdef RTE_LIBRTE_BOND_DEBUG_ALB
	strlcpy(buf, info, 16);
#endif

	if (ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
		ipv4_h = (struct rte_ipv4_hdr *)((char *)(eth_h + 1) + offset);
		ipv4_addr_to_dot(ipv4_h->src_addr, src_ip, MaxIPv4String);
#ifdef RTE_LIBRTE_BOND_DEBUG_ALB
		ipv4_addr_to_dot(ipv4_h->dst_addr, dst_ip, MaxIPv4String);
		MODE6_DEBUG(buf, src_ip, dst_ip, eth_h, "", port, *burstnumber);
#endif
		update_client_stats(ipv4_h->src_addr, port, burstnumber);
	}
#ifdef RTE_LIBRTE_BOND_DEBUG_ALB
	else if (ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
		arp_h = (struct rte_arp_hdr *)((char *)(eth_h + 1) + offset);
		ipv4_addr_to_dot(arp_h->arp_data.arp_sip, src_ip, MaxIPv4String);
		ipv4_addr_to_dot(arp_h->arp_data.arp_tip, dst_ip, MaxIPv4String);
		arp_op_name(rte_be_to_cpu_16(arp_h->arp_opcode),
				ArpOp, sizeof(ArpOp));
		MODE6_DEBUG(buf, src_ip, dst_ip, eth_h, ArpOp, port, *burstnumber);
	}
#endif
}
#endif

static uint16_t
bond_ethdev_rx_burst_alb(void *queue, struct rte_mbuf **bufs, uint16_t nb_pkts)
{
	struct bond_rx_queue *bd_rx_q = (struct bond_rx_queue *)queue;
	struct bond_dev_private *internals = bd_rx_q->dev_private;
	struct rte_ether_hdr *eth_h;
	uint16_t ether_type, offset;
	uint16_t nb_recv_pkts;
	int i;

	nb_recv_pkts = bond_ethdev_rx_burst(queue, bufs, nb_pkts);

	for (i = 0; i < nb_recv_pkts; i++) {
		eth_h = rte_pktmbuf_mtod(bufs[i], struct rte_ether_hdr *);
		ether_type = eth_h->ether_type;
		offset = get_vlan_offset(eth_h, &ether_type);

		if (ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
#if defined(RTE_LIBRTE_BOND_DEBUG_ALB) || defined(RTE_LIBRTE_BOND_DEBUG_ALB_L1)
			mode6_debug("RX ARP:", eth_h, bufs[i]->port, &burstnumberRX);
#endif
			bond_mode_alb_arp_recv(eth_h, offset, internals);
		}
#if defined(RTE_LIBRTE_BOND_DEBUG_ALB) || defined(RTE_LIBRTE_BOND_DEBUG_ALB_L1)
		else if (ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
			mode6_debug("RX IPv4:", eth_h, bufs[i]->port, &burstnumberRX);
#endif
	}

	return nb_recv_pkts;
}

static uint16_t
bond_ethdev_tx_burst_round_robin(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_pkts)
{
	struct bond_dev_private *internals;
	struct bond_tx_queue *bd_tx_q;

	struct rte_mbuf *member_bufs[RTE_MAX_ETHPORTS][nb_pkts];
	uint16_t member_nb_pkts[RTE_MAX_ETHPORTS] = { 0 };

	uint16_t num_of_members;
	uint16_t members[RTE_MAX_ETHPORTS];

	uint16_t num_tx_total = 0, num_tx_member;

	static int member_idx;
	int i, cmember_idx = 0, tx_fail_total = 0;

	bd_tx_q = (struct bond_tx_queue *)queue;
	internals = bd_tx_q->dev_private;

	/* Copy member list to protect against member up/down changes during tx
	 * bursting */
	num_of_members = internals->active_member_count;
	memcpy(members, internals->active_members,
			sizeof(internals->active_members[0]) * num_of_members);

	if (num_of_members < 1)
		return num_tx_total;

	/* Populate members mbuf with which packets are to be sent on it  */
	for (i = 0; i < nb_pkts; i++) {
		cmember_idx = (member_idx + i) % num_of_members;
		member_bufs[cmember_idx][(member_nb_pkts[cmember_idx])++] = bufs[i];
	}

	/*
	 * increment current member index so the next call to tx burst starts on the
	 * next member.
	 */
	member_idx = ++cmember_idx;

	/* Send packet burst on each member device */
	for (i = 0; i < num_of_members; i++) {
		if (member_nb_pkts[i] > 0) {
			num_tx_member = rte_eth_tx_prepare(members[i],
					bd_tx_q->queue_id, member_bufs[i],
					member_nb_pkts[i]);
			num_tx_member = rte_eth_tx_burst(members[i], bd_tx_q->queue_id,
					member_bufs[i], num_tx_member);

			/* if tx burst fails move packets to end of bufs */
			if (unlikely(num_tx_member < member_nb_pkts[i])) {
				int tx_fail_member = member_nb_pkts[i] - num_tx_member;

				tx_fail_total += tx_fail_member;

				memcpy(&bufs[nb_pkts - tx_fail_total],
				       &member_bufs[i][num_tx_member],
				       tx_fail_member * sizeof(bufs[0]));
			}
			num_tx_total += num_tx_member;
		}
	}

	return num_tx_total;
}

static uint16_t
bond_ethdev_tx_burst_active_backup(void *queue,
		struct rte_mbuf **bufs, uint16_t nb_pkts)
{
	struct bond_dev_private *internals;
	struct bond_tx_queue *bd_tx_q;
	uint16_t nb_prep_pkts;

	bd_tx_q = (struct bond_tx_queue *)queue;
	internals = bd_tx_q->dev_private;

	if (internals->active_member_count < 1)
		return 0;

	nb_prep_pkts = rte_eth_tx_prepare(internals->current_primary_port,
				bd_tx_q->queue_id, bufs, nb_pkts);

	return rte_eth_tx_burst(internals->current_primary_port, bd_tx_q->queue_id,
			bufs, nb_prep_pkts);
}

static inline uint16_t
ether_hash(struct rte_ether_hdr *eth_hdr)
{
	unaligned_uint16_t *word_src_addr =
		(unaligned_uint16_t *)eth_hdr->src_addr.addr_bytes;
	unaligned_uint16_t *word_dst_addr =
		(unaligned_uint16_t *)eth_hdr->dst_addr.addr_bytes;

	return (word_src_addr[0] ^ word_dst_addr[0]) ^
			(word_src_addr[1] ^ word_dst_addr[1]) ^
			(word_src_addr[2] ^ word_dst_addr[2]);
}

static inline uint32_t
ipv4_hash(struct rte_ipv4_hdr *ipv4_hdr)
{
	return ipv4_hdr->src_addr ^ ipv4_hdr->dst_addr;
}

static inline uint32_t
ipv6_hash(struct rte_ipv6_hdr *ipv6_hdr)
{
	unaligned_uint32_t *word_src_addr = (unaligned_uint32_t *)&ipv6_hdr->src_addr;
	unaligned_uint32_t *word_dst_addr = (unaligned_uint32_t *)&ipv6_hdr->dst_addr;

	return (word_src_addr[0] ^ word_dst_addr[0]) ^
			(word_src_addr[1] ^ word_dst_addr[1]) ^
			(word_src_addr[2] ^ word_dst_addr[2]) ^
			(word_src_addr[3] ^ word_dst_addr[3]);
}


void
burst_xmit_l2_hash(struct rte_mbuf **buf, uint16_t nb_pkts,
		uint16_t member_count, uint16_t *members)
{
	struct rte_ether_hdr *eth_hdr;
	uint32_t hash;
	int i;

	for (i = 0; i < nb_pkts; i++) {
		eth_hdr = rte_pktmbuf_mtod(buf[i], struct rte_ether_hdr *);

		hash = ether_hash(eth_hdr);

		members[i] = (hash ^= hash >> 8) % member_count;
	}
}

void
burst_xmit_l23_hash(struct rte_mbuf **buf, uint16_t nb_pkts,
		uint16_t member_count, uint16_t *members)
{
	uint16_t i;
	struct rte_ether_hdr *eth_hdr;
	uint16_t proto;
	size_t vlan_offset;
	uint32_t hash, l3hash;

	for (i = 0; i < nb_pkts; i++) {
		eth_hdr = rte_pktmbuf_mtod(buf[i], struct rte_ether_hdr *);
		l3hash = 0;

		proto = eth_hdr->ether_type;
		hash = ether_hash(eth_hdr);

		vlan_offset = get_vlan_offset(eth_hdr, &proto);

		if (rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) == proto) {
			struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)
					((char *)(eth_hdr + 1) + vlan_offset);
			l3hash = ipv4_hash(ipv4_hdr);

		} else if (rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6) == proto) {
			struct rte_ipv6_hdr *ipv6_hdr = (struct rte_ipv6_hdr *)
					((char *)(eth_hdr + 1) + vlan_offset);
			l3hash = ipv6_hash(ipv6_hdr);
		}

		hash = hash ^ l3hash;
		hash ^= hash >> 16;
		hash ^= hash >> 8;

		members[i] = hash % member_count;
	}
}

void
burst_xmit_l34_hash(struct rte_mbuf **buf, uint16_t nb_pkts,
		uint16_t member_count, uint16_t *members)
{
	struct rte_ether_hdr *eth_hdr;
	uint16_t proto;
	size_t vlan_offset;
	int i;

	struct rte_udp_hdr *udp_hdr;
	struct rte_tcp_hdr *tcp_hdr;
	uint32_t hash, l3hash, l4hash;

	for (i = 0; i < nb_pkts; i++) {
		eth_hdr = rte_pktmbuf_mtod(buf[i], struct rte_ether_hdr *);
		size_t pkt_end = (size_t)eth_hdr + rte_pktmbuf_data_len(buf[i]);
		proto = eth_hdr->ether_type;
		vlan_offset = get_vlan_offset(eth_hdr, &proto);
		l3hash = 0;
		l4hash = 0;

		if (rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) == proto) {
			struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)
					((char *)(eth_hdr + 1) + vlan_offset);
			size_t ip_hdr_offset;

			l3hash = ipv4_hash(ipv4_hdr);

			/* there is no L4 header in fragmented packet */
			if (likely(rte_ipv4_frag_pkt_is_fragmented(ipv4_hdr)
								== 0)) {
				ip_hdr_offset = (ipv4_hdr->version_ihl
					& RTE_IPV4_HDR_IHL_MASK) *
					RTE_IPV4_IHL_MULTIPLIER;

				if (ipv4_hdr->next_proto_id == IPPROTO_TCP) {
					tcp_hdr = (struct rte_tcp_hdr *)
						((char *)ipv4_hdr +
							ip_hdr_offset);
					if ((size_t)tcp_hdr + sizeof(*tcp_hdr)
							<= pkt_end)
						l4hash = HASH_L4_PORTS(tcp_hdr);
				} else if (ipv4_hdr->next_proto_id ==
								IPPROTO_UDP) {
					udp_hdr = (struct rte_udp_hdr *)
						((char *)ipv4_hdr +
							ip_hdr_offset);
					if ((size_t)udp_hdr + sizeof(*udp_hdr)
							< pkt_end)
						l4hash = HASH_L4_PORTS(udp_hdr);
				}
			}
		} else if  (rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6) == proto) {
			struct rte_ipv6_hdr *ipv6_hdr = (struct rte_ipv6_hdr *)
					((char *)(eth_hdr + 1) + vlan_offset);
			l3hash = ipv6_hash(ipv6_hdr);

			if (ipv6_hdr->proto == IPPROTO_TCP) {
				tcp_hdr = (struct rte_tcp_hdr *)(ipv6_hdr + 1);
				l4hash = HASH_L4_PORTS(tcp_hdr);
			} else if (ipv6_hdr->proto == IPPROTO_UDP) {
				udp_hdr = (struct rte_udp_hdr *)(ipv6_hdr + 1);
				l4hash = HASH_L4_PORTS(udp_hdr);
			}
		}

		hash = l3hash ^ l4hash;
		hash ^= hash >> 16;
		hash ^= hash >> 8;

		members[i] = hash % member_count;
	}
}

struct bwg_member {
	uint64_t bwg_left_int;
	uint64_t bwg_left_remainder;
	uint16_t member;
};

void
bond_tlb_activate_member(struct bond_dev_private *internals) {
	int i;

	for (i = 0; i < internals->active_member_count; i++)
		tlb_last_obytets[internals->active_members[i]] = 0;
}

static int
bandwidth_cmp(const void *a, const void *b)
{
	const struct bwg_member *bwg_a = a;
	const struct bwg_member *bwg_b = b;
	int64_t diff = (int64_t)bwg_b->bwg_left_int - (int64_t)bwg_a->bwg_left_int;
	int64_t diff2 = (int64_t)bwg_b->bwg_left_remainder -
			(int64_t)bwg_a->bwg_left_remainder;
	if (diff > 0)
		return 1;
	else if (diff < 0)
		return -1;
	else if (diff2 > 0)
		return 1;
	else if (diff2 < 0)
		return -1;
	else
		return 0;
}

static void
bandwidth_left(uint16_t port_id, uint64_t load, uint8_t update_idx,
		struct bwg_member *bwg_member)
{
	struct rte_eth_link link_status;
	int ret;

	ret = rte_eth_link_get_nowait(port_id, &link_status);
	if (ret < 0) {
		RTE_BOND_LOG(ERR, "Member (port %u) link get failed: %s",
			     port_id, rte_strerror(-ret));
		return;
	}
	uint64_t link_bwg = link_status.link_speed * 1000000ULL / 8;
	if (link_bwg == 0)
		return;
	link_bwg = link_bwg * (update_idx+1) * REORDER_PERIOD_MS;
	bwg_member->bwg_left_int = (link_bwg - 1000 * load) / link_bwg;
	bwg_member->bwg_left_remainder = (link_bwg - 1000 * load) % link_bwg;
}

static void
bond_ethdev_update_tlb_member_cb(void *arg)
{
	struct bond_dev_private *internals = arg;
	struct rte_eth_stats member_stats;
	struct bwg_member bwg_array[RTE_MAX_ETHPORTS];
	uint16_t member_count;
	uint64_t tx_bytes;

	uint8_t update_stats = 0;
	uint16_t member_id;
	uint16_t i;

	internals->member_update_idx++;


	if (internals->member_update_idx >= REORDER_PERIOD_MS)
		update_stats = 1;

	for (i = 0; i < internals->active_member_count; i++) {
		member_id = internals->active_members[i];
		rte_eth_stats_get(member_id, &member_stats);
		tx_bytes = member_stats.obytes - tlb_last_obytets[member_id];
		bandwidth_left(member_id, tx_bytes,
				internals->member_update_idx, &bwg_array[i]);
		bwg_array[i].member = member_id;

		if (update_stats) {
			tlb_last_obytets[member_id] = member_stats.obytes;
		}
	}

	if (update_stats == 1)
		internals->member_update_idx = 0;

	member_count = i;
	qsort(bwg_array, member_count, sizeof(bwg_array[0]), bandwidth_cmp);
	for (i = 0; i < member_count; i++)
		internals->tlb_members_order[i] = bwg_array[i].member;

	rte_eal_alarm_set(REORDER_PERIOD_MS * 1000, bond_ethdev_update_tlb_member_cb,
			(struct bond_dev_private *)internals);
}

static uint16_t
bond_ethdev_tx_burst_tlb(void *queue, struct rte_mbuf **bufs, uint16_t nb_pkts)
{
	struct bond_tx_queue *bd_tx_q = (struct bond_tx_queue *)queue;
	struct bond_dev_private *internals = bd_tx_q->dev_private;

	struct rte_eth_dev *primary_port =
			&rte_eth_devices[internals->primary_port];
	uint16_t num_tx_total = 0, num_tx_prep;
	uint16_t i, j;

	uint16_t num_of_members = internals->active_member_count;
	uint16_t members[RTE_MAX_ETHPORTS];

	struct rte_ether_hdr *ether_hdr;
	struct rte_ether_addr primary_member_addr;
	struct rte_ether_addr active_member_addr;

	if (num_of_members < 1)
		return num_tx_total;

	memcpy(members, internals->tlb_members_order,
				sizeof(internals->tlb_members_order[0]) * num_of_members);


	rte_ether_addr_copy(primary_port->data->mac_addrs, &primary_member_addr);

	if (nb_pkts > 3) {
		for (i = 0; i < 3; i++)
			rte_prefetch0(rte_pktmbuf_mtod(bufs[i], void*));
	}

	for (i = 0; i < num_of_members; i++) {
		rte_eth_macaddr_get(members[i], &active_member_addr);
		for (j = num_tx_total; j < nb_pkts; j++) {
			if (j + 3 < nb_pkts)
				rte_prefetch0(rte_pktmbuf_mtod(bufs[j+3], void*));

			ether_hdr = rte_pktmbuf_mtod(bufs[j],
						struct rte_ether_hdr *);
			if (rte_is_same_ether_addr(&ether_hdr->src_addr,
							&primary_member_addr))
				rte_ether_addr_copy(&active_member_addr,
						&ether_hdr->src_addr);
#if defined(RTE_LIBRTE_BOND_DEBUG_ALB) || defined(RTE_LIBRTE_BOND_DEBUG_ALB_L1)
					mode6_debug("TX IPv4:", ether_hdr, members[i],
						&burst_number_TX);
#endif
		}

		num_tx_prep = rte_eth_tx_prepare(members[i], bd_tx_q->queue_id,
				bufs + num_tx_total, nb_pkts - num_tx_total);
		num_tx_total += rte_eth_tx_burst(members[i], bd_tx_q->queue_id,
				bufs + num_tx_total, num_tx_prep);

		if (num_tx_total == nb_pkts)
			break;
	}

	return num_tx_total;
}

void
bond_tlb_disable(struct bond_dev_private *internals)
{
	rte_eal_alarm_cancel(bond_ethdev_update_tlb_member_cb, internals);
}

void
bond_tlb_enable(struct bond_dev_private *internals)
{
	bond_ethdev_update_tlb_member_cb(internals);
}

static uint16_t
bond_ethdev_tx_burst_alb(void *queue, struct rte_mbuf **bufs, uint16_t nb_pkts)
{
	struct bond_tx_queue *bd_tx_q = (struct bond_tx_queue *)queue;
	struct bond_dev_private *internals = bd_tx_q->dev_private;

	struct rte_ether_hdr *eth_h;
	uint16_t ether_type, offset;

	struct client_data *client_info;

	/*
	 * We create transmit buffers for every member and one additional to send
	 * through tlb. In worst case every packet will be send on one port.
	 */
	struct rte_mbuf *member_bufs[RTE_MAX_ETHPORTS + 1][nb_pkts];
	uint16_t member_bufs_pkts[RTE_MAX_ETHPORTS + 1] = { 0 };

	/*
	 * We create separate transmit buffers for update packets as they won't
	 * be counted in num_tx_total.
	 */
	struct rte_mbuf *update_bufs[RTE_MAX_ETHPORTS][ALB_HASH_TABLE_SIZE];
	uint16_t update_bufs_pkts[RTE_MAX_ETHPORTS] = { 0 };

	struct rte_mbuf *upd_pkt;
	size_t pkt_size;

	uint16_t num_send, num_not_send = 0;
	uint16_t num_tx_total = 0;
	uint16_t member_idx;

	int i, j;

	/* Search tx buffer for ARP packets and forward them to alb */
	for (i = 0; i < nb_pkts; i++) {
		eth_h = rte_pktmbuf_mtod(bufs[i], struct rte_ether_hdr *);
		ether_type = eth_h->ether_type;
		offset = get_vlan_offset(eth_h, &ether_type);

		if (ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
			member_idx = bond_mode_alb_arp_xmit(eth_h, offset, internals);

			/* Change src mac in eth header */
			rte_eth_macaddr_get(member_idx, &eth_h->src_addr);

			/* Add packet to member tx buffer */
			member_bufs[member_idx][member_bufs_pkts[member_idx]] = bufs[i];
			member_bufs_pkts[member_idx]++;
		} else {
			/* If packet is not ARP, send it with TLB policy */
			member_bufs[RTE_MAX_ETHPORTS][member_bufs_pkts[RTE_MAX_ETHPORTS]] =
					bufs[i];
			member_bufs_pkts[RTE_MAX_ETHPORTS]++;
		}
	}

	/* Update connected client ARP tables */
	if (internals->mode6.ntt) {
		for (i = 0; i < ALB_HASH_TABLE_SIZE; i++) {
			client_info = &internals->mode6.client_table[i];

			if (client_info->in_use) {
				/* Allocate new packet to send ARP update on current member */
				upd_pkt = rte_pktmbuf_alloc(internals->mode6.mempool);
				if (upd_pkt == NULL) {
					RTE_BOND_LOG(ERR,
						     "Failed to allocate ARP packet from pool");
					continue;
				}
				pkt_size = sizeof(struct rte_ether_hdr) +
					sizeof(struct rte_arp_hdr) +
					client_info->vlan_count *
					sizeof(struct rte_vlan_hdr);
				upd_pkt->data_len = pkt_size;
				upd_pkt->pkt_len = pkt_size;

				member_idx = bond_mode_alb_arp_upd(client_info, upd_pkt,
						internals);

				/* Add packet to update tx buffer */
				update_bufs[member_idx][update_bufs_pkts[member_idx]] = upd_pkt;
				update_bufs_pkts[member_idx]++;
			}
		}
		internals->mode6.ntt = 0;
	}

	/* Send ARP packets on proper members */
	for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
		if (member_bufs_pkts[i] > 0) {
			num_send = rte_eth_tx_prepare(i, bd_tx_q->queue_id,
					member_bufs[i], member_bufs_pkts[i]);
			num_send = rte_eth_tx_burst(i, bd_tx_q->queue_id,
					member_bufs[i], num_send);
			for (j = 0; j < member_bufs_pkts[i] - num_send; j++) {
				bufs[nb_pkts - 1 - num_not_send - j] =
						member_bufs[i][nb_pkts - 1 - j];
			}

			num_tx_total += num_send;
			num_not_send += member_bufs_pkts[i] - num_send;

#if defined(RTE_LIBRTE_BOND_DEBUG_ALB) || defined(RTE_LIBRTE_BOND_DEBUG_ALB_L1)
	/* Print TX stats including update packets */
			for (j = 0; j < member_bufs_pkts[i]; j++) {
				eth_h = rte_pktmbuf_mtod(member_bufs[i][j],
							struct rte_ether_hdr *);
				mode6_debug("TX ARP:", eth_h, i, &burst_number_TX);
			}
#endif
		}
	}

	/* Send update packets on proper members */
	for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
		if (update_bufs_pkts[i] > 0) {
			num_send = rte_eth_tx_prepare(i, bd_tx_q->queue_id,
					update_bufs[i], update_bufs_pkts[i]);
			num_send = rte_eth_tx_burst(i, bd_tx_q->queue_id, update_bufs[i],
					num_send);
			for (j = num_send; j < update_bufs_pkts[i]; j++) {
				rte_pktmbuf_free(update_bufs[i][j]);
			}
#if defined(RTE_LIBRTE_BOND_DEBUG_ALB) || defined(RTE_LIBRTE_BOND_DEBUG_ALB_L1)
			for (j = 0; j < update_bufs_pkts[i]; j++) {
				eth_h = rte_pktmbuf_mtod(update_bufs[i][j],
							struct rte_ether_hdr *);
				mode6_debug("TX ARPupd:", eth_h, i, &burst_number_TX);
			}
#endif
		}
	}

	/* Send non-ARP packets using tlb policy */
	if (member_bufs_pkts[RTE_MAX_ETHPORTS] > 0) {
		num_send = bond_ethdev_tx_burst_tlb(queue,
				member_bufs[RTE_MAX_ETHPORTS],
				member_bufs_pkts[RTE_MAX_ETHPORTS]);

		for (j = 0; j < member_bufs_pkts[RTE_MAX_ETHPORTS]; j++) {
			bufs[nb_pkts - 1 - num_not_send - j] =
					member_bufs[RTE_MAX_ETHPORTS][nb_pkts - 1 - j];
		}

		num_tx_total += num_send;
	}

	return num_tx_total;
}

static inline uint16_t
tx_burst_balance(void *queue, struct rte_mbuf **bufs, uint16_t nb_bufs,
		 uint16_t *member_port_ids, uint16_t member_count)
{
	struct bond_tx_queue *bd_tx_q = (struct bond_tx_queue *)queue;
	struct bond_dev_private *internals = bd_tx_q->dev_private;

	/* Array to sort mbufs for transmission on each member into */
	struct rte_mbuf *member_bufs[RTE_MAX_ETHPORTS][nb_bufs];
	/* Number of mbufs for transmission on each member */
	uint16_t member_nb_bufs[RTE_MAX_ETHPORTS] = { 0 };
	/* Mapping array generated by hash function to map mbufs to members */
	uint16_t bufs_member_port_idxs[nb_bufs];

	uint16_t member_tx_count;
	uint16_t total_tx_count = 0, total_tx_fail_count = 0;

	uint16_t i;

	/*
	 * Populate members mbuf with the packets which are to be sent on it
	 * selecting output member using hash based on xmit policy
	 */
	internals->burst_xmit_hash(bufs, nb_bufs, member_count,
			bufs_member_port_idxs);

	for (i = 0; i < nb_bufs; i++) {
		/* Populate member mbuf arrays with mbufs for that member. */
		uint16_t member_idx = bufs_member_port_idxs[i];

		member_bufs[member_idx][member_nb_bufs[member_idx]++] = bufs[i];
	}

	/* Send packet burst on each member device */
	for (i = 0; i < member_count; i++) {
		if (member_nb_bufs[i] == 0)
			continue;

		member_tx_count = rte_eth_tx_prepare(member_port_ids[i],
				bd_tx_q->queue_id, member_bufs[i],
				member_nb_bufs[i]);
		member_tx_count = rte_eth_tx_burst(member_port_ids[i],
				bd_tx_q->queue_id, member_bufs[i],
				member_tx_count);

		total_tx_count += member_tx_count;

		/* If tx burst fails move packets to end of bufs */
		if (unlikely(member_tx_count < member_nb_bufs[i])) {
			int member_tx_fail_count = member_nb_bufs[i] -
					member_tx_count;
			total_tx_fail_count += member_tx_fail_count;
			memcpy(&bufs[nb_bufs - total_tx_fail_count],
			       &member_bufs[i][member_tx_count],
			       member_tx_fail_count * sizeof(bufs[0]));
		}
	}

	return total_tx_count;
}

static uint16_t
bond_ethdev_tx_burst_balance(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_bufs)
{
	struct bond_tx_queue *bd_tx_q = (struct bond_tx_queue *)queue;
	struct bond_dev_private *internals = bd_tx_q->dev_private;

	uint16_t member_port_ids[RTE_MAX_ETHPORTS];
	uint16_t member_count;

	if (unlikely(nb_bufs == 0))
		return 0;

	/* Copy member list to protect against member up/down changes during tx
	 * bursting
	 */
	member_count = internals->active_member_count;
	if (unlikely(member_count < 1))
		return 0;

	memcpy(member_port_ids, internals->active_members,
			sizeof(member_port_ids[0]) * member_count);
	return tx_burst_balance(queue, bufs, nb_bufs, member_port_ids,
				member_count);
}

static inline uint16_t
tx_burst_8023ad(void *queue, struct rte_mbuf **bufs, uint16_t nb_bufs,
		bool dedicated_txq)
{
	struct bond_tx_queue *bd_tx_q = (struct bond_tx_queue *)queue;
	struct bond_dev_private *internals = bd_tx_q->dev_private;

	uint16_t member_port_ids[RTE_MAX_ETHPORTS];
	uint16_t member_count;

	uint16_t dist_member_port_ids[RTE_MAX_ETHPORTS];
	uint16_t dist_member_count;

	uint16_t member_tx_count;

	uint16_t i;

	/* Copy member list to protect against member up/down changes during tx
	 * bursting */
	member_count = internals->active_member_count;
	if (unlikely(member_count < 1))
		return 0;

	memcpy(member_port_ids, internals->active_members,
			sizeof(member_port_ids[0]) * member_count);

	if (dedicated_txq)
		goto skip_tx_ring;

	/* Check for LACP control packets and send if available */
	for (i = 0; i < member_count; i++) {
		struct port *port = &bond_mode_8023ad_ports[member_port_ids[i]];
		struct rte_mbuf *ctrl_pkt = NULL;

		if (likely(rte_ring_empty(port->tx_ring)))
			continue;

		if (rte_ring_dequeue(port->tx_ring,
				     (void **)&ctrl_pkt) != -ENOENT) {
			member_tx_count = rte_eth_tx_prepare(member_port_ids[i],
					bd_tx_q->queue_id, &ctrl_pkt, 1);
			member_tx_count = rte_eth_tx_burst(member_port_ids[i],
					bd_tx_q->queue_id, &ctrl_pkt, member_tx_count);
			/*
			 * re-enqueue LAG control plane packets to buffering
			 * ring if transmission fails so the packet isn't lost.
			 */
			if (member_tx_count != 1)
				rte_ring_enqueue(port->tx_ring,	ctrl_pkt);
		}
	}

skip_tx_ring:
	if (unlikely(nb_bufs == 0))
		return 0;

	dist_member_count = 0;
	for (i = 0; i < member_count; i++) {
		struct port *port = &bond_mode_8023ad_ports[member_port_ids[i]];

		if (ACTOR_STATE(port, DISTRIBUTING))
			dist_member_port_ids[dist_member_count++] =
					member_port_ids[i];
	}

	if (unlikely(dist_member_count < 1))
		return 0;

	return tx_burst_balance(queue, bufs, nb_bufs, dist_member_port_ids,
				dist_member_count);
}

static uint16_t
bond_ethdev_tx_burst_8023ad(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_bufs)
{
	return tx_burst_8023ad(queue, bufs, nb_bufs, false);
}

static uint16_t
bond_ethdev_tx_burst_8023ad_fast_queue(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_bufs)
{
	return tx_burst_8023ad(queue, bufs, nb_bufs, true);
}

static uint16_t
bond_ethdev_tx_burst_broadcast(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_pkts)
{
	struct bond_dev_private *internals;
	struct bond_tx_queue *bd_tx_q;

	uint16_t members[RTE_MAX_ETHPORTS];
	uint8_t tx_failed_flag = 0;
	uint16_t num_of_members;

	uint16_t max_nb_of_tx_pkts = 0;

	int member_tx_total[RTE_MAX_ETHPORTS];
	int i, most_successful_tx_member = -1;

	bd_tx_q = (struct bond_tx_queue *)queue;
	internals = bd_tx_q->dev_private;

	/* Copy member list to protect against member up/down changes during tx
	 * bursting */
	num_of_members = internals->active_member_count;
	memcpy(members, internals->active_members,
			sizeof(internals->active_members[0]) * num_of_members);

	if (num_of_members < 1)
		return 0;

	/* It is rare that bond different PMDs together, so just call tx-prepare once */
	nb_pkts = rte_eth_tx_prepare(members[0], bd_tx_q->queue_id, bufs, nb_pkts);

	/* Increment reference count on mbufs */
	for (i = 0; i < nb_pkts; i++)
		rte_pktmbuf_refcnt_update(bufs[i], num_of_members - 1);

	/* Transmit burst on each active member */
	for (i = 0; i < num_of_members; i++) {
		member_tx_total[i] = rte_eth_tx_burst(members[i], bd_tx_q->queue_id,
					bufs, nb_pkts);

		if (unlikely(member_tx_total[i] < nb_pkts))
			tx_failed_flag = 1;

		/* record the value and member index for the member which transmits the
		 * maximum number of packets */
		if (member_tx_total[i] > max_nb_of_tx_pkts) {
			max_nb_of_tx_pkts = member_tx_total[i];
			most_successful_tx_member = i;
		}
	}

	/* if members fail to transmit packets from burst, the calling application
	 * is not expected to know about multiple references to packets so we must
	 * handle failures of all packets except those of the most successful member
	 */
	if (unlikely(tx_failed_flag))
		for (i = 0; i < num_of_members; i++)
			if (i != most_successful_tx_member)
				while (member_tx_total[i] < nb_pkts)
					rte_pktmbuf_free(bufs[member_tx_total[i]++]);

	return max_nb_of_tx_pkts;
}

static void
link_properties_set(struct rte_eth_dev *ethdev, struct rte_eth_link *member_link)
{
	struct bond_dev_private *bond_ctx = ethdev->data->dev_private;

	if (bond_ctx->mode == BONDING_MODE_8023AD) {
		/**
		 * If in mode 4 then save the link properties of the first
		 * member, all subsequent members must match these properties
		 */
		struct rte_eth_link *bond_link = &bond_ctx->mode4.member_link;

		bond_link->link_autoneg = member_link->link_autoneg;
		bond_link->link_duplex = member_link->link_duplex;
		bond_link->link_speed = member_link->link_speed;
	} else {
		/**
		 * In any other mode the link properties are set to default
		 * values of AUTONEG/DUPLEX
		 */
		ethdev->data->dev_link.link_autoneg = RTE_ETH_LINK_AUTONEG;
		ethdev->data->dev_link.link_duplex = RTE_ETH_LINK_FULL_DUPLEX;
	}
}

static int
link_properties_valid(struct rte_eth_dev *ethdev,
		struct rte_eth_link *member_link)
{
	struct bond_dev_private *bond_ctx = ethdev->data->dev_private;

	if (bond_ctx->mode == BONDING_MODE_8023AD) {
		struct rte_eth_link *bond_link = &bond_ctx->mode4.member_link;

		if (bond_link->link_duplex != member_link->link_duplex ||
			bond_link->link_autoneg != member_link->link_autoneg ||
			bond_link->link_speed != member_link->link_speed)
			return -1;
	}

	return 0;
}

int
mac_address_get(struct rte_eth_dev *eth_dev,
		struct rte_ether_addr *dst_mac_addr)
{
	struct rte_ether_addr *mac_addr;

	if (eth_dev == NULL) {
		RTE_BOND_LOG(ERR, "NULL pointer eth_dev specified");
		return -1;
	}

	if (dst_mac_addr == NULL) {
		RTE_BOND_LOG(ERR, "NULL pointer MAC specified");
		return -1;
	}

	mac_addr = eth_dev->data->mac_addrs;

	rte_ether_addr_copy(mac_addr, dst_mac_addr);
	return 0;
}

int
mac_address_set(struct rte_eth_dev *eth_dev,
		struct rte_ether_addr *new_mac_addr)
{
	struct rte_ether_addr *mac_addr;

	if (eth_dev == NULL) {
		RTE_BOND_LOG(ERR, "NULL pointer eth_dev specified");
		return -1;
	}

	if (new_mac_addr == NULL) {
		RTE_BOND_LOG(ERR, "NULL pointer MAC specified");
		return -1;
	}

	mac_addr = eth_dev->data->mac_addrs;

	/* If new MAC is different to current MAC then update */
	if (memcmp(mac_addr, new_mac_addr, sizeof(*mac_addr)) != 0)
		memcpy(mac_addr, new_mac_addr, sizeof(*mac_addr));

	return 0;
}

static const struct rte_ether_addr null_mac_addr;

/*
 * Add additional MAC addresses to the member
 */
int
member_add_mac_addresses(struct rte_eth_dev *bonding_eth_dev,
		uint16_t member_port_id)
{
	int i, ret;
	struct rte_ether_addr *mac_addr;

	for (i = 1; i < BOND_MAX_MAC_ADDRS; i++) {
		mac_addr = &bonding_eth_dev->data->mac_addrs[i];
		if (rte_is_same_ether_addr(mac_addr, &null_mac_addr))
			break;

		ret = rte_eth_dev_mac_addr_add(member_port_id, mac_addr, 0);
		if (ret < 0) {
			/* rollback */
			for (i--; i > 0; i--)
				rte_eth_dev_mac_addr_remove(member_port_id,
					&bonding_eth_dev->data->mac_addrs[i]);
			return ret;
		}
	}

	return 0;
}

/*
 * Remove additional MAC addresses from the member
 */
int
member_remove_mac_addresses(struct rte_eth_dev *bonding_eth_dev,
		uint16_t member_port_id)
{
	int i, rc, ret;
	struct rte_ether_addr *mac_addr;

	rc = 0;
	for (i = 1; i < BOND_MAX_MAC_ADDRS; i++) {
		mac_addr = &bonding_eth_dev->data->mac_addrs[i];
		if (rte_is_same_ether_addr(mac_addr, &null_mac_addr))
			break;

		ret = rte_eth_dev_mac_addr_remove(member_port_id, mac_addr);
		/* save only the first error */
		if (ret < 0 && rc == 0)
			rc = ret;
	}

	return rc;
}

int
mac_address_members_update(struct rte_eth_dev *bonding_eth_dev)
{
	struct bond_dev_private *internals = bonding_eth_dev->data->dev_private;
	bool set;
	int i;

	/* Update member devices MAC addresses */
	if (internals->member_count < 1)
		return -1;

	switch (internals->mode) {
	case BONDING_MODE_ROUND_ROBIN:
	case BONDING_MODE_BALANCE:
	case BONDING_MODE_BROADCAST:
		for (i = 0; i < internals->member_count; i++) {
			if (rte_eth_dev_default_mac_addr_set(
					internals->members[i].port_id,
					bonding_eth_dev->data->mac_addrs)) {
				RTE_BOND_LOG(ERR, "Failed to update port Id %d MAC address",
						internals->members[i].port_id);
				return -1;
			}
		}
		break;
	case BONDING_MODE_8023AD:
		bond_mode_8023ad_mac_address_update(bonding_eth_dev);
		break;
	case BONDING_MODE_ACTIVE_BACKUP:
	case BONDING_MODE_TLB:
	case BONDING_MODE_ALB:
	default:
		set = true;
		for (i = 0; i < internals->member_count; i++) {
			if (internals->members[i].port_id ==
					internals->current_primary_port) {
				if (rte_eth_dev_default_mac_addr_set(
						internals->current_primary_port,
						bonding_eth_dev->data->mac_addrs)) {
					RTE_BOND_LOG(ERR, "Failed to update port Id %d MAC address",
							internals->current_primary_port);
					set = false;
				}
			} else {
				if (rte_eth_dev_default_mac_addr_set(
						internals->members[i].port_id,
						&internals->members[i].persisted_mac_addr)) {
					RTE_BOND_LOG(ERR, "Failed to update port Id %d MAC address",
							internals->members[i].port_id);
				}
			}
		}
		if (!set)
			return -1;
	}

	return 0;
}

int
bond_ethdev_mode_set(struct rte_eth_dev *eth_dev, uint8_t mode)
{
	struct bond_dev_private *internals;

	internals = eth_dev->data->dev_private;

	switch (mode) {
	case BONDING_MODE_ROUND_ROBIN:
		eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_round_robin;
		eth_dev->rx_pkt_burst = bond_ethdev_rx_burst;
		break;
	case BONDING_MODE_ACTIVE_BACKUP:
		eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_active_backup;
		eth_dev->rx_pkt_burst = bond_ethdev_rx_burst_active_backup;
		break;
	case BONDING_MODE_BALANCE:
		eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_balance;
		eth_dev->rx_pkt_burst = bond_ethdev_rx_burst;
		break;
	case BONDING_MODE_BROADCAST:
		eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_broadcast;
		eth_dev->rx_pkt_burst = bond_ethdev_rx_burst;
		break;
	case BONDING_MODE_8023AD:
		if (bond_mode_8023ad_enable(eth_dev) != 0)
			return -1;

		if (internals->mode4.dedicated_queues.enabled == 0) {
			eth_dev->rx_pkt_burst = bond_ethdev_rx_burst_8023ad;
			eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_8023ad;
			RTE_BOND_LOG(WARNING,
				"Using mode 4, it is necessary to do TX burst "
				"and RX burst at least every 100ms.");
		} else {
			/* Use flow director's optimization */
			eth_dev->rx_pkt_burst =
					bond_ethdev_rx_burst_8023ad_fast_queue;
			eth_dev->tx_pkt_burst =
					bond_ethdev_tx_burst_8023ad_fast_queue;
		}
		break;
	case BONDING_MODE_TLB:
		eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_tlb;
		eth_dev->rx_pkt_burst = bond_ethdev_rx_burst_active_backup;
		break;
	case BONDING_MODE_ALB:
		if (bond_mode_alb_enable(eth_dev) != 0)
			return -1;

		eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_alb;
		eth_dev->rx_pkt_burst = bond_ethdev_rx_burst_alb;
		break;
	default:
		return -1;
	}

	internals->mode = mode;

	return 0;
}


static int
member_configure_slow_queue(struct rte_eth_dev *bonding_eth_dev,
		struct rte_eth_dev *member_eth_dev)
{
	int errval = 0;
	struct bond_dev_private *internals = bonding_eth_dev->data->dev_private;
	struct port *port = &bond_mode_8023ad_ports[member_eth_dev->data->port_id];

	if (port->slow_pool == NULL) {
		char mem_name[256];
		int member_id = member_eth_dev->data->port_id;

		snprintf(mem_name, RTE_DIM(mem_name), "member_port%u_slow_pool",
				member_id);
		port->slow_pool = rte_pktmbuf_pool_create(mem_name, 8191,
			250, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
			member_eth_dev->data->numa_node);

		/* Any memory allocation failure in initialization is critical because
		 * resources can't be free, so reinitialization is impossible. */
		if (port->slow_pool == NULL) {
			rte_panic("Member %u: Failed to create memory pool '%s': %s\n",
				member_id, mem_name, rte_strerror(rte_errno));
		}
	}

	if (internals->mode4.dedicated_queues.enabled == 1) {
		struct rte_eth_dev_info member_info = {};
		uint16_t nb_rx_desc = SLOW_RX_QUEUE_HW_DEFAULT_SIZE;
		uint16_t nb_tx_desc = SLOW_TX_QUEUE_HW_DEFAULT_SIZE;

		errval = rte_eth_dev_info_get(member_eth_dev->data->port_id,
				&member_info);
		if (errval != 0) {
			RTE_BOND_LOG(ERR,
					"rte_eth_dev_info_get: port=%d, err (%d)",
					member_eth_dev->data->port_id,
					errval);
			return errval;
		}

		if (member_info.rx_desc_lim.nb_min != 0)
			nb_rx_desc = member_info.rx_desc_lim.nb_min;

		/* Configure slow Rx queue */
		errval = rte_eth_rx_queue_setup(member_eth_dev->data->port_id,
				internals->mode4.dedicated_queues.rx_qid, nb_rx_desc,
				rte_eth_dev_socket_id(member_eth_dev->data->port_id),
				NULL, port->slow_pool);
		if (errval != 0) {
			RTE_BOND_LOG(ERR,
					"rte_eth_rx_queue_setup: port=%d queue_id %d, err (%d)",
					member_eth_dev->data->port_id,
					internals->mode4.dedicated_queues.rx_qid,
					errval);
			return errval;
		}

		if (member_info.tx_desc_lim.nb_min != 0)
			nb_tx_desc = member_info.tx_desc_lim.nb_min;

		errval = rte_eth_tx_queue_setup(member_eth_dev->data->port_id,
				internals->mode4.dedicated_queues.tx_qid, nb_tx_desc,
				rte_eth_dev_socket_id(member_eth_dev->data->port_id),
				NULL);
		if (errval != 0) {
			RTE_BOND_LOG(ERR,
				"rte_eth_tx_queue_setup: port=%d queue_id %d, err (%d)",
				member_eth_dev->data->port_id,
				internals->mode4.dedicated_queues.tx_qid,
				errval);
			return errval;
		}
	}
	return 0;
}

int
member_configure(struct rte_eth_dev *bonding_eth_dev,
		struct rte_eth_dev *member_eth_dev)
{
	uint16_t nb_rx_queues;
	uint16_t nb_tx_queues;

	int errval;

	struct bond_dev_private *internals = bonding_eth_dev->data->dev_private;

	/* Stop member */
	errval = rte_eth_dev_stop(member_eth_dev->data->port_id);
	if (errval != 0)
		RTE_BOND_LOG(ERR, "rte_eth_dev_stop: port %u, err (%d)",
			     member_eth_dev->data->port_id, errval);

	/* Enable interrupts on member device if supported */
	if (member_eth_dev->data->dev_flags & RTE_ETH_DEV_INTR_LSC)
		member_eth_dev->data->dev_conf.intr_conf.lsc = 1;

	/* If RSS is enabled for bonding, try to enable it for members  */
	if (bonding_eth_dev->data->dev_conf.rxmode.mq_mode & RTE_ETH_MQ_RX_RSS_FLAG) {
		/* rss_key won't be empty if RSS is configured in bonding dev */
		member_eth_dev->data->dev_conf.rx_adv_conf.rss_conf.rss_key_len =
					internals->rss_key_len;
		member_eth_dev->data->dev_conf.rx_adv_conf.rss_conf.rss_key =
					internals->rss_key;

		member_eth_dev->data->dev_conf.rx_adv_conf.rss_conf.rss_hf =
				bonding_eth_dev->data->dev_conf.rx_adv_conf.rss_conf.rss_hf;
		member_eth_dev->data->dev_conf.rxmode.mq_mode =
				bonding_eth_dev->data->dev_conf.rxmode.mq_mode;
	} else {
		member_eth_dev->data->dev_conf.rx_adv_conf.rss_conf.rss_key_len = 0;
		member_eth_dev->data->dev_conf.rx_adv_conf.rss_conf.rss_key = NULL;
		member_eth_dev->data->dev_conf.rx_adv_conf.rss_conf.rss_hf = 0;
		member_eth_dev->data->dev_conf.rxmode.mq_mode =
				bonding_eth_dev->data->dev_conf.rxmode.mq_mode;
	}

	member_eth_dev->data->dev_conf.rxmode.mtu =
			bonding_eth_dev->data->dev_conf.rxmode.mtu;
	member_eth_dev->data->dev_conf.link_speeds =
			bonding_eth_dev->data->dev_conf.link_speeds;

	member_eth_dev->data->dev_conf.txmode.offloads =
			bonding_eth_dev->data->dev_conf.txmode.offloads;

	member_eth_dev->data->dev_conf.rxmode.offloads =
			bonding_eth_dev->data->dev_conf.rxmode.offloads;

	nb_rx_queues = bonding_eth_dev->data->nb_rx_queues;
	nb_tx_queues = bonding_eth_dev->data->nb_tx_queues;

	if (internals->mode == BONDING_MODE_8023AD) {
		if (internals->mode4.dedicated_queues.enabled == 1) {
			nb_rx_queues++;
			nb_tx_queues++;
		}
	}

	/* Configure device */
	errval = rte_eth_dev_configure(member_eth_dev->data->port_id,
			nb_rx_queues, nb_tx_queues,
			&member_eth_dev->data->dev_conf);
	if (errval != 0) {
		RTE_BOND_LOG(ERR, "Cannot configure member device: port %u, err (%d)",
				member_eth_dev->data->port_id, errval);
		return errval;
	}

	errval = rte_eth_dev_set_mtu(member_eth_dev->data->port_id,
				     bonding_eth_dev->data->mtu);
	if (errval != 0 && errval != -ENOTSUP) {
		RTE_BOND_LOG(ERR, "rte_eth_dev_set_mtu: port %u, err (%d)",
				member_eth_dev->data->port_id, errval);
		return errval;
	}
	return 0;
}

int
member_start(struct rte_eth_dev *bonding_eth_dev,
		struct rte_eth_dev *member_eth_dev)
{
	int errval = 0;
	struct bond_rx_queue *bd_rx_q;
	struct bond_tx_queue *bd_tx_q;
	uint16_t q_id;
	struct rte_flow_error flow_error;
	struct bond_dev_private *internals = bonding_eth_dev->data->dev_private;
	uint16_t member_port_id = member_eth_dev->data->port_id;

	/* Setup Rx Queues */
	for (q_id = 0; q_id < bonding_eth_dev->data->nb_rx_queues; q_id++) {
		bd_rx_q = (struct bond_rx_queue *)bonding_eth_dev->data->rx_queues[q_id];

		errval = rte_eth_rx_queue_setup(member_port_id, q_id,
				bd_rx_q->nb_rx_desc,
				rte_eth_dev_socket_id(member_port_id),
				&(bd_rx_q->rx_conf), bd_rx_q->mb_pool);
		if (errval != 0) {
			RTE_BOND_LOG(ERR,
					"rte_eth_rx_queue_setup: port=%d queue_id %d, err (%d)",
					member_port_id, q_id, errval);
			return errval;
		}
	}

	/* Setup Tx Queues */
	for (q_id = 0; q_id < bonding_eth_dev->data->nb_tx_queues; q_id++) {
		bd_tx_q = (struct bond_tx_queue *)bonding_eth_dev->data->tx_queues[q_id];

		errval = rte_eth_tx_queue_setup(member_port_id, q_id,
				bd_tx_q->nb_tx_desc,
				rte_eth_dev_socket_id(member_port_id),
				&bd_tx_q->tx_conf);
		if (errval != 0) {
			RTE_BOND_LOG(ERR,
				"rte_eth_tx_queue_setup: port=%d queue_id %d, err (%d)",
				member_port_id, q_id, errval);
			return errval;
		}
	}

	if (internals->mode == BONDING_MODE_8023AD &&
			internals->mode4.dedicated_queues.enabled == 1) {
		if (member_configure_slow_queue(bonding_eth_dev, member_eth_dev)
				!= 0)
			return errval;

		errval = bond_ethdev_8023ad_flow_verify(bonding_eth_dev,
				member_port_id);
		if (errval != 0) {
			RTE_BOND_LOG(ERR,
				"bond_ethdev_8023ad_flow_verify: port=%d, err (%d)",
				member_port_id, errval);
			return errval;
		}

		if (internals->mode4.dedicated_queues.flow[member_port_id] != NULL) {
			errval = rte_flow_destroy(member_port_id,
					internals->mode4.dedicated_queues.flow[member_port_id],
					&flow_error);
			RTE_BOND_LOG(ERR, "bond_ethdev_8023ad_flow_destroy: port=%d, err (%d)",
				member_port_id, errval);
		}
	}

	/* Start device */
	errval = rte_eth_dev_start(member_port_id);
	if (errval != 0) {
		RTE_BOND_LOG(ERR, "rte_eth_dev_start: port=%u, err (%d)",
				member_port_id, errval);
		return -1;
	}

	if (internals->mode == BONDING_MODE_8023AD &&
			internals->mode4.dedicated_queues.enabled == 1) {
		errval = bond_ethdev_8023ad_flow_set(bonding_eth_dev,
				member_port_id);
		if (errval != 0) {
			RTE_BOND_LOG(ERR,
				"bond_ethdev_8023ad_flow_set: port=%d, err (%d)",
				member_port_id, errval);
			return errval;
		}
	}

	/*
	 * If flow-isolation is not enabled, then check whether RSS is enabled for
	 * bonding, synchronize RETA
	 */
	if (internals->flow_isolated_valid == 0 &&
		(bonding_eth_dev->data->dev_conf.rxmode.mq_mode & RTE_ETH_MQ_RX_RSS)) {
		int i;

		for (i = 0; i < internals->member_count; i++) {
			if (internals->members[i].port_id == member_port_id) {
				errval = rte_eth_dev_rss_reta_update(
						member_port_id,
						&internals->reta_conf[0],
						internals->members[i].reta_size);
				if (errval != 0) {
					RTE_BOND_LOG(WARNING,
						     "rte_eth_dev_rss_reta_update on member port %d fails (err %d)."
						     " RSS Configuration for bonding may be inconsistent.",
						     member_port_id, errval);
				}
				break;
			}
		}
	}

	/* If lsc interrupt is set, check initial member's link status */
	if (member_eth_dev->data->dev_flags & RTE_ETH_DEV_INTR_LSC) {
		member_eth_dev->dev_ops->link_update(member_eth_dev, 0);
		bond_ethdev_lsc_event_callback(member_port_id,
			RTE_ETH_EVENT_INTR_LSC, &bonding_eth_dev->data->port_id,
			NULL);
	}

	return 0;
}

void
member_remove(struct bond_dev_private *internals,
		struct rte_eth_dev *member_eth_dev)
{
	uint16_t i;

	for (i = 0; i < internals->member_count; i++)
		if (internals->members[i].port_id ==
				member_eth_dev->data->port_id)
			break;

	if (i < (internals->member_count - 1)) {
		struct rte_flow *flow;

		memmove(&internals->members[i], &internals->members[i + 1],
				sizeof(internals->members[0]) *
				(internals->member_count - i - 1));
		TAILQ_FOREACH(flow, &internals->flow_list, next) {
			memmove(&flow->flows[i], &flow->flows[i + 1],
				sizeof(flow->flows[0]) *
				(internals->member_count - i - 1));
			flow->flows[internals->member_count - 1] = NULL;
		}
	}

	internals->member_count--;

	/* force reconfiguration of member interfaces */
	rte_eth_dev_internal_reset(member_eth_dev);
}

static void
bond_ethdev_member_link_status_change_monitor(void *cb_arg);

void
member_add(struct bond_dev_private *internals,
		struct rte_eth_dev *member_eth_dev)
{
	struct bond_member_details *member_details =
			&internals->members[internals->member_count];

	member_details->port_id = member_eth_dev->data->port_id;
	member_details->last_link_status = 0;

	/* Mark member devices that don't support interrupts so we can
	 * compensate when we start the bond
	 */
	if (!(member_eth_dev->data->dev_flags & RTE_ETH_DEV_INTR_LSC))
		member_details->link_status_poll_enabled = 1;

	member_details->link_status_wait_to_complete = 0;
	/* clean tlb_last_obytes when adding port for bonding device */
	memcpy(&member_details->persisted_mac_addr, member_eth_dev->data->mac_addrs,
			sizeof(struct rte_ether_addr));
}

void
bond_ethdev_primary_set(struct bond_dev_private *internals,
		uint16_t member_port_id)
{
	int i;

	if (internals->active_member_count < 1)
		internals->current_primary_port = member_port_id;
	else
		/* Search bonding device member ports for new proposed primary port */
		for (i = 0; i < internals->active_member_count; i++) {
			if (internals->active_members[i] == member_port_id)
				internals->current_primary_port = member_port_id;
		}
}

static int
bond_ethdev_promiscuous_enable(struct rte_eth_dev *eth_dev);

static int
bond_ethdev_start(struct rte_eth_dev *eth_dev)
{
	struct bond_dev_private *internals;
	int i;

	/* member eth dev will be started by bonding device */
	if (check_for_bonding_ethdev(eth_dev)) {
		RTE_BOND_LOG(ERR, "User tried to explicitly start a member eth_dev (%d)",
				eth_dev->data->port_id);
		return -1;
	}

	eth_dev->data->dev_link.link_status = RTE_ETH_LINK_DOWN;
	eth_dev->data->dev_started = 1;

	internals = eth_dev->data->dev_private;

	if (internals->member_count == 0) {
		RTE_BOND_LOG(ERR, "Cannot start port since there are no member devices");
		goto out_err;
	}

	if (internals->user_defined_mac == 0) {
		struct rte_ether_addr *new_mac_addr = NULL;

		for (i = 0; i < internals->member_count; i++)
			if (internals->members[i].port_id == internals->primary_port)
				new_mac_addr = &internals->members[i].persisted_mac_addr;

		if (new_mac_addr == NULL)
			goto out_err;

		if (mac_address_set(eth_dev, new_mac_addr) != 0) {
			RTE_BOND_LOG(ERR, "bonding port (%d) failed to update MAC address",
					eth_dev->data->port_id);
			goto out_err;
		}
	}

	if (internals->mode == BONDING_MODE_8023AD) {
		if (internals->mode4.dedicated_queues.enabled == 1) {
			internals->mode4.dedicated_queues.rx_qid =
					eth_dev->data->nb_rx_queues;
			internals->mode4.dedicated_queues.tx_qid =
					eth_dev->data->nb_tx_queues;
		}
	}


	/* Reconfigure each member device if starting bonding device */
	for (i = 0; i < internals->member_count; i++) {
		struct rte_eth_dev *member_ethdev =
				&(rte_eth_devices[internals->members[i].port_id]);
		if (member_configure(eth_dev, member_ethdev) != 0) {
			RTE_BOND_LOG(ERR,
				"bonding port (%d) failed to reconfigure member device (%d)",
				eth_dev->data->port_id,
				internals->members[i].port_id);
			goto out_err;
		}
		if (member_start(eth_dev, member_ethdev) != 0) {
			RTE_BOND_LOG(ERR,
				"bonding port (%d) failed to start member device (%d)",
				eth_dev->data->port_id,
				internals->members[i].port_id);
			goto out_err;
		}
		/* We will need to poll for link status if any member doesn't
		 * support interrupts
		 */
		if (internals->members[i].link_status_poll_enabled)
			internals->link_status_polling_enabled = 1;
	}

	/* start polling if needed */
	if (internals->link_status_polling_enabled) {
		rte_eal_alarm_set(
			internals->link_status_polling_interval_ms * 1000,
			bond_ethdev_member_link_status_change_monitor,
			(void *)&rte_eth_devices[internals->port_id]);
	}

	/* Update all member devices MACs*/
	if (mac_address_members_update(eth_dev) != 0)
		goto out_err;

	if (internals->user_defined_primary_port)
		bond_ethdev_primary_set(internals, internals->primary_port);

	if (internals->mode == BONDING_MODE_8023AD)
		bond_mode_8023ad_start(eth_dev);

	if (internals->mode == BONDING_MODE_TLB ||
			internals->mode == BONDING_MODE_ALB)
		bond_tlb_enable(internals);

	for (i = 0; i < eth_dev->data->nb_rx_queues; i++)
		eth_dev->data->rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STARTED;
	for (i = 0; i < eth_dev->data->nb_tx_queues; i++)
		eth_dev->data->tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STARTED;

	return 0;

out_err:
	eth_dev->data->dev_started = 0;
	return -1;
}

static void
bond_ethdev_free_queues(struct rte_eth_dev *dev)
{
	uint16_t i;

	if (dev->data->rx_queues != NULL) {
		for (i = 0; i < dev->data->nb_rx_queues; i++) {
			rte_free(dev->data->rx_queues[i]);
			dev->data->rx_queues[i] = NULL;
		}
		dev->data->nb_rx_queues = 0;
	}

	if (dev->data->tx_queues != NULL) {
		for (i = 0; i < dev->data->nb_tx_queues; i++) {
			rte_free(dev->data->tx_queues[i]);
			dev->data->tx_queues[i] = NULL;
		}
		dev->data->nb_tx_queues = 0;
	}
}

int
bond_ethdev_stop(struct rte_eth_dev *eth_dev)
{
	struct bond_dev_private *internals = eth_dev->data->dev_private;
	uint16_t i;
	int ret;

	if (internals->mode == BONDING_MODE_8023AD) {
		struct port *port;
		void *pkt = NULL;

		bond_mode_8023ad_stop(eth_dev);

		/* Discard all messages to/from mode 4 state machines */
		for (i = 0; i < internals->active_member_count; i++) {
			port = &bond_mode_8023ad_ports[internals->active_members[i]];

			RTE_ASSERT(port->rx_ring != NULL);
			while (rte_ring_dequeue(port->rx_ring, &pkt) != -ENOENT)
				rte_pktmbuf_free(pkt);

			RTE_ASSERT(port->tx_ring != NULL);
			while (rte_ring_dequeue(port->tx_ring, &pkt) != -ENOENT)
				rte_pktmbuf_free(pkt);
		}
	}

	if (internals->mode == BONDING_MODE_TLB ||
			internals->mode == BONDING_MODE_ALB) {
		bond_tlb_disable(internals);
		for (i = 0; i < internals->active_member_count; i++)
			tlb_last_obytets[internals->active_members[i]] = 0;
	}

	eth_dev->data->dev_link.link_status = RTE_ETH_LINK_DOWN;
	eth_dev->data->dev_started = 0;

	if (internals->link_status_polling_enabled) {
		rte_eal_alarm_cancel(bond_ethdev_member_link_status_change_monitor,
			(void *)&rte_eth_devices[internals->port_id]);
	}
	internals->link_status_polling_enabled = 0;
	for (i = 0; i < internals->member_count; i++) {
		uint16_t member_id = internals->members[i].port_id;

		internals->members[i].last_link_status = 0;
		ret = rte_eth_dev_stop(member_id);
		if (ret != 0) {
			RTE_BOND_LOG(ERR, "Failed to stop device on port %u",
				     member_id);
			return ret;
		}

		/* active members need to be deactivated. */
		if (find_member_by_id(internals->active_members,
				internals->active_member_count, member_id) !=
					internals->active_member_count)
			deactivate_member(eth_dev, member_id);
	}

	for (i = 0; i < eth_dev->data->nb_rx_queues; i++)
		eth_dev->data->rx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;
	for (i = 0; i < eth_dev->data->nb_tx_queues; i++)
		eth_dev->data->tx_queue_state[i] = RTE_ETH_QUEUE_STATE_STOPPED;

	return 0;
}

static void
bond_ethdev_cfg_cleanup(struct rte_eth_dev *dev, bool remove)
{
	struct bond_dev_private *internals = dev->data->dev_private;
	uint16_t bond_port_id = internals->port_id;
	int skipped = 0;
	struct rte_flow_error ferror;

	/* Flush flows in all back-end devices before removing them */
	bond_flow_ops.flush(dev, &ferror);

	while (internals->member_count != skipped) {
		uint16_t port_id = internals->members[skipped].port_id;
		int ret;

		ret = rte_eth_dev_stop(port_id);
		if (ret != 0) {
			RTE_BOND_LOG(ERR, "Failed to stop device on port %u",
				     port_id);
		}

		if (ret != 0 || !remove) {
			skipped++;
			continue;
		}

		if (rte_eth_bond_member_remove(bond_port_id, port_id) != 0) {
			RTE_BOND_LOG(ERR,
				     "Failed to remove port %d from bonding device %s",
				     port_id, dev->device->name);
			skipped++;
		}
	}
}

int
bond_ethdev_close(struct rte_eth_dev *dev)
{
	struct bond_dev_private *internals = dev->data->dev_private;

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	RTE_BOND_LOG(INFO, "Closing bonding device %s", dev->device->name);

	bond_ethdev_cfg_cleanup(dev, true);

	bond_ethdev_free_queues(dev);
	rte_bitmap_reset(internals->vlan_filter_bmp);
	rte_bitmap_free(internals->vlan_filter_bmp);
	rte_free(internals->vlan_filter_bmpmem);

	/* Try to release mempool used in mode6. If the bond
	 * device is not mode6, free the NULL is not problem.
	 */
	rte_mempool_free(internals->mode6.mempool);

	rte_kvargs_free(internals->kvlist);

	return 0;
}

/* forward declaration */
static int bond_ethdev_configure(struct rte_eth_dev *dev);

static int
bond_ethdev_info(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{
	struct bond_dev_private *internals = dev->data->dev_private;
	struct bond_member_details member;
	int ret;

	uint16_t max_nb_rx_queues = UINT16_MAX;
	uint16_t max_nb_tx_queues = UINT16_MAX;

	dev_info->max_mac_addrs = BOND_MAX_MAC_ADDRS;

	dev_info->max_rx_pktlen = internals->candidate_max_rx_pktlen ?
			internals->candidate_max_rx_pktlen :
			RTE_ETHER_MAX_JUMBO_FRAME_LEN;

	/* Max number of tx/rx queues that the bonding device can support is the
	 * minimum values of the bonding members, as all members must be capable
	 * of supporting the same number of tx/rx queues.
	 */
	if (internals->member_count > 0) {
		struct rte_eth_dev_info member_info;
		uint16_t idx;

		for (idx = 0; idx < internals->member_count; idx++) {
			member = internals->members[idx];
			ret = rte_eth_dev_info_get(member.port_id, &member_info);
			if (ret != 0) {
				RTE_BOND_LOG(ERR,
					"%s: Error during getting device (port %u) info: %s",
					__func__,
					member.port_id,
					strerror(-ret));

				return ret;
			}

			if (member_info.max_rx_queues < max_nb_rx_queues)
				max_nb_rx_queues = member_info.max_rx_queues;

			if (member_info.max_tx_queues < max_nb_tx_queues)
				max_nb_tx_queues = member_info.max_tx_queues;
		}
	}

	dev_info->max_rx_queues = max_nb_rx_queues;
	dev_info->max_tx_queues = max_nb_tx_queues;

	memcpy(&dev_info->default_rxconf, &internals->default_rxconf,
	       sizeof(dev_info->default_rxconf));
	memcpy(&dev_info->default_txconf, &internals->default_txconf,
	       sizeof(dev_info->default_txconf));

	memcpy(&dev_info->rx_desc_lim, &internals->rx_desc_lim,
	       sizeof(dev_info->rx_desc_lim));
	memcpy(&dev_info->tx_desc_lim, &internals->tx_desc_lim,
	       sizeof(dev_info->tx_desc_lim));

	/**
	 * If dedicated hw queues enabled for link bonding device in LACP mode
	 * then we need to reduce the maximum number of data path queues by 1.
	 */
	if (internals->mode == BONDING_MODE_8023AD &&
		internals->mode4.dedicated_queues.enabled == 1) {
		dev_info->max_rx_queues--;
		dev_info->max_tx_queues--;
	}

	dev_info->min_rx_bufsize = 0;

	dev_info->rx_offload_capa = internals->rx_offload_capa;
	dev_info->tx_offload_capa = internals->tx_offload_capa;
	dev_info->rx_queue_offload_capa = internals->rx_queue_offload_capa;
	dev_info->tx_queue_offload_capa = internals->tx_queue_offload_capa;
	dev_info->flow_type_rss_offloads = internals->flow_type_rss_offloads;

	dev_info->reta_size = internals->reta_size;
	dev_info->hash_key_size = internals->rss_key_len;
	dev_info->speed_capa = internals->speed_capa;

	return 0;
}

static int
bond_ethdev_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	int res;
	uint16_t i;
	struct bond_dev_private *internals = dev->data->dev_private;

	/* don't do this while a member is being added */
	rte_spinlock_lock(&internals->lock);

	if (on)
		rte_bitmap_set(internals->vlan_filter_bmp, vlan_id);
	else
		rte_bitmap_clear(internals->vlan_filter_bmp, vlan_id);

	for (i = 0; i < internals->member_count; i++) {
		uint16_t port_id = internals->members[i].port_id;

		res = rte_eth_dev_vlan_filter(port_id, vlan_id, on);
		if (res == ENOTSUP)
			RTE_BOND_LOG(WARNING,
				     "Setting VLAN filter on member port %u not supported.",
				     port_id);
	}

	rte_spinlock_unlock(&internals->lock);
	return 0;
}

static int
bond_ethdev_rx_queue_setup(struct rte_eth_dev *dev, uint16_t rx_queue_id,
		uint16_t nb_rx_desc, unsigned int socket_id __rte_unused,
		const struct rte_eth_rxconf *rx_conf, struct rte_mempool *mb_pool)
{
	struct bond_rx_queue *bd_rx_q = (struct bond_rx_queue *)
			rte_zmalloc_socket(NULL, sizeof(struct bond_rx_queue),
					0, dev->data->numa_node);
	if (bd_rx_q == NULL)
		return -1;

	bd_rx_q->queue_id = rx_queue_id;
	bd_rx_q->dev_private = dev->data->dev_private;

	bd_rx_q->nb_rx_desc = nb_rx_desc;

	memcpy(&(bd_rx_q->rx_conf), rx_conf, sizeof(struct rte_eth_rxconf));
	bd_rx_q->mb_pool = mb_pool;

	dev->data->rx_queues[rx_queue_id] = bd_rx_q;

	return 0;
}

static int
bond_ethdev_tx_queue_setup(struct rte_eth_dev *dev, uint16_t tx_queue_id,
		uint16_t nb_tx_desc, unsigned int socket_id __rte_unused,
		const struct rte_eth_txconf *tx_conf)
{
	struct bond_tx_queue *bd_tx_q  = (struct bond_tx_queue *)
			rte_zmalloc_socket(NULL, sizeof(struct bond_tx_queue),
					0, dev->data->numa_node);

	if (bd_tx_q == NULL)
		return -1;

	bd_tx_q->queue_id = tx_queue_id;
	bd_tx_q->dev_private = dev->data->dev_private;

	bd_tx_q->nb_tx_desc = nb_tx_desc;
	memcpy(&(bd_tx_q->tx_conf), tx_conf, sizeof(bd_tx_q->tx_conf));

	dev->data->tx_queues[tx_queue_id] = bd_tx_q;

	return 0;
}

static void
bond_ethdev_rx_queue_release(struct rte_eth_dev *dev, uint16_t queue_id)
{
	void *queue = dev->data->rx_queues[queue_id];

	if (queue == NULL)
		return;

	rte_free(queue);
}

static void
bond_ethdev_tx_queue_release(struct rte_eth_dev *dev, uint16_t queue_id)
{
	void *queue = dev->data->tx_queues[queue_id];

	if (queue == NULL)
		return;

	rte_free(queue);
}

static void
bond_ethdev_member_link_status_change_monitor(void *cb_arg)
{
	struct rte_eth_dev *bonding_ethdev, *member_ethdev;
	struct bond_dev_private *internals;

	/* Default value for polling member found is true as we don't want to
	 * disable the polling thread if we cannot get the lock */
	int i, polling_member_found = 1;

	if (cb_arg == NULL)
		return;

	bonding_ethdev = cb_arg;
	internals = bonding_ethdev->data->dev_private;

	if (!bonding_ethdev->data->dev_started ||
		!internals->link_status_polling_enabled)
		return;

	/* If device is currently being configured then don't check members link
	 * status, wait until next period */
	if (rte_spinlock_trylock(&internals->lock)) {
		if (internals->member_count > 0)
			polling_member_found = 0;

		for (i = 0; i < internals->member_count; i++) {
			if (!internals->members[i].link_status_poll_enabled)
				continue;

			member_ethdev = &rte_eth_devices[internals->members[i].port_id];
			polling_member_found = 1;

			/* Update member link status */
			member_ethdev->dev_ops->link_update(member_ethdev,
					      internals->members[i].link_status_wait_to_complete);

			/* if link status has changed since last checked then call lsc
			 * event callback */
			if (member_ethdev->data->dev_link.link_status !=
					internals->members[i].last_link_status) {
				bond_ethdev_lsc_event_callback(internals->members[i].port_id,
						RTE_ETH_EVENT_INTR_LSC,
						&bonding_ethdev->data->port_id,
						NULL);
			}
		}
		rte_spinlock_unlock(&internals->lock);
	}

	if (polling_member_found)
		/* Set alarm to continue monitoring link status of member ethdev's */
		rte_eal_alarm_set(internals->link_status_polling_interval_ms * 1000,
				bond_ethdev_member_link_status_change_monitor, cb_arg);
}

static int
bond_ethdev_link_update(struct rte_eth_dev *ethdev, int wait_to_complete)
{
	int (*link_update)(uint16_t port_id, struct rte_eth_link *eth_link);

	struct bond_dev_private *bond_ctx;
	struct rte_eth_link member_link;

	bool one_link_update_succeeded;
	uint32_t idx;
	int ret;

	bond_ctx = ethdev->data->dev_private;

	ethdev->data->dev_link.link_speed = RTE_ETH_SPEED_NUM_NONE;

	if (ethdev->data->dev_started == 0 ||
			bond_ctx->active_member_count == 0) {
		ethdev->data->dev_link.link_status = RTE_ETH_LINK_DOWN;
		return 0;
	}

	ethdev->data->dev_link.link_status = RTE_ETH_LINK_UP;

	if (wait_to_complete)
		link_update = rte_eth_link_get;
	else
		link_update = rte_eth_link_get_nowait;

	switch (bond_ctx->mode) {
	case BONDING_MODE_BROADCAST:
		/**
		 * Setting link speed to UINT32_MAX to ensure we pick up the
		 * value of the first active member
		 */
		ethdev->data->dev_link.link_speed = UINT32_MAX;

		/**
		 * link speed is minimum value of all the members link speed as
		 * packet loss will occur on this member if transmission at rates
		 * greater than this are attempted
		 */
		for (idx = 0; idx < bond_ctx->active_member_count; idx++) {
			ret = link_update(bond_ctx->active_members[idx],
					  &member_link);
			if (ret < 0) {
				ethdev->data->dev_link.link_speed =
					RTE_ETH_SPEED_NUM_NONE;
				RTE_BOND_LOG(ERR,
					"Member (port %u) link get failed: %s",
					bond_ctx->active_members[idx],
					rte_strerror(-ret));
				return 0;
			}

			if (member_link.link_speed <
					ethdev->data->dev_link.link_speed)
				ethdev->data->dev_link.link_speed =
						member_link.link_speed;
		}
		break;
	case BONDING_MODE_ACTIVE_BACKUP:
		/* Current primary member */
		ret = link_update(bond_ctx->current_primary_port, &member_link);
		if (ret < 0) {
			RTE_BOND_LOG(ERR, "Member (port %u) link get failed: %s",
				bond_ctx->current_primary_port,
				rte_strerror(-ret));
			return 0;
		}

		ethdev->data->dev_link.link_speed = member_link.link_speed;
		break;
	case BONDING_MODE_8023AD:
		ethdev->data->dev_link.link_autoneg =
				bond_ctx->mode4.member_link.link_autoneg;
		ethdev->data->dev_link.link_duplex =
				bond_ctx->mode4.member_link.link_duplex;
		/* fall through */
		/* to update link speed */
	case BONDING_MODE_ROUND_ROBIN:
	case BONDING_MODE_BALANCE:
	case BONDING_MODE_TLB:
	case BONDING_MODE_ALB:
	default:
		/**
		 * In theses mode the maximum theoretical link speed is the sum
		 * of all the members
		 */
		ethdev->data->dev_link.link_speed = RTE_ETH_SPEED_NUM_NONE;
		one_link_update_succeeded = false;

		for (idx = 0; idx < bond_ctx->active_member_count; idx++) {
			ret = link_update(bond_ctx->active_members[idx],
					&member_link);
			if (ret < 0) {
				RTE_BOND_LOG(ERR,
					"Member (port %u) link get failed: %s",
					bond_ctx->active_members[idx],
					rte_strerror(-ret));
				continue;
			}

			one_link_update_succeeded = true;
			ethdev->data->dev_link.link_speed +=
					member_link.link_speed;
		}

		if (!one_link_update_succeeded) {
			RTE_BOND_LOG(ERR, "All members link get failed");
			return 0;
		}
	}


	return 0;
}


static int
bond_ethdev_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	struct bond_dev_private *internals = dev->data->dev_private;
	struct rte_eth_stats member_stats;
	int i, j;

	for (i = 0; i < internals->member_count; i++) {
		rte_eth_stats_get(internals->members[i].port_id, &member_stats);

		stats->ipackets += member_stats.ipackets;
		stats->opackets += member_stats.opackets;
		stats->ibytes += member_stats.ibytes;
		stats->obytes += member_stats.obytes;
		stats->imissed += member_stats.imissed;
		stats->ierrors += member_stats.ierrors;
		stats->oerrors += member_stats.oerrors;
		stats->rx_nombuf += member_stats.rx_nombuf;

		for (j = 0; j < RTE_ETHDEV_QUEUE_STAT_CNTRS; j++) {
			stats->q_ipackets[j] += member_stats.q_ipackets[j];
			stats->q_opackets[j] += member_stats.q_opackets[j];
			stats->q_ibytes[j] += member_stats.q_ibytes[j];
			stats->q_obytes[j] += member_stats.q_obytes[j];
			stats->q_errors[j] += member_stats.q_errors[j];
		}

	}

	return 0;
}

static int
bond_ethdev_stats_reset(struct rte_eth_dev *dev)
{
	struct bond_dev_private *internals = dev->data->dev_private;
	int i;
	int err;
	int ret;

	for (i = 0, err = 0; i < internals->member_count; i++) {
		ret = rte_eth_stats_reset(internals->members[i].port_id);
		if (ret != 0)
			err = ret;
	}

	return err;
}

static int
bond_ethdev_promiscuous_enable(struct rte_eth_dev *eth_dev)
{
	struct bond_dev_private *internals = eth_dev->data->dev_private;
	int i;
	int ret = 0;
	uint16_t port_id;

	switch (internals->mode) {
	/* Promiscuous mode is propagated to all members */
	case BONDING_MODE_ROUND_ROBIN:
	case BONDING_MODE_BALANCE:
	case BONDING_MODE_BROADCAST:
	case BONDING_MODE_8023AD: {
		unsigned int member_ok = 0;

		for (i = 0; i < internals->member_count; i++) {
			port_id = internals->members[i].port_id;

			ret = rte_eth_promiscuous_enable(port_id);
			if (ret != 0)
				RTE_BOND_LOG(ERR,
					"Failed to enable promiscuous mode for port %u: %s",
					port_id, rte_strerror(-ret));
			else
				member_ok++;
		}
		/*
		 * Report success if operation is successful on at least
		 * on one member. Otherwise return last error code.
		 */
		if (member_ok > 0)
			ret = 0;
		break;
	}
	/* Promiscuous mode is propagated only to primary member */
	case BONDING_MODE_ACTIVE_BACKUP:
	case BONDING_MODE_TLB:
	case BONDING_MODE_ALB:
	default:
		/* Do not touch promisc when there cannot be primary ports */
		if (internals->member_count == 0)
			break;
		port_id = internals->current_primary_port;
		ret = rte_eth_promiscuous_enable(port_id);
		if (ret != 0)
			RTE_BOND_LOG(ERR,
				"Failed to enable promiscuous mode for port %u: %s",
				port_id, rte_strerror(-ret));
	}

	return ret;
}

static int
bond_ethdev_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct bond_dev_private *internals = dev->data->dev_private;
	int i;
	int ret = 0;
	uint16_t port_id;

	switch (internals->mode) {
	/* Promiscuous mode is propagated to all members */
	case BONDING_MODE_ROUND_ROBIN:
	case BONDING_MODE_BALANCE:
	case BONDING_MODE_BROADCAST:
	case BONDING_MODE_8023AD: {
		unsigned int member_ok = 0;

		for (i = 0; i < internals->member_count; i++) {
			port_id = internals->members[i].port_id;

			if (internals->mode == BONDING_MODE_8023AD &&
			    bond_mode_8023ad_ports[port_id].forced_rx_flags ==
					BOND_8023AD_FORCED_PROMISC) {
				member_ok++;
				continue;
			}
			ret = rte_eth_promiscuous_disable(port_id);
			if (ret != 0)
				RTE_BOND_LOG(ERR,
					"Failed to disable promiscuous mode for port %u: %s",
					port_id, rte_strerror(-ret));
			else
				member_ok++;
		}
		/*
		 * Report success if operation is successful on at least
		 * on one member. Otherwise return last error code.
		 */
		if (member_ok > 0)
			ret = 0;
		break;
	}
	/* Promiscuous mode is propagated only to primary member */
	case BONDING_MODE_ACTIVE_BACKUP:
	case BONDING_MODE_TLB:
	case BONDING_MODE_ALB:
	default:
		/* Do not touch promisc when there cannot be primary ports */
		if (internals->member_count == 0)
			break;
		port_id = internals->current_primary_port;
		ret = rte_eth_promiscuous_disable(port_id);
		if (ret != 0)
			RTE_BOND_LOG(ERR,
				"Failed to disable promiscuous mode for port %u: %s",
				port_id, rte_strerror(-ret));
	}

	return ret;
}

static int
bond_ethdev_promiscuous_update(struct rte_eth_dev *dev)
{
	struct bond_dev_private *internals = dev->data->dev_private;
	uint16_t port_id = internals->current_primary_port;
	int ret;

	switch (internals->mode) {
	case BONDING_MODE_ROUND_ROBIN:
	case BONDING_MODE_BALANCE:
	case BONDING_MODE_BROADCAST:
	case BONDING_MODE_8023AD:
		/* As promiscuous mode is propagated to all members for these
		 * mode, no need to update for bonding device.
		 */
		break;
	case BONDING_MODE_ACTIVE_BACKUP:
	case BONDING_MODE_TLB:
	case BONDING_MODE_ALB:
	default:
		/* As promiscuous mode is propagated only to primary member
		 * for these mode. When active/standby switchover, promiscuous
		 * mode should be set to new primary member according to bonding
		 * device.
		 */
		if (rte_eth_promiscuous_get(internals->port_id) == 1) {
			ret = rte_eth_promiscuous_enable(port_id);
			if (ret != 0)
				RTE_BOND_LOG(ERR,
					     "Failed to enable promiscuous mode for port %u: %s",
					     port_id, rte_strerror(-ret));
		} else {
			ret = rte_eth_promiscuous_disable(port_id);
			if (ret != 0)
				RTE_BOND_LOG(ERR,
					     "Failed to disable promiscuous mode for port %u: %s",
					     port_id, rte_strerror(-ret));
		}
	}

	return 0;
}

static int
bond_ethdev_allmulticast_enable(struct rte_eth_dev *eth_dev)
{
	struct bond_dev_private *internals = eth_dev->data->dev_private;
	int i;
	int ret = 0;
	uint16_t port_id;

	switch (internals->mode) {
	/* allmulti mode is propagated to all members */
	case BONDING_MODE_ROUND_ROBIN:
	case BONDING_MODE_BALANCE:
	case BONDING_MODE_BROADCAST:
	case BONDING_MODE_8023AD: {
		unsigned int member_ok = 0;

		for (i = 0; i < internals->member_count; i++) {
			port_id = internals->members[i].port_id;

			ret = rte_eth_allmulticast_enable(port_id);
			if (ret != 0)
				RTE_BOND_LOG(ERR,
					"Failed to enable allmulti mode for port %u: %s",
					port_id, rte_strerror(-ret));
			else
				member_ok++;
		}
		/*
		 * Report success if operation is successful on at least
		 * on one member. Otherwise return last error code.
		 */
		if (member_ok > 0)
			ret = 0;
		break;
	}
	/* allmulti mode is propagated only to primary member */
	case BONDING_MODE_ACTIVE_BACKUP:
	case BONDING_MODE_TLB:
	case BONDING_MODE_ALB:
	default:
		/* Do not touch allmulti when there cannot be primary ports */
		if (internals->member_count == 0)
			break;
		port_id = internals->current_primary_port;
		ret = rte_eth_allmulticast_enable(port_id);
		if (ret != 0)
			RTE_BOND_LOG(ERR,
				"Failed to enable allmulti mode for port %u: %s",
				port_id, rte_strerror(-ret));
	}

	return ret;
}

static int
bond_ethdev_allmulticast_disable(struct rte_eth_dev *eth_dev)
{
	struct bond_dev_private *internals = eth_dev->data->dev_private;
	int i;
	int ret = 0;
	uint16_t port_id;

	switch (internals->mode) {
	/* allmulti mode is propagated to all members */
	case BONDING_MODE_ROUND_ROBIN:
	case BONDING_MODE_BALANCE:
	case BONDING_MODE_BROADCAST:
	case BONDING_MODE_8023AD: {
		unsigned int member_ok = 0;

		for (i = 0; i < internals->member_count; i++) {
			uint16_t port_id = internals->members[i].port_id;

			if (internals->mode == BONDING_MODE_8023AD &&
			    bond_mode_8023ad_ports[port_id].forced_rx_flags ==
					BOND_8023AD_FORCED_ALLMULTI)
				continue;

			ret = rte_eth_allmulticast_disable(port_id);
			if (ret != 0)
				RTE_BOND_LOG(ERR,
					"Failed to disable allmulti mode for port %u: %s",
					port_id, rte_strerror(-ret));
			else
				member_ok++;
		}
		/*
		 * Report success if operation is successful on at least
		 * on one member. Otherwise return last error code.
		 */
		if (member_ok > 0)
			ret = 0;
		break;
	}
	/* allmulti mode is propagated only to primary member */
	case BONDING_MODE_ACTIVE_BACKUP:
	case BONDING_MODE_TLB:
	case BONDING_MODE_ALB:
	default:
		/* Do not touch allmulti when there cannot be primary ports */
		if (internals->member_count == 0)
			break;
		port_id = internals->current_primary_port;
		ret = rte_eth_allmulticast_disable(port_id);
		if (ret != 0)
			RTE_BOND_LOG(ERR,
				"Failed to disable allmulti mode for port %u: %s",
				port_id, rte_strerror(-ret));
	}

	return ret;
}

static int
bond_ethdev_allmulticast_update(struct rte_eth_dev *dev)
{
	struct bond_dev_private *internals = dev->data->dev_private;
	uint16_t port_id = internals->current_primary_port;

	switch (internals->mode) {
	case BONDING_MODE_ROUND_ROBIN:
	case BONDING_MODE_BALANCE:
	case BONDING_MODE_BROADCAST:
	case BONDING_MODE_8023AD:
		/* As allmulticast mode is propagated to all members for these
		 * mode, no need to update for bonding device.
		 */
		break;
	case BONDING_MODE_ACTIVE_BACKUP:
	case BONDING_MODE_TLB:
	case BONDING_MODE_ALB:
	default:
		/* As allmulticast mode is propagated only to primary member
		 * for these mode. When active/standby switchover, allmulticast
		 * mode should be set to new primary member according to bonding
		 * device.
		 */
		if (rte_eth_allmulticast_get(internals->port_id) == 1)
			rte_eth_allmulticast_enable(port_id);
		else
			rte_eth_allmulticast_disable(port_id);
	}

	return 0;
}

static void
bond_ethdev_delayed_lsc_propagation(void *arg)
{
	if (arg == NULL)
		return;

	rte_eth_dev_callback_process((struct rte_eth_dev *)arg,
			RTE_ETH_EVENT_INTR_LSC, NULL);
}

int
bond_ethdev_lsc_event_callback(uint16_t port_id, enum rte_eth_event_type type,
		void *param, void *ret_param __rte_unused)
{
	struct rte_eth_dev *bonding_eth_dev;
	struct bond_dev_private *internals;
	struct rte_eth_link link;
	int rc = -1;
	int ret;

	uint8_t lsc_flag = 0;
	int valid_member = 0;
	uint16_t active_pos, member_idx;
	uint16_t i;

	if (type != RTE_ETH_EVENT_INTR_LSC || param == NULL)
		return rc;

	bonding_eth_dev = &rte_eth_devices[*(uint16_t *)param];

	if (check_for_bonding_ethdev(bonding_eth_dev))
		return rc;

	internals = bonding_eth_dev->data->dev_private;

	/* If the device isn't started don't handle interrupts */
	if (!bonding_eth_dev->data->dev_started)
		return rc;

	/* verify that port_id is a valid member of bonding port */
	for (i = 0; i < internals->member_count; i++) {
		if (internals->members[i].port_id == port_id) {
			valid_member = 1;
			member_idx = i;
			break;
		}
	}

	if (!valid_member)
		return rc;

	/* Synchronize lsc callback parallel calls either by real link event
	 * from the members PMDs or by the bonding PMD itself.
	 */
	rte_spinlock_lock(&internals->lsc_lock);

	/* Search for port in active port list */
	active_pos = find_member_by_id(internals->active_members,
			internals->active_member_count, port_id);

	ret = rte_eth_link_get_nowait(port_id, &link);
	if (ret < 0)
		RTE_BOND_LOG(ERR, "Member (port %u) link get failed", port_id);

	if (ret == 0 && link.link_status) {
		if (active_pos < internals->active_member_count)
			goto link_update;

		/* check link state properties if bonding link is up*/
		if (bonding_eth_dev->data->dev_link.link_status == RTE_ETH_LINK_UP) {
			if (link_properties_valid(bonding_eth_dev, &link) != 0)
				RTE_BOND_LOG(ERR, "Invalid link properties "
					     "for member %d in bonding mode %d",
					     port_id, internals->mode);
		} else {
			/* inherit member link properties */
			link_properties_set(bonding_eth_dev, &link);
		}

		/* If no active member ports then set this port to be
		 * the primary port.
		 */
		if (internals->active_member_count < 1) {
			/* If first active member, then change link status */
			bonding_eth_dev->data->dev_link.link_status =
								RTE_ETH_LINK_UP;
			internals->current_primary_port = port_id;
			lsc_flag = 1;

			mac_address_members_update(bonding_eth_dev);
			bond_ethdev_promiscuous_update(bonding_eth_dev);
			bond_ethdev_allmulticast_update(bonding_eth_dev);
		}

		activate_member(bonding_eth_dev, port_id);

		/* If the user has defined the primary port then default to
		 * using it.
		 */
		if (internals->user_defined_primary_port &&
				internals->primary_port == port_id)
			bond_ethdev_primary_set(internals, port_id);
	} else {
		if (active_pos == internals->active_member_count)
			goto link_update;

		/* Remove from active member list */
		deactivate_member(bonding_eth_dev, port_id);

		if (internals->active_member_count < 1)
			lsc_flag = 1;

		/* Update primary id, take first active member from list or if none
		 * available set to -1 */
		if (port_id == internals->current_primary_port) {
			if (internals->active_member_count > 0)
				bond_ethdev_primary_set(internals,
						internals->active_members[0]);
			else
				internals->current_primary_port = internals->primary_port;
			mac_address_members_update(bonding_eth_dev);
			bond_ethdev_promiscuous_update(bonding_eth_dev);
			bond_ethdev_allmulticast_update(bonding_eth_dev);
		}
	}

link_update:
	/**
	 * Update bonding device link properties after any change to active
	 * members
	 */
	bond_ethdev_link_update(bonding_eth_dev, 0);
	internals->members[member_idx].last_link_status = link.link_status;

	if (lsc_flag) {
		/* Cancel any possible outstanding interrupts if delays are enabled */
		if (internals->link_up_delay_ms > 0 ||
			internals->link_down_delay_ms > 0)
			rte_eal_alarm_cancel(bond_ethdev_delayed_lsc_propagation,
					bonding_eth_dev);

		if (bonding_eth_dev->data->dev_link.link_status) {
			if (internals->link_up_delay_ms > 0)
				rte_eal_alarm_set(internals->link_up_delay_ms * 1000,
						bond_ethdev_delayed_lsc_propagation,
						(void *)bonding_eth_dev);
			else
				rte_eth_dev_callback_process(bonding_eth_dev,
						RTE_ETH_EVENT_INTR_LSC,
						NULL);

		} else {
			if (internals->link_down_delay_ms > 0)
				rte_eal_alarm_set(internals->link_down_delay_ms * 1000,
						bond_ethdev_delayed_lsc_propagation,
						(void *)bonding_eth_dev);
			else
				rte_eth_dev_callback_process(bonding_eth_dev,
						RTE_ETH_EVENT_INTR_LSC,
						NULL);
		}
	}

	rte_spinlock_unlock(&internals->lsc_lock);

	return rc;
}

static int
bond_ethdev_rss_reta_update(struct rte_eth_dev *dev,
		struct rte_eth_rss_reta_entry64 *reta_conf, uint16_t reta_size)
{
	unsigned i, j;
	int result = 0;
	int member_reta_size;
	unsigned reta_count;
	struct bond_dev_private *internals = dev->data->dev_private;

	if (reta_size != internals->reta_size)
		return -EINVAL;

	 /* Copy RETA table */
	reta_count = (reta_size + RTE_ETH_RETA_GROUP_SIZE - 1) /
			RTE_ETH_RETA_GROUP_SIZE;

	for (i = 0; i < reta_count; i++) {
		internals->reta_conf[i].mask = reta_conf[i].mask;
		for (j = 0; j < RTE_ETH_RETA_GROUP_SIZE; j++)
			if ((reta_conf[i].mask >> j) & 0x01)
				internals->reta_conf[i].reta[j] = reta_conf[i].reta[j];
	}

	/* Fill rest of array */
	for (; i < RTE_DIM(internals->reta_conf); i += reta_count)
		memcpy(&internals->reta_conf[i], &internals->reta_conf[0],
				sizeof(internals->reta_conf[0]) * reta_count);

	/* Propagate RETA over members */
	for (i = 0; i < internals->member_count; i++) {
		member_reta_size = internals->members[i].reta_size;
		result = rte_eth_dev_rss_reta_update(internals->members[i].port_id,
				&internals->reta_conf[0], member_reta_size);
		if (result < 0)
			return result;
	}

	return 0;
}

static int
bond_ethdev_rss_reta_query(struct rte_eth_dev *dev,
		struct rte_eth_rss_reta_entry64 *reta_conf, uint16_t reta_size)
{
	int i, j;
	struct bond_dev_private *internals = dev->data->dev_private;

	if (reta_size != internals->reta_size)
		return -EINVAL;

	 /* Copy RETA table */
	for (i = 0; i < reta_size / RTE_ETH_RETA_GROUP_SIZE; i++)
		for (j = 0; j < RTE_ETH_RETA_GROUP_SIZE; j++)
			if ((reta_conf[i].mask >> j) & 0x01)
				reta_conf[i].reta[j] = internals->reta_conf[i].reta[j];

	return 0;
}

static int
bond_ethdev_rss_hash_update(struct rte_eth_dev *dev,
		struct rte_eth_rss_conf *rss_conf)
{
	int i, result = 0;
	struct bond_dev_private *internals = dev->data->dev_private;
	struct rte_eth_rss_conf bond_rss_conf;

	bond_rss_conf = *rss_conf;

	bond_rss_conf.rss_hf &= internals->flow_type_rss_offloads;

	if (bond_rss_conf.rss_hf != 0)
		dev->data->dev_conf.rx_adv_conf.rss_conf.rss_hf = bond_rss_conf.rss_hf;

	if (bond_rss_conf.rss_key) {
		if (bond_rss_conf.rss_key_len < internals->rss_key_len)
			return -EINVAL;
		else if (bond_rss_conf.rss_key_len > internals->rss_key_len)
			RTE_BOND_LOG(WARNING, "rss_key will be truncated");

		memcpy(internals->rss_key, bond_rss_conf.rss_key,
				internals->rss_key_len);
		bond_rss_conf.rss_key_len = internals->rss_key_len;
	}

	for (i = 0; i < internals->member_count; i++) {
		result = rte_eth_dev_rss_hash_update(internals->members[i].port_id,
				&bond_rss_conf);
		if (result < 0)
			return result;
	}

	return 0;
}

static int
bond_ethdev_rss_hash_conf_get(struct rte_eth_dev *dev,
		struct rte_eth_rss_conf *rss_conf)
{
	struct bond_dev_private *internals = dev->data->dev_private;

	rss_conf->rss_hf = dev->data->dev_conf.rx_adv_conf.rss_conf.rss_hf;
	rss_conf->rss_key_len = internals->rss_key_len;
	if (rss_conf->rss_key)
		memcpy(rss_conf->rss_key, internals->rss_key, internals->rss_key_len);

	return 0;
}

static int
bond_ethdev_mtu_set(struct rte_eth_dev *dev, uint16_t mtu)
{
	struct rte_eth_dev *member_eth_dev;
	struct bond_dev_private *internals = dev->data->dev_private;
	int ret, i;

	rte_spinlock_lock(&internals->lock);

	for (i = 0; i < internals->member_count; i++) {
		member_eth_dev = &rte_eth_devices[internals->members[i].port_id];
		if (member_eth_dev->dev_ops->mtu_set == NULL) {
			rte_spinlock_unlock(&internals->lock);
			return -ENOTSUP;
		}
	}
	for (i = 0; i < internals->member_count; i++) {
		ret = rte_eth_dev_set_mtu(internals->members[i].port_id, mtu);
		if (ret < 0) {
			rte_spinlock_unlock(&internals->lock);
			return ret;
		}
	}

	rte_spinlock_unlock(&internals->lock);
	return 0;
}

static int
bond_ethdev_mac_address_set(struct rte_eth_dev *dev,
			struct rte_ether_addr *addr)
{
	if (mac_address_set(dev, addr)) {
		RTE_BOND_LOG(ERR, "Failed to update MAC address");
		return -EINVAL;
	}

	return 0;
}

static int
bond_flow_ops_get(struct rte_eth_dev *dev __rte_unused,
		  const struct rte_flow_ops **ops)
{
	*ops = &bond_flow_ops;
	return 0;
}

static int
bond_ethdev_mac_addr_add(struct rte_eth_dev *dev,
			struct rte_ether_addr *mac_addr,
			__rte_unused uint32_t index, uint32_t vmdq)
{
	struct rte_eth_dev *member_eth_dev;
	struct bond_dev_private *internals = dev->data->dev_private;
	int ret, i;

	rte_spinlock_lock(&internals->lock);

	for (i = 0; i < internals->member_count; i++) {
		member_eth_dev = &rte_eth_devices[internals->members[i].port_id];
		if (member_eth_dev->dev_ops->mac_addr_add == NULL ||
		    member_eth_dev->dev_ops->mac_addr_remove == NULL) {
			ret = -ENOTSUP;
			goto end;
		}
	}

	for (i = 0; i < internals->member_count; i++) {
		ret = rte_eth_dev_mac_addr_add(internals->members[i].port_id,
				mac_addr, vmdq);
		if (ret < 0) {
			/* rollback */
			for (i--; i >= 0; i--)
				rte_eth_dev_mac_addr_remove(
					internals->members[i].port_id, mac_addr);
			goto end;
		}
	}

	ret = 0;
end:
	rte_spinlock_unlock(&internals->lock);
	return ret;
}

static void
bond_ethdev_mac_addr_remove(struct rte_eth_dev *dev, uint32_t index)
{
	struct rte_eth_dev *member_eth_dev;
	struct bond_dev_private *internals = dev->data->dev_private;
	int i;

	rte_spinlock_lock(&internals->lock);

	for (i = 0; i < internals->member_count; i++) {
		member_eth_dev = &rte_eth_devices[internals->members[i].port_id];
		if (member_eth_dev->dev_ops->mac_addr_remove == NULL)
			goto end;
	}

	struct rte_ether_addr *mac_addr = &dev->data->mac_addrs[index];

	for (i = 0; i < internals->member_count; i++)
		rte_eth_dev_mac_addr_remove(internals->members[i].port_id,
				mac_addr);

end:
	rte_spinlock_unlock(&internals->lock);
}

static const char *
bond_mode_name(uint8_t mode)
{
	switch (mode) {
	case BONDING_MODE_ROUND_ROBIN:
		return "ROUND_ROBIN";
	case BONDING_MODE_ACTIVE_BACKUP:
		return "ACTIVE_BACKUP";
	case BONDING_MODE_BALANCE:
		return "BALANCE";
	case BONDING_MODE_BROADCAST:
		return "BROADCAST";
	case BONDING_MODE_8023AD:
		return "8023AD";
	case BONDING_MODE_TLB:
		return "TLB";
	case BONDING_MODE_ALB:
		return "ALB";
	default:
		return "Unknown";
	}
}

static void
dump_basic(const struct rte_eth_dev *dev, FILE *f)
{
	struct bond_dev_private instant_priv;
	const struct bond_dev_private *internals = &instant_priv;
	int mode, i;

	/* Obtain a instance of dev_private to prevent data from being modified. */
	memcpy(&instant_priv, dev->data->dev_private, sizeof(struct bond_dev_private));
	mode = internals->mode;

	fprintf(f, "  - Dev basic:\n");
	fprintf(f, "\tBonding mode: %s(%d)\n", bond_mode_name(mode), mode);

	if (mode == BONDING_MODE_BALANCE || mode == BONDING_MODE_8023AD) {
		fprintf(f, "\tBalance Xmit Policy: ");
		switch (internals->balance_xmit_policy) {
		case BALANCE_XMIT_POLICY_LAYER2:
			fprintf(f, "BALANCE_XMIT_POLICY_LAYER2");
			break;
		case BALANCE_XMIT_POLICY_LAYER23:
			fprintf(f, "BALANCE_XMIT_POLICY_LAYER23");
			break;
		case BALANCE_XMIT_POLICY_LAYER34:
			fprintf(f, "BALANCE_XMIT_POLICY_LAYER34");
			break;
		default:
			fprintf(f, "Unknown");
		}
		fprintf(f, "\n");
	}

	if (mode == BONDING_MODE_8023AD) {
		fprintf(f, "\tIEEE802.3AD Aggregator Mode: ");
		switch (internals->mode4.agg_selection) {
		case AGG_BANDWIDTH:
			fprintf(f, "bandwidth");
			break;
		case AGG_STABLE:
			fprintf(f, "stable");
			break;
		case AGG_COUNT:
			fprintf(f, "count");
			break;
		default:
			fprintf(f, "unknown");
		}
		fprintf(f, "\n");
	}

	if (internals->member_count > 0) {
		fprintf(f, "\tMembers (%u): [", internals->member_count);
		for (i = 0; i < internals->member_count - 1; i++)
			fprintf(f, "%u ", internals->members[i].port_id);

		fprintf(f, "%u]\n", internals->members[internals->member_count - 1].port_id);
	} else {
		fprintf(f, "\tMembers: []\n");
	}

	if (internals->active_member_count > 0) {
		fprintf(f, "\tActive Members (%u): [", internals->active_member_count);
		for (i = 0; i < internals->active_member_count - 1; i++)
			fprintf(f, "%u ", internals->active_members[i]);

		fprintf(f, "%u]\n", internals->active_members[internals->active_member_count - 1]);

	} else {
		fprintf(f, "\tActive Members: []\n");
	}

	if (internals->user_defined_primary_port)
		fprintf(f, "\tUser Defined Primary: [%u]\n", internals->primary_port);
	if (internals->member_count > 0)
		fprintf(f, "\tCurrent Primary: [%u]\n", internals->current_primary_port);
}

static void
dump_lacp_conf(const struct rte_eth_bond_8023ad_conf *conf, FILE *f)
{
	fprintf(f, "\tfast period: %u ms\n", conf->fast_periodic_ms);
	fprintf(f, "\tslow period: %u ms\n", conf->slow_periodic_ms);
	fprintf(f, "\tshort timeout: %u ms\n", conf->short_timeout_ms);
	fprintf(f, "\tlong timeout: %u ms\n", conf->long_timeout_ms);
	fprintf(f, "\taggregate wait timeout: %u ms\n",
			conf->aggregate_wait_timeout_ms);
	fprintf(f, "\ttx period: %u ms\n", conf->tx_period_ms);
	fprintf(f, "\trx marker period: %u ms\n", conf->rx_marker_period_ms);
	fprintf(f, "\tupdate timeout: %u ms\n", conf->update_timeout_ms);
	switch (conf->agg_selection) {
	case AGG_BANDWIDTH:
		fprintf(f, "\taggregation mode: bandwidth\n");
		break;
	case AGG_STABLE:
		fprintf(f, "\taggregation mode: stable\n");
		break;
	case AGG_COUNT:
		fprintf(f, "\taggregation mode: count\n");
		break;
	default:
		fprintf(f, "\taggregation mode: invalid\n");
		break;
	}
	fprintf(f, "\n");
}

static void
dump_lacp_port_param(const struct port_params *params, FILE *f)
{
	char buf[RTE_ETHER_ADDR_FMT_SIZE];
	fprintf(f, "\t\tsystem priority: %u\n", params->system_priority);
	rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, &params->system);
	fprintf(f, "\t\tsystem mac address: %s\n", buf);
	fprintf(f, "\t\tport key: %u\n", params->key);
	fprintf(f, "\t\tport priority: %u\n", params->port_priority);
	fprintf(f, "\t\tport number: %u\n", params->port_number);
}

static void
dump_lacp_member(const struct rte_eth_bond_8023ad_member_info *info, FILE *f)
{
	char a_state[256] = { 0 };
	char p_state[256] = { 0 };
	int a_len = 0;
	int p_len = 0;
	uint32_t i;

	static const char * const state[] = {
		"ACTIVE",
		"TIMEOUT",
		"AGGREGATION",
		"SYNCHRONIZATION",
		"COLLECTING",
		"DISTRIBUTING",
		"DEFAULTED",
		"EXPIRED"
	};
	static const char * const selection[] = {
		"UNSELECTED",
		"STANDBY",
		"SELECTED"
	};

	for (i = 0; i < RTE_DIM(state); i++) {
		if ((info->actor_state >> i) & 1)
			a_len += snprintf(&a_state[a_len],
						RTE_DIM(a_state) - a_len, "%s ",
						state[i]);

		if ((info->partner_state >> i) & 1)
			p_len += snprintf(&p_state[p_len],
						RTE_DIM(p_state) - p_len, "%s ",
						state[i]);
	}
	fprintf(f, "\tAggregator port id: %u\n", info->agg_port_id);
	fprintf(f, "\tselection: %s\n", selection[info->selected]);
	fprintf(f, "\tActor detail info:\n");
	dump_lacp_port_param(&info->actor, f);
	fprintf(f, "\t\tport state: %s\n", a_state);
	fprintf(f, "\tPartner detail info:\n");
	dump_lacp_port_param(&info->partner, f);
	fprintf(f, "\t\tport state: %s\n", p_state);
	fprintf(f, "\n");
}

static void
dump_lacp(uint16_t port_id, FILE *f)
{
	struct rte_eth_bond_8023ad_member_info member_info;
	struct rte_eth_bond_8023ad_conf port_conf;
	uint16_t members[RTE_MAX_ETHPORTS];
	int num_active_members;
	int i, ret;

	fprintf(f, "  - Lacp info:\n");

	num_active_members = rte_eth_bond_active_members_get(port_id, members,
			RTE_MAX_ETHPORTS);
	if (num_active_members < 0) {
		fprintf(f, "\tFailed to get active member list for port %u\n",
				port_id);
		return;
	}

	fprintf(f, "\tIEEE802.3 port: %u\n", port_id);
	ret = rte_eth_bond_8023ad_conf_get(port_id, &port_conf);
	if (ret) {
		fprintf(f, "\tGet bonding device %u 8023ad config failed\n",
			port_id);
		return;
	}
	dump_lacp_conf(&port_conf, f);

	for (i = 0; i < num_active_members; i++) {
		ret = rte_eth_bond_8023ad_member_info(port_id, members[i],
				&member_info);
		if (ret) {
			fprintf(f, "\tGet member device %u 8023ad info failed\n",
				members[i]);
			return;
		}
		fprintf(f, "\tMember Port: %u\n", members[i]);
		dump_lacp_member(&member_info, f);
	}
}

static int
bond_ethdev_priv_dump(struct rte_eth_dev *dev, FILE *f)
{
	const struct bond_dev_private *internals = dev->data->dev_private;

	dump_basic(dev, f);
	if (internals->mode == BONDING_MODE_8023AD)
		dump_lacp(dev->data->port_id, f);

	return 0;
}

const struct eth_dev_ops default_dev_ops = {
	.dev_start            = bond_ethdev_start,
	.dev_stop             = bond_ethdev_stop,
	.dev_close            = bond_ethdev_close,
	.dev_configure        = bond_ethdev_configure,
	.dev_infos_get        = bond_ethdev_info,
	.vlan_filter_set      = bond_ethdev_vlan_filter_set,
	.rx_queue_setup       = bond_ethdev_rx_queue_setup,
	.tx_queue_setup       = bond_ethdev_tx_queue_setup,
	.rx_queue_release     = bond_ethdev_rx_queue_release,
	.tx_queue_release     = bond_ethdev_tx_queue_release,
	.link_update          = bond_ethdev_link_update,
	.stats_get            = bond_ethdev_stats_get,
	.stats_reset          = bond_ethdev_stats_reset,
	.promiscuous_enable   = bond_ethdev_promiscuous_enable,
	.promiscuous_disable  = bond_ethdev_promiscuous_disable,
	.allmulticast_enable  = bond_ethdev_allmulticast_enable,
	.allmulticast_disable = bond_ethdev_allmulticast_disable,
	.reta_update          = bond_ethdev_rss_reta_update,
	.reta_query           = bond_ethdev_rss_reta_query,
	.rss_hash_update      = bond_ethdev_rss_hash_update,
	.rss_hash_conf_get    = bond_ethdev_rss_hash_conf_get,
	.mtu_set              = bond_ethdev_mtu_set,
	.mac_addr_set         = bond_ethdev_mac_address_set,
	.mac_addr_add         = bond_ethdev_mac_addr_add,
	.mac_addr_remove      = bond_ethdev_mac_addr_remove,
	.flow_ops_get         = bond_flow_ops_get,
	.eth_dev_priv_dump    = bond_ethdev_priv_dump,
};

static int
bond_alloc(struct rte_vdev_device *dev, uint8_t mode)
{
	const char *name = rte_vdev_device_name(dev);
	int socket_id = dev->device.numa_node;
	struct bond_dev_private *internals = NULL;
	struct rte_eth_dev *eth_dev = NULL;
	uint32_t vlan_filter_bmp_size;

	/* now do all data allocation - for eth_dev structure, dummy pci driver
	 * and internal (private) data
	 */

	/* reserve an ethdev entry */
	eth_dev = rte_eth_vdev_allocate(dev, sizeof(*internals));
	if (eth_dev == NULL) {
		RTE_BOND_LOG(ERR, "Unable to allocate rte_eth_dev");
		goto err;
	}

	internals = eth_dev->data->dev_private;
	eth_dev->data->nb_rx_queues = (uint16_t)1;
	eth_dev->data->nb_tx_queues = (uint16_t)1;

	/* Allocate memory for storing MAC addresses */
	eth_dev->data->mac_addrs = rte_zmalloc_socket(name, RTE_ETHER_ADDR_LEN *
			BOND_MAX_MAC_ADDRS, 0, socket_id);
	if (eth_dev->data->mac_addrs == NULL) {
		RTE_BOND_LOG(ERR,
			     "Failed to allocate %u bytes needed to store MAC addresses",
			     RTE_ETHER_ADDR_LEN * BOND_MAX_MAC_ADDRS);
		goto err;
	}

	eth_dev->dev_ops = &default_dev_ops;
	eth_dev->data->dev_flags = RTE_ETH_DEV_INTR_LSC |
					RTE_ETH_DEV_AUTOFILL_QUEUE_XSTATS;

	rte_spinlock_init(&internals->lock);
	rte_spinlock_init(&internals->lsc_lock);

	internals->port_id = eth_dev->data->port_id;
	internals->mode = BONDING_MODE_INVALID;
	internals->current_primary_port = RTE_MAX_ETHPORTS + 1;
	internals->balance_xmit_policy = BALANCE_XMIT_POLICY_LAYER2;
	internals->burst_xmit_hash = burst_xmit_l2_hash;
	internals->user_defined_mac = 0;

	internals->link_status_polling_enabled = 0;

	internals->link_status_polling_interval_ms =
		DEFAULT_POLLING_INTERVAL_10_MS;
	internals->link_down_delay_ms = 0;
	internals->link_up_delay_ms = 0;

	internals->member_count = 0;
	internals->active_member_count = 0;
	internals->rx_offload_capa = 0;
	internals->tx_offload_capa = 0;
	internals->rx_queue_offload_capa = 0;
	internals->tx_queue_offload_capa = 0;
	internals->candidate_max_rx_pktlen = 0;
	internals->max_rx_pktlen = 0;

	/* Initially allow to choose any offload type */
	internals->flow_type_rss_offloads = RTE_ETH_RSS_PROTO_MASK;

	memset(&internals->default_rxconf, 0,
	       sizeof(internals->default_rxconf));
	memset(&internals->default_txconf, 0,
	       sizeof(internals->default_txconf));

	memset(&internals->rx_desc_lim, 0, sizeof(internals->rx_desc_lim));
	memset(&internals->tx_desc_lim, 0, sizeof(internals->tx_desc_lim));

	/*
	 * Do not restrict descriptor counts until
	 * the first back-end device gets attached.
	 */
	internals->rx_desc_lim.nb_max = UINT16_MAX;
	internals->tx_desc_lim.nb_max = UINT16_MAX;
	internals->rx_desc_lim.nb_align = 1;
	internals->tx_desc_lim.nb_align = 1;

	memset(internals->active_members, 0, sizeof(internals->active_members));
	memset(internals->members, 0, sizeof(internals->members));

	TAILQ_INIT(&internals->flow_list);
	internals->flow_isolated_valid = 0;

	/* Set mode 4 default configuration */
	bond_mode_8023ad_setup(eth_dev, NULL);
	if (bond_ethdev_mode_set(eth_dev, mode)) {
		RTE_BOND_LOG(ERR, "Failed to set bonding device %u mode to %u",
				 eth_dev->data->port_id, mode);
		goto err;
	}

	vlan_filter_bmp_size =
		rte_bitmap_get_memory_footprint(RTE_ETHER_MAX_VLAN_ID + 1);
	internals->vlan_filter_bmpmem = rte_malloc(name, vlan_filter_bmp_size,
						   RTE_CACHE_LINE_SIZE);
	if (internals->vlan_filter_bmpmem == NULL) {
		RTE_BOND_LOG(ERR,
			     "Failed to allocate vlan bitmap for bonding device %u",
			     eth_dev->data->port_id);
		goto err;
	}

	internals->vlan_filter_bmp = rte_bitmap_init(RTE_ETHER_MAX_VLAN_ID + 1,
			internals->vlan_filter_bmpmem, vlan_filter_bmp_size);
	if (internals->vlan_filter_bmp == NULL) {
		RTE_BOND_LOG(ERR,
			     "Failed to init vlan bitmap for bonding device %u",
			     eth_dev->data->port_id);
		rte_free(internals->vlan_filter_bmpmem);
		goto err;
	}

	return eth_dev->data->port_id;

err:
	rte_free(internals);
	if (eth_dev != NULL)
		eth_dev->data->dev_private = NULL;
	rte_eth_dev_release_port(eth_dev);
	return -1;
}

static int
bond_probe(struct rte_vdev_device *dev)
{
	const char *name;
	struct bond_dev_private *internals;
	struct rte_kvargs *kvlist;
	uint8_t bonding_mode;
	int arg_count, port_id;
	int socket_id;
	uint8_t agg_mode;
	struct rte_eth_dev *eth_dev;

	if (!dev)
		return -EINVAL;

	name = rte_vdev_device_name(dev);
	RTE_BOND_LOG(INFO, "Initializing pmd_bond for %s", name);

	if (rte_eal_process_type() == RTE_PROC_SECONDARY) {
		eth_dev = rte_eth_dev_attach_secondary(name);
		if (!eth_dev) {
			RTE_BOND_LOG(ERR, "Failed to probe %s", name);
			return -1;
		}
		/* TODO: request info from primary to set up Rx and Tx */
		eth_dev->dev_ops = &default_dev_ops;
		eth_dev->device = &dev->device;
		rte_eth_dev_probing_finish(eth_dev);
		return 0;
	}

	kvlist = rte_kvargs_parse(rte_vdev_device_args(dev),
		pmd_bond_init_valid_arguments);
	if (kvlist == NULL) {
		RTE_BOND_LOG(ERR, "Invalid args in %s", rte_vdev_device_args(dev));
		return -1;
	}

	/* Parse link bonding mode */
	if (rte_kvargs_count(kvlist, PMD_BOND_MODE_KVARG) == 1) {
		if (rte_kvargs_process(kvlist, PMD_BOND_MODE_KVARG,
				&bond_ethdev_parse_member_mode_kvarg,
				&bonding_mode) != 0) {
			RTE_BOND_LOG(ERR, "Invalid mode for bonding device %s",
					name);
			goto parse_error;
		}
	} else {
		RTE_BOND_LOG(ERR, "Mode must be specified only once for bonding "
				"device %s", name);
		goto parse_error;
	}

	/* Parse socket id to create bonding device on */
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_SOCKET_ID_KVARG);
	if (arg_count == 1) {
		if (rte_kvargs_process(kvlist, PMD_BOND_SOCKET_ID_KVARG,
				&bond_ethdev_parse_socket_id_kvarg, &socket_id)
				!= 0) {
			RTE_BOND_LOG(ERR, "Invalid socket Id specified for "
					"bonding device %s", name);
			goto parse_error;
		}
	} else if (arg_count > 1) {
		RTE_BOND_LOG(ERR, "Socket Id can be specified only once for "
				"bonding device %s", name);
		goto parse_error;
	} else {
		socket_id = rte_socket_id();
	}

	dev->device.numa_node = socket_id;

	/* Create link bonding eth device */
	port_id = bond_alloc(dev, bonding_mode);
	if (port_id < 0) {
		RTE_BOND_LOG(ERR, "Failed to create socket %s in mode %u on "
				"socket %d.",	name, bonding_mode, socket_id);
		goto parse_error;
	}
	internals = rte_eth_devices[port_id].data->dev_private;
	internals->kvlist = kvlist;

	if (rte_kvargs_count(kvlist, PMD_BOND_AGG_MODE_KVARG) == 1) {
		if (rte_kvargs_process(kvlist,
				PMD_BOND_AGG_MODE_KVARG,
				&bond_ethdev_parse_member_agg_mode_kvarg,
				&agg_mode) != 0) {
			RTE_BOND_LOG(ERR,
					"Failed to parse agg selection mode for bonding device %s",
					name);
			goto parse_error;
		}

		if (internals->mode == BONDING_MODE_8023AD)
			internals->mode4.agg_selection = agg_mode;
	} else {
		internals->mode4.agg_selection = AGG_STABLE;
	}

	rte_eth_dev_probing_finish(&rte_eth_devices[port_id]);
	RTE_BOND_LOG(INFO, "Create bonding device %s on port %d in mode %u on "
			"socket %u.",	name, port_id, bonding_mode, socket_id);
	return 0;

parse_error:
	rte_kvargs_free(kvlist);

	return -1;
}

static int
bond_remove(struct rte_vdev_device *dev)
{
	struct rte_eth_dev *eth_dev;
	struct bond_dev_private *internals;
	const char *name;
	int ret = 0;

	if (!dev)
		return -EINVAL;

	name = rte_vdev_device_name(dev);
	RTE_BOND_LOG(INFO, "Uninitializing pmd_bond for %s", name);

	/* find an ethdev entry */
	eth_dev = rte_eth_dev_allocated(name);
	if (eth_dev == NULL)
		return 0; /* port already released */

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return rte_eth_dev_release_port(eth_dev);

	RTE_ASSERT(eth_dev->device == &dev->device);

	internals = eth_dev->data->dev_private;
	if (internals->member_count != 0)
		return -EBUSY;

	if (eth_dev->data->dev_started == 1) {
		ret = bond_ethdev_stop(eth_dev);
		bond_ethdev_close(eth_dev);
	}
	rte_eth_dev_release_port(eth_dev);

	return ret;
}

/* this part will resolve the member portids after all the other pdev and vdev
 * have been allocated */
static int
bond_ethdev_configure(struct rte_eth_dev *dev)
{
	const char *name = dev->device->name;
	struct bond_dev_private *internals = dev->data->dev_private;
	struct rte_kvargs *kvlist = internals->kvlist;
	int arg_count;
	uint16_t port_id = dev - rte_eth_devices;
	uint32_t link_speeds;
	uint8_t agg_mode;

	static const uint8_t default_rss_key[40] = {
		0x6D, 0x5A, 0x56, 0xDA, 0x25, 0x5B, 0x0E, 0xC2, 0x41, 0x67, 0x25, 0x3D,
		0x43, 0xA3, 0x8F, 0xB0, 0xD0, 0xCA, 0x2B, 0xCB, 0xAE, 0x7B, 0x30, 0xB4,
		0x77, 0xCB, 0x2D, 0xA3, 0x80, 0x30, 0xF2, 0x0C, 0x6A, 0x42, 0xB7, 0x3B,
		0xBE, 0xAC, 0x01, 0xFA
	};

	unsigned i, j;


	bond_ethdev_cfg_cleanup(dev, false);

	/*
	 * If RSS is enabled, fill table with default values and
	 * set key to the value specified in port RSS configuration.
	 * Fall back to default RSS key if the key is not specified
	 */
	if (dev->data->dev_conf.rxmode.mq_mode & RTE_ETH_MQ_RX_RSS) {
		struct rte_eth_rss_conf *rss_conf =
			&dev->data->dev_conf.rx_adv_conf.rss_conf;

		if (internals->rss_key_len == 0) {
			internals->rss_key_len = sizeof(default_rss_key);
		}

		if (rss_conf->rss_key != NULL) {
			if (internals->rss_key_len > rss_conf->rss_key_len) {
				RTE_BOND_LOG(ERR, "Invalid rss key length(%u)",
						rss_conf->rss_key_len);
				return -EINVAL;
			}

			memcpy(internals->rss_key, rss_conf->rss_key,
			       internals->rss_key_len);
		} else {
			if (internals->rss_key_len > sizeof(default_rss_key)) {
				/*
				 * If the rss_key includes standard_rss_key and
				 * extended_hash_key, the rss key length will be
				 * larger than default rss key length, so it should
				 * re-calculate the hash key.
				 */
				for (i = 0; i < internals->rss_key_len; i++)
					internals->rss_key[i] = (uint8_t)rte_rand();
			} else {
				memcpy(internals->rss_key, default_rss_key,
					internals->rss_key_len);
			}
		}

		for (i = 0; i < RTE_DIM(internals->reta_conf); i++) {
			internals->reta_conf[i].mask = ~0LL;
			for (j = 0; j < RTE_ETH_RETA_GROUP_SIZE; j++)
				internals->reta_conf[i].reta[j] =
						(i * RTE_ETH_RETA_GROUP_SIZE + j) %
						dev->data->nb_rx_queues;
		}
	}

	link_speeds = dev->data->dev_conf.link_speeds;
	/*
	 * The default value of 'link_speeds' is zero. From its definition,
	 * this value actually means auto-negotiation. But not all PMDs support
	 * auto-negotiation. So ignore the check for the auto-negotiation and
	 * only consider fixed speed to reduce the impact on PMDs.
	 */
	if (link_speeds & RTE_ETH_LINK_SPEED_FIXED) {
		if ((link_speeds &
		    (internals->speed_capa & ~RTE_ETH_LINK_SPEED_FIXED)) == 0) {
			RTE_BOND_LOG(ERR, "the fixed speed is not supported by all member devices.");
			return -EINVAL;
		}
		/*
		 * Two '1' in binary of 'link_speeds': bit0 and a unique
		 * speed bit.
		 */
		if (rte_popcount64(link_speeds) != 2) {
			RTE_BOND_LOG(ERR, "please set a unique speed.");
			return -EINVAL;
		}
	}

	/* set the max_rx_pktlen */
	internals->max_rx_pktlen = internals->candidate_max_rx_pktlen;

	/*
	 * if no kvlist, it means that this bonding device has been created
	 * through the bonding api.
	 */
	if (!kvlist || internals->kvargs_processing_is_done)
		return 0;

	internals->kvargs_processing_is_done = true;

	/* Parse MAC address for bonding device */
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_MAC_ADDR_KVARG);
	if (arg_count == 1) {
		struct rte_ether_addr bond_mac;

		if (rte_kvargs_process(kvlist, PMD_BOND_MAC_ADDR_KVARG,
				       &bond_ethdev_parse_bond_mac_addr_kvarg, &bond_mac) < 0) {
			RTE_BOND_LOG(INFO, "Invalid mac address for bonding device %s",
				     name);
			return -1;
		}

		/* Set MAC address */
		if (rte_eth_bond_mac_address_set(port_id, &bond_mac) != 0) {
			RTE_BOND_LOG(ERR,
				     "Failed to set mac address on bonding device %s",
				     name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_BOND_LOG(ERR,
			     "MAC address can be specified only once for bonding device %s",
			     name);
		return -1;
	}

	/* Parse/set balance mode transmit policy */
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_XMIT_POLICY_KVARG);
	if (arg_count == 1) {
		uint8_t xmit_policy;

		if (rte_kvargs_process(kvlist, PMD_BOND_XMIT_POLICY_KVARG,
				       &bond_ethdev_parse_balance_xmit_policy_kvarg, &xmit_policy) !=
		    0) {
			RTE_BOND_LOG(INFO,
				     "Invalid xmit policy specified for bonding device %s",
				     name);
			return -1;
		}

		/* Set balance mode transmit policy*/
		if (rte_eth_bond_xmit_policy_set(port_id, xmit_policy) != 0) {
			RTE_BOND_LOG(ERR,
				     "Failed to set balance xmit policy on bonding device %s",
				     name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_BOND_LOG(ERR,
			     "Transmit policy can be specified only once for bonding device %s",
			     name);
		return -1;
	}

	if (rte_kvargs_count(kvlist, PMD_BOND_AGG_MODE_KVARG) == 1) {
		if (rte_kvargs_process(kvlist,
				       PMD_BOND_AGG_MODE_KVARG,
				       &bond_ethdev_parse_member_agg_mode_kvarg,
				       &agg_mode) != 0) {
			RTE_BOND_LOG(ERR,
				     "Failed to parse agg selection mode for bonding device %s",
				     name);
		}
		if (internals->mode == BONDING_MODE_8023AD) {
			int ret = rte_eth_bond_8023ad_agg_selection_set(port_id,
					agg_mode);
			if (ret < 0) {
				RTE_BOND_LOG(ERR,
					"Invalid args for agg selection set for bonding device %s",
					name);
				return -1;
			}
		}
	}

	/* Parse/add member ports to bonding device */
	if (rte_kvargs_count(kvlist, PMD_BOND_MEMBER_PORT_KVARG) > 0) {
		struct bond_ethdev_member_ports member_ports;
		unsigned i;

		memset(&member_ports, 0, sizeof(member_ports));

		if (rte_kvargs_process(kvlist, PMD_BOND_MEMBER_PORT_KVARG,
				       &bond_ethdev_parse_member_port_kvarg, &member_ports) != 0) {
			RTE_BOND_LOG(ERR,
				     "Failed to parse member ports for bonding device %s",
				     name);
			return -1;
		}

		for (i = 0; i < member_ports.member_count; i++) {
			if (rte_eth_bond_member_add(port_id, member_ports.members[i]) != 0) {
				RTE_BOND_LOG(ERR,
					     "Failed to add port %d as member to bonding device %s",
					     member_ports.members[i], name);
			}
		}

	} else {
		RTE_BOND_LOG(INFO, "No members specified for bonding device %s", name);
		return -1;
	}

	/* Parse/set primary member port id*/
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_PRIMARY_MEMBER_KVARG);
	if (arg_count == 1) {
		uint16_t primary_member_port_id;

		if (rte_kvargs_process(kvlist,
				       PMD_BOND_PRIMARY_MEMBER_KVARG,
				       &bond_ethdev_parse_primary_member_port_id_kvarg,
				       &primary_member_port_id) < 0) {
			RTE_BOND_LOG(INFO,
				     "Invalid primary member port id specified for bonding device %s",
				     name);
			return -1;
		}

		/* Set balance mode transmit policy*/
		if (rte_eth_bond_primary_set(port_id, primary_member_port_id)
		    != 0) {
			RTE_BOND_LOG(ERR,
				     "Failed to set primary member port %d on bonding device %s",
				     primary_member_port_id, name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_BOND_LOG(INFO,
			     "Primary member can be specified only once for bonding device %s",
			     name);
		return -1;
	}

	/* Parse link status monitor polling interval */
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_LSC_POLL_PERIOD_KVARG);
	if (arg_count == 1) {
		uint32_t lsc_poll_interval_ms;

		if (rte_kvargs_process(kvlist,
				       PMD_BOND_LSC_POLL_PERIOD_KVARG,
				       &bond_ethdev_parse_time_ms_kvarg,
				       &lsc_poll_interval_ms) < 0) {
			RTE_BOND_LOG(INFO,
				     "Invalid lsc polling interval value specified for bonding"
				     " device %s", name);
			return -1;
		}

		if (rte_eth_bond_link_monitoring_set(port_id, lsc_poll_interval_ms)
		    != 0) {
			RTE_BOND_LOG(ERR,
				     "Failed to set lsc monitor polling interval (%u ms) on bonding device %s",
				     lsc_poll_interval_ms, name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_BOND_LOG(INFO,
			     "LSC polling interval can be specified only once for bonding"
			     " device %s", name);
		return -1;
	}

	/* Parse link up interrupt propagation delay */
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_LINK_UP_PROP_DELAY_KVARG);
	if (arg_count == 1) {
		uint32_t link_up_delay_ms;

		if (rte_kvargs_process(kvlist,
				       PMD_BOND_LINK_UP_PROP_DELAY_KVARG,
				       &bond_ethdev_parse_time_ms_kvarg,
				       &link_up_delay_ms) < 0) {
			RTE_BOND_LOG(INFO,
				     "Invalid link up propagation delay value specified for"
				     " bonding device %s", name);
			return -1;
		}

		/* Set balance mode transmit policy*/
		if (rte_eth_bond_link_up_prop_delay_set(port_id, link_up_delay_ms)
		    != 0) {
			RTE_BOND_LOG(ERR,
				     "Failed to set link up propagation delay (%u ms) on bonding"
				     " device %s", link_up_delay_ms, name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_BOND_LOG(INFO,
			     "Link up propagation delay can be specified only once for"
			     " bonding device %s", name);
		return -1;
	}

	/* Parse link down interrupt propagation delay */
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_LINK_DOWN_PROP_DELAY_KVARG);
	if (arg_count == 1) {
		uint32_t link_down_delay_ms;

		if (rte_kvargs_process(kvlist,
				       PMD_BOND_LINK_DOWN_PROP_DELAY_KVARG,
				       &bond_ethdev_parse_time_ms_kvarg,
				       &link_down_delay_ms) < 0) {
			RTE_BOND_LOG(INFO,
				     "Invalid link down propagation delay value specified for"
				     " bonding device %s", name);
			return -1;
		}

		/* Set balance mode transmit policy*/
		if (rte_eth_bond_link_down_prop_delay_set(port_id, link_down_delay_ms)
		    != 0) {
			RTE_BOND_LOG(ERR,
				     "Failed to set link down propagation delay (%u ms) on bonding device %s",
				     link_down_delay_ms, name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_BOND_LOG(INFO,
			     "Link down propagation delay can be specified only once for  bonding device %s",
			     name);
		return -1;
	}

	/* configure members so we can pass mtu setting */
	for (i = 0; i < internals->member_count; i++) {
		struct rte_eth_dev *member_ethdev =
				&(rte_eth_devices[internals->members[i].port_id]);
		if (member_configure(dev, member_ethdev) != 0) {
			RTE_BOND_LOG(ERR,
				"bonding port (%d) failed to configure member device (%d)",
				dev->data->port_id,
				internals->members[i].port_id);
			return -1;
		}
	}
	return 0;
}

struct rte_vdev_driver pmd_bond_drv = {
	.probe = bond_probe,
	.remove = bond_remove,
};

RTE_PMD_REGISTER_VDEV(net_bonding, pmd_bond_drv);
RTE_PMD_REGISTER_ALIAS(net_bonding, eth_bond);

RTE_PMD_REGISTER_PARAM_STRING(net_bonding,
	"member=<ifc> "
	"primary=<ifc> "
	"mode=[0-6] "
	"xmit_policy=[l2 | l23 | l34] "
	"agg_mode=[count | stable | bandwidth] "
	"socket_id=<int> "
	"mac=<mac addr> "
	"lsc_poll_period_ms=<int> "
	"up_delay=<int> "
	"down_delay=<int>");

/* We can't use RTE_LOG_REGISTER_DEFAULT because of the forced name for
 * this library, see meson.build.
 */
RTE_LOG_REGISTER(bond_logtype, pmd.net.bonding, NOTICE);
