/* Simple TRAU frame reflector to route voice calls */

/* (C) 2009 by Harald Welte <laforge@gnumonks.org>
 * All Rights Reserved
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
 *
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <openbsc/gsm_data.h>
#include <openbsc/trau_frame.h>
#include <openbsc/trau_mux.h>
#include <openbsc/subchan_demux.h>
#include <openbsc/e1_input.h>

struct map_entry {
	struct llist_head list;
	struct gsm_e1_subslot src, dst;
};

static LLIST_HEAD(ss_map);

/* map one particular subslot to another subslot */
int trau_mux_map(const struct gsm_e1_subslot *src,
		 const struct gsm_e1_subslot *dst)
{
	struct map_entry *me = malloc(sizeof(*me));
	if (!me)
		return -ENOMEM;

	memcpy(&me->src, src, sizeof(me->src));
	memcpy(&me->dst, dst, sizeof(me->dst));
	llist_add(&me->list, &ss_map);

	return 0;
}

/* unmap one particular subslot from another subslot */
int trau_mux_unmap(const struct gsm_e1_subslot *ss)
{
	struct map_entry *me, *me2;

	llist_for_each_entry_safe(me, me2, &ss_map, list) {
		if (!memcmp(&me->src, ss, sizeof(*ss)) ||
		    !memcmp(&me->dst, ss, sizeof(*ss))) {
			llist_del(&me->list);
			return 0;
		}
	}
	return -ENOENT;
}

/* look-up an enty in the TRAU mux map */
static struct gsm_e1_subslot *
lookup_trau_mux_map(const struct gsm_e1_subslot *src)
{
	struct map_entry *me;

	llist_for_each_entry(me, &ss_map, list) {
		if (!memcmp(&me->src, src, sizeof(*src)))
			return &me->dst;
		if (!memcmp(&me->dst, src, sizeof(*src)))
			return &me->src;
	}
	return NULL;
}

/* we get called by subchan_demux */
int trau_mux_input(struct gsm_e1_subslot *src_e1_ss,
		   const u_int8_t *trau_bits, int num_bits)
{
	struct decoded_trau_frame tf;
	u_int8_t trau_bits_out[TRAU_FRAME_BITS];
	struct gsm_e1_subslot *dst_e1_ss = lookup_trau_mux_map(src_e1_ss);
	struct subch_mux *mx;

	if (!dst_e1_ss)
		return -EINVAL;

	mx = e1inp_get_mux(dst_e1_ss->e1_nr, dst_e1_ss->e1_ts);
	if (!mx)
		return -EINVAL;

	/* decode TRAU, change it to downlink, re-encode */
	decode_trau_frame(&tf, trau_bits);
	trau_frame_up2down(&tf);
	encode_trau_frame(trau_bits_out, &tf);

	/* and send it to the muxer */
	return subchan_mux_enqueue(mx, dst_e1_ss->e1_ts_ss, trau_bits_out,
				   TRAU_FRAME_BITS);
}
