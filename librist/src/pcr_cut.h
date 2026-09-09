/* pcr_cut.h -- PCR-boundary packetisation and PID filtering for VSF TR-06-4
 * Part 8.
 *
 * See pcr_cut.c for what this is and why the boundary is derived from the
 * content rather than from the caller's framing.
 *
 * Deliberately a SEPARATE entry point rather than a mode inside
 * rist_sender_data_write(): that call is documented and relied upon as one
 * datagram in, one payload out, and this library runs live Part 7 chains. A
 * caller that does not opt in never reaches this code at all, so the default
 * path is not merely equivalent to what shipped before -- it is the same
 * instructions.
 */

#ifndef RIST_PCR_CUT_H
#define RIST_PCR_CUT_H

#include <librist/librist.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define RIST_PCR_CUT_MAX_PKTS 7
#define RIST_PCR_CUT_PKT_SIZE 188
#define RIST_PCR_CUT_NUM_PIDS 8192

struct rist_pcr_cut {
	uint16_t pcr_pid;
	bool     seen_first_pcr;

	/* False when init was given pcr_pid 0: no PCR anchoring and no pre-PCR
	 * drop, just 188-alignment, the PID filter and greedy-7. That mode exists
	 * so the filter can be used with the cut switched off; it is NOT a mode
	 * for Part 8 repair, which needs the anchor to stay aligned. */
	bool     anchor;

	/* At most 7 packets pending plus one partial: this never scales with the
	 * length of a PCR interval. ~1.5 KB total. */
	uint8_t  pending[RIST_PCR_CUT_MAX_PKTS * RIST_PCR_CUT_PKT_SIZE];
	int      n_pkts;
	uint8_t  partial[RIST_PCR_CUT_PKT_SIZE];
	size_t   n_partial;

	/* PID FILTER -- see rist_pcr_cut_set_filter().
	 *
	 * One bit per PID, 1 KB. Off unless a list was set, and when off not one
	 * instruction of it runs. */
	bool     filter_on;
	uint16_t filter_npids;
	uint8_t  keep[RIST_PCR_CUT_NUM_PIDS / 8];

	/* Observability: all diagnostic only, none of them steer behaviour. */
	uint64_t pkts_in;
	uint64_t pcr_count;
	uint64_t dropped_pre_pcr;
	uint64_t bad_sync;
	uint64_t resyncs;
	uint64_t filtered_out;
};

/* pcr_pid 0 means "no PCR anchor" -- see the anchor field above. Zeroes the
 * whole struct, so set the PID filter AFTER this, never before. */
void rist_pcr_cut_init(struct rist_pcr_cut *c, uint16_t pcr_pid);

/* Keep ONLY the PIDs in this list; drop every other packet before it can reach
 * the cutter. list is comma- or space-separated, decimal or 0x-hex, e.g.
 * "0,1,16,17,18,19,20,0x083F,2112".
 *
 * NO PID IS ADDED IMPLICITLY. The list is the whole policy and the caller owns
 * it -- which is the point: the far end must filter to the identical set for
 * the two byte streams to match, and a filter that quietly adds "mandatory"
 * PIDs of its own is a filter the far end cannot reproduce without sharing the
 * same idea of mandatory. Part 6's contentSelection deliberately DOES add
 * PSI/PMT/EMM implicitly; that is right for a receiver asking for a program and
 * wrong here, so this is a separate, dumber gate.
 *
 * Returns 0 on success, -1 if the list is empty or has a token that is not a
 * PID in 0..8191. Rejects rather than filtering to a partial set: a short list
 * looks like a working channel and drops audio.
 */
int rist_pcr_cut_set_filter(struct rist_pcr_cut *c, const char *list);

/* Feed an arbitrary run of bytes. Input framing is ignored; the stream is
 * re-cut on 188-byte TS alignment, filtered if a list was set, and payload
 * boundaries fall at every PCR of pcr_pid, then greedily every 7 packets.
 * Emits zero or more payloads via rist_sender_data_write(). Returns <0 if any
 * write failed. */
int rist_pcr_cut_feed(struct rist_pcr_cut *c, const uint8_t *data,
                                size_t len, struct rist_ctx *ctx);

#endif /* RIST_PCR_CUT_H */
