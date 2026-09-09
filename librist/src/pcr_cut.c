/* pcr_cut.c -- PCR-boundary packetisation for VSF TR-06-4 Part 8.
 *
 * WHAT THIS IS FOR
 * ----------------
 * Part 8 repairs a satellite feed on the box from an IP copy held by a server.
 * For an ordinary RIST NACK to fetch the right bytes, sequence number N on the
 * box's stream and sequence number N on the server's must mean the same seven
 * TS packets. librist itself preserves numbering under Part 6 filtering, so the
 * only thing that can misalign the two sides is where each one CUTS its input
 * into payloads.
 *
 * Stock librist never groups: one datagram in, one RTP payload out. The grouping
 * therefore happens in whatever feeds the sender -- tsp on the server, the demux
 * reader on the box -- and those are two different programs whose payload
 * boundaries depend on nothing better than which TS packet each happened to see
 * first. Two feeders, two phases, permanent misalignment.
 *
 * This moves the cutting INTO librist so both sides run the same code, and makes
 * the boundary a function of the CONTENT rather than of when a process started:
 *
 *   - a new payload starts at every packet carrying a PCR on the service's PCR
 *     PID (the interval-start boundary);
 *   - within an interval, fill greedily up to 7 TS packets per payload;
 *   - a payload is one datagram of at most 7 packets. An interval is several
 *     payloads, and the sequence advances PER PAYLOAD, not per PCR.
 *
 * Because both sides derive boundaries from the same bytes, phase cannot drift,
 * and it re-establishes at every PCR -- tens of times a second -- so a
 * disagreement is confined to one interval instead of being permanent.
 *
 * INPUT FRAMING IS DELIBERATELY IGNORED
 * -------------------------------------
 * The caller's datagram boundaries mean nothing here. The feed is treated as a
 * continuous TS byte stream: partial packets are carried across calls and
 * alignment is recovered by hunting for the 0x47 sync byte. That is what lets
 * the same binary sit behind tsp's 7-packet datagrams on one side and the box
 * reader's 1316-byte chunks on the other and still cut identically.
 *
 * THE ACCUMULATOR IS SMALL
 * ------------------------
 * It flushes at 7 packets or at a PCR, whichever comes first, so it never holds
 * a whole PCR interval -- intervals reach 79 packets on this transponder and the
 * buffer is still 7 packets plus one partial, about 1.5 KB. Nothing here grows
 * with interval length.
 *
 * OFF BY DEFAULT. When cfg->pcr_cut is 0 the caller does not route through here
 * at all and the original one-in-one-out path runs untouched.
 *
 * PID FILTERING, AND WHY IT LIVES HERE
 * ------------------------------------
 * The box captures the WHOLE transponder -- 59 Mb/s, 201 PIDs -- because that is
 * the only way its demux will hand over the broadcast PSI the repair needs; a
 * demux slot takes one scalar PID and there are not enough of them. But 59 Mb/s
 * is more than the box can carry through a RIST hop: librist allocates two
 * mallocs per packet and holds them for the buffer window, which at 2000 ms is
 * ~17 MB in the sender's retransmit queue and ~17 MB again in the receiver, on a
 * box with 54 MB managed and ~25 MB free. It does not fit, and the failure is
 * not a clean error -- it is packets silently dropped inside librist, a NACK
 * storm chasing them, and eventually the OOM killer.
 *
 * So the stream has to be cut down to the service before the RIST hop, and this
 * is the right place for the gate for two reasons:
 *
 *   - The far end filters too, with tsp -P filter --pid, and byte-identity is
 *     the whole basis of the repair. tsp runs BEFORE the headend's cutter, so
 *     the box must filter before its cutter as well: cut first and the PCR
 *     boundaries would be computed over a stream the other side never saw, and
 *     the greedy-7 packing inside every interval would differ.
 *
 *   - feed_packet() already has a whole, sync-checked, 188-byte packet in hand.
 *     A filter anywhere upstream of the cutter would need its own copy of the
 *     re-sync and partial-carry logic below, and two alignment implementations
 *     that must agree forever is exactly what this file exists to avoid.
 *
 * The gate itself is deliberately dumb -- a bitmap test, no table parsing, no
 * implicit PIDs. See rist_pcr_cut_set_filter().
 */

#include "pcr_cut.h"
#include <stdlib.h>
#include <string.h>

#define TS_SYNC        0x47
#define TS_PKT         188
#define TS_PER_PAYLOAD 7

void rist_pcr_cut_init(struct rist_pcr_cut *c, uint16_t pcr_pid)
{
	memset(c, 0, sizeof(*c));
	c->pcr_pid = pcr_pid;
	c->anchor = (pcr_pid != 0);
	c->seen_first_pcr = false;
}

/* Parse the keep-list into the bitmap. See the header for why nothing is added
 * implicitly and why a malformed list is refused rather than partially applied. */
int rist_pcr_cut_set_filter(struct rist_pcr_cut *c, const char *list)
{
	const char *p = list;
	uint16_t n = 0;

	if (!c || !list)
		return -1;

	memset(c->keep, 0, sizeof(c->keep));
	c->filter_on = false;
	c->filter_npids = 0;

	while (*p) {
		unsigned long v;
		char *end;

		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;

		/* Base 0 so both 17 and 0x11 are accepted -- the box builds this list
		 * from a JSON array of decimals, a human editing a knob file will reach
		 * for hex, and neither should be a silent no-match. */
		v = strtoul(p, &end, 0);
		if (end == p)
			return -1;                     /* not a number */
		if (v >= RIST_PCR_CUT_NUM_PIDS)
			return -1;                     /* not a PID */
		if (*end && *end != ',' && *end != ' ' && *end != '\t')
			return -1;                     /* trailing junk, e.g. "17x" */

		if (!(c->keep[v >> 3] & (uint8_t)(1u << (v & 7)))) {
			c->keep[v >> 3] |= (uint8_t)(1u << (v & 7));
			n++;
		}
		p = end;
	}

	if (n == 0)
		return -1;

	c->filter_npids = n;
	c->filter_on = true;
	return 0;
}

/* True if this packet carries a PCR on our PCR PID.
 *
 * A PCR lives in the adaptation field, so: the PID must match, an adaptation
 * field must be present (AFC 2 or 3), it must be non-empty, and the PCR flag
 * must be set. Checked in that order so a short or malformed packet cannot lead
 * us to read past the header. */
static bool packet_has_pcr(const uint8_t *p, uint16_t pcr_pid)
{
	uint16_t pid = (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
	uint8_t afc;

	if (pid != pcr_pid)
		return false;

	afc = (uint8_t)((p[3] >> 4) & 0x03);
	if (afc != 2 && afc != 3)
		return false;
	if (p[4] == 0)                 /* adaptation_field_length */
		return false;

	return (p[5] & 0x10) != 0;     /* PCR_flag */
}

/* Hand the pending packets to librist as one payload, then reset.
 *
 * Emitting nothing when nothing is pending is not an edge case bolted on -- it
 * is what makes two adjacent PCRs correct. The sequence must not advance for an
 * interval that contained no packets, or the two sides diverge by one from then
 * on. */
static int flush(struct rist_pcr_cut *c, struct rist_ctx *ctx)
{
	struct rist_data_block blk;
	int ret;

	if (c->n_pkts == 0)
		return 0;

	memset(&blk, 0, sizeof(blk));
	blk.payload     = c->pending;
	blk.payload_len = (size_t)c->n_pkts * TS_PKT;
	blk.ts_ntp      = 0;           /* let the library stamp it, as the UDP path does */

	ret = rist_sender_data_write(ctx, &blk);
	c->n_pkts = 0;
	return ret;
}

/* Consume one whole, sync-checked TS packet. */
static int feed_packet(struct rist_pcr_cut *c, const uint8_t *pkt, struct rist_ctx *ctx)
{
	int ret = 0;

	/* THE FILTER GATE, before anything else in this function looks at the
	 * packet. A dropped packet must not flush, must not count, must not be
	 * appended -- it must be as if it never arrived, because on the far end tsp
	 * removed it before the cutter ever saw it. */
	if (c->filter_on) {
		uint16_t pid = (uint16_t)(((pkt[1] & 0x1F) << 8) | pkt[2]);

		if (!(c->keep[pid >> 3] & (uint8_t)(1u << (pid & 7)))) {
			c->filtered_out++;
			return 0;
		}
	}

	/* c->anchor is false only in filter-only mode; Part 8 repair always has it
	 * on, and without it there is no boundary the far end can agree with. */
	if (c->anchor) {
		if (packet_has_pcr(pkt, c->pcr_pid)) {
			/* Flush BEFORE appending, so the PCR packet always lands at offset
			 * 0 of a payload and the greedy-7 counter restarts there. This
			 * ordering is the whole mechanism: get it backwards and the two
			 * sides split differently inside every interval while still
			 * agreeing on where intervals begin. */
			ret = flush(c, ctx);
			c->seen_first_pcr = true;
			c->pcr_count++;
		} else if (!c->seen_first_pcr) {
			/* Bytes before the first PCR belong to an interval we did not see
			 * the start of. Dropping them is what lets each side begin cleanly
			 * at its own first PCR -- the two sides generally start at
			 * different PCRs and absolute alignment is the anchor's job, not
			 * ours. */
			c->dropped_pre_pcr++;
			return 0;
		}
	}

	memcpy(c->pending + (size_t)c->n_pkts * TS_PKT, pkt, TS_PKT);
	c->n_pkts++;
	c->pkts_in++;

	if (c->n_pkts == TS_PER_PAYLOAD) {
		int r = flush(c, ctx);
		if (r < 0)
			ret = r;
	}

	return ret;
}

int rist_pcr_cut_feed(struct rist_pcr_cut *c, const uint8_t *data, size_t len,
                      struct rist_ctx *ctx)
{
	size_t off = 0;
	int ret = 0;

	while (off < len) {
		/* Complete a packet that straddled the previous call. */
		if (c->n_partial > 0) {
			size_t need = TS_PKT - c->n_partial;
			size_t take = (len - off < need) ? (len - off) : need;

			memcpy(c->partial + c->n_partial, data + off, take);
			c->n_partial += take;
			off += take;

			if (c->n_partial < TS_PKT)
				break;                 /* still short; wait for more */

			c->n_partial = 0;
			if (c->partial[0] == TS_SYNC) {
				int r = feed_packet(c, c->partial, ctx);
				if (r < 0)
					ret = r;
			} else {
				/* The carried bytes were not a packet after all. Drop them and
				 * let the resync below find the next real boundary rather than
				 * emitting misframed data. */
				c->resyncs++;
			}
			continue;
		}

		/* Resync: never trust the caller's framing, only the sync byte. */
		if (data[off] != TS_SYNC) {
			size_t start = off;
			while (off < len && data[off] != TS_SYNC)
				off++;
			c->bad_sync += (uint64_t)(off - start);
			if (off >= len)
				break;
		}

		if (len - off >= TS_PKT) {
			int r = feed_packet(c, data + off, ctx);
			if (r < 0)
				ret = r;
			off += TS_PKT;
		} else {
			/* Tail of the buffer: carry it and finish next call. */
			c->n_partial = len - off;
			memcpy(c->partial, data + off, c->n_partial);
			off = len;
		}
	}

	return ret;
}
