/* pcr_cut.h -- PCR-boundary packetisation for VSF TR-06-4 Part 8.
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

struct rist_pcr_cut {
	uint16_t pcr_pid;
	bool     seen_first_pcr;

	/* At most 7 packets pending plus one partial: this never scales with the
	 * length of a PCR interval. ~1.5 KB total. */
	uint8_t  pending[RIST_PCR_CUT_MAX_PKTS * RIST_PCR_CUT_PKT_SIZE];
	int      n_pkts;
	uint8_t  partial[RIST_PCR_CUT_PKT_SIZE];
	size_t   n_partial;

	/* Observability: all four are diagnostic only and never steer behaviour. */
	uint64_t pkts_in;
	uint64_t pcr_count;
	uint64_t dropped_pre_pcr;
	uint64_t bad_sync;
	uint64_t resyncs;
};

void rist_pcr_cut_init(struct rist_pcr_cut *c, uint16_t pcr_pid);

/* Feed an arbitrary run of bytes. Input framing is ignored; the stream is
 * re-cut on 188-byte TS alignment and payload boundaries fall at every PCR of
 * pcr_pid, then greedily every 7 packets. Emits zero or more payloads via
 * rist_sender_data_write(). Returns <0 if any write failed. */
int rist_pcr_cut_feed(struct rist_pcr_cut *c, const uint8_t *data,
                                size_t len, struct rist_ctx *ctx);

#endif /* RIST_PCR_CUT_H */
