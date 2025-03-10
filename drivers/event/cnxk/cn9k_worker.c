/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2021 Marvell.
 */

#include "roc_api.h"

#include "cn9k_worker.h"
#include "cn9k_cryptodev_ops.h"

uint16_t __rte_hot
cn9k_sso_hws_enq_burst(void *port, const struct rte_event ev[],
		       uint16_t nb_events)
{
	struct cn9k_sso_hws *ws = port;

	RTE_SET_USED(nb_events);

	switch (ev->op) {
	case RTE_EVENT_OP_NEW:
		return cn9k_sso_hws_new_event(ws, ev);
	case RTE_EVENT_OP_FORWARD:
		cn9k_sso_hws_forward_event(ws, ev);
		break;
	case RTE_EVENT_OP_RELEASE:
		if (ws->swtag_req) {
			cnxk_sso_hws_desched(ev->u64, ws->base);
			ws->swtag_req = 0;
			break;
		}
		cnxk_sso_hws_swtag_flush(ws->base);
		break;
	default:
		return 0;
	}

	return 1;
}

uint16_t __rte_hot
cn9k_sso_hws_enq_new_burst(void *port, const struct rte_event ev[],
			   uint16_t nb_events)
{
	struct cn9k_sso_hws *ws = port;
	uint16_t i, rc = 1;

	for (i = 0; i < nb_events && rc; i++)
		rc = cn9k_sso_hws_new_event(ws, &ev[i]);

	return nb_events;
}

uint16_t __rte_hot
cn9k_sso_hws_enq_fwd_burst(void *port, const struct rte_event ev[],
			   uint16_t nb_events)
{
	struct cn9k_sso_hws *ws = port;

	RTE_SET_USED(nb_events);
	cn9k_sso_hws_forward_event(ws, ev);

	return 1;
}

int __rte_hot
cn9k_sso_hws_profile_switch(void *port, uint8_t profile)
{
	struct cn9k_sso_hws *ws = port;

	ws->gw_wdata &= ~(0xFFUL);
	ws->gw_wdata |= (profile + 1);

	return 0;
}

/* Dual ws ops. */

uint16_t __rte_hot
cn9k_sso_hws_dual_enq_burst(void *port, const struct rte_event ev[],
			    uint16_t nb_events)
{
	struct cn9k_sso_hws_dual *dws = port;
	uint64_t base;

	RTE_SET_USED(nb_events);

	base = dws->base[!dws->vws];
	switch (ev->op) {
	case RTE_EVENT_OP_NEW:
		return cn9k_sso_hws_dual_new_event(dws, ev);
	case RTE_EVENT_OP_FORWARD:
		cn9k_sso_hws_dual_forward_event(dws, base, ev);
		break;
	case RTE_EVENT_OP_RELEASE:
		if (dws->swtag_req) {
			cnxk_sso_hws_desched(ev->u64, base);
			dws->swtag_req = 0;
			break;
		}
		cnxk_sso_hws_swtag_flush(base);
		break;
	default:
		return 0;
	}

	return 1;
}

uint16_t __rte_hot
cn9k_sso_hws_dual_enq_new_burst(void *port, const struct rte_event ev[],
				uint16_t nb_events)
{
	struct cn9k_sso_hws_dual *dws = port;
	uint16_t i, rc = 1;

	for (i = 0; i < nb_events && rc; i++)
		rc = cn9k_sso_hws_dual_new_event(dws, &ev[i]);

	return nb_events;
}

uint16_t __rte_hot
cn9k_sso_hws_dual_enq_fwd_burst(void *port, const struct rte_event ev[],
				uint16_t nb_events)
{
	struct cn9k_sso_hws_dual *dws = port;

	RTE_SET_USED(nb_events);
	cn9k_sso_hws_dual_forward_event(dws, dws->base[!dws->vws], ev);

	return 1;
}

uint16_t __rte_hot
cn9k_sso_hws_ca_enq(void *port, struct rte_event ev[], uint16_t nb_events)
{
	struct cn9k_sso_hws *ws = port;

	RTE_SET_USED(nb_events);

	return cn9k_cpt_crypto_adapter_enqueue(ws->base, ev->event_ptr);
}

uint16_t __rte_hot
cn9k_sso_hws_dual_ca_enq(void *port, struct rte_event ev[], uint16_t nb_events)
{
	struct cn9k_sso_hws_dual *dws = port;

	RTE_SET_USED(nb_events);

	return cn9k_cpt_crypto_adapter_enqueue(dws->base[!dws->vws],
					       ev->event_ptr);
}

int __rte_hot
cn9k_sso_hws_dual_profile_switch(void *port, uint8_t profile)
{
	struct cn9k_sso_hws_dual *dws = port;

	dws->gw_wdata &= ~(0xFFUL);
	dws->gw_wdata |= (profile + 1);

	return 0;
}
