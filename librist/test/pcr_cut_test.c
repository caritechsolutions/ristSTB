/* pcr_cut_test.c -- self-contained unit test for PCR-boundary packetisation.
 *
 * No hardware, no network, no librist link: pcr_cut.c is included directly and
 * rist_sender_data_write() is stubbed to record what would have been sent.
 *
 * Build and run:
 *   gcc -I../src -I../include -o pcr_cut_test pcr_cut_test.c && ./pcr_cut_test
 *
 * The test that matters most is FRAMING INDEPENDENCE (test 6): the same TS fed
 * in wildly different chunk sizes must produce byte-identical payloads. That is
 * the property the whole design rests on -- tsp packs 7 packets per datagram on
 * the server and the box reader packs 1316-byte chunks, and both must cut the
 * same way regardless.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Use the REAL librist headers -- the types must match what pcr_cut.c compiles
 * against, or the test proves nothing about the real thing. Only the one
 * function is replaced, with a definition matching its declared signature, so
 * the linker takes ours instead of the library's. */
#include <librist/librist.h>

#define MAX_REC 4096
static struct { uint8_t buf[7 * 188]; size_t len; } g_rec[MAX_REC];
static int g_nrec = 0;

int rist_sender_data_write(struct rist_ctx *ctx, const struct rist_data_block *b)
{
	(void)ctx;
	if (g_nrec >= MAX_REC) { fprintf(stderr, "record overflow\n"); exit(1); }
	if (b->payload_len == 0 || b->payload_len > 7 * 188) {
		fprintf(stderr, "FAIL: payload_len %zu out of range\n", b->payload_len);
		exit(1);
	}
	if (b->payload_len % 188 != 0) {
		fprintf(stderr, "FAIL: payload_len %zu not a multiple of 188\n", b->payload_len);
		exit(1);
	}
	memcpy(g_rec[g_nrec].buf, b->payload, b->payload_len);
	g_rec[g_nrec].len = b->payload_len;
	g_nrec++;
	return (int)b->payload_len;
}

#include "pcr_cut.c"

/* ---- helpers ----------------------------------------------------------- */
static void mk_pkt(uint8_t *p, uint16_t pid, bool pcr, uint8_t tag)
{
	memset(p, 0xFF, 188);
	p[0] = 0x47;
	p[1] = (uint8_t)((pid >> 8) & 0x1F);
	p[2] = (uint8_t)(pid & 0xFF);
	if (pcr) {
		p[3] = 0x30;      /* AFC = 3 (adaptation + payload) */
		p[4] = 7;         /* adaptation_field_length */
		p[5] = 0x10;      /* PCR_flag */
	} else {
		p[3] = 0x10;      /* AFC = 1, payload only */
	}
	p[187] = tag;         /* identity, so we can prove ordering */
}

static int fails = 0;
static void check(int cond, const char *what)
{
	printf("  [%s] %s\n", cond ? " ok " : "FAIL", what);
	if (!cond) fails++;
}

static void reset(void) { g_nrec = 0; }

/* ---- tests -------------------------------------------------------------- */

/* 1. boundaries land at each PCR packet; greedy 7 within the interval */
static void test_boundaries(void)
{
	uint8_t ts[188 * 20];
	struct rist_pcr_cut c;
	int i;

	printf("1. PCR boundaries + greedy 7\n");
	/* PCR at 0, then 9 non-PCR; PCR at 10, then 9 non-PCR. */
	for (i = 0; i < 20; i++)
		mk_pkt(ts + i * 188, (i % 10 == 0) ? 0x0101 : 0x0202, (i % 10 == 0), (uint8_t)i);

	reset();
	rist_pcr_cut_init(&c, 0x0101);
	rist_pcr_cut_feed(&c, ts, sizeof(ts), (struct rist_ctx *)1);

	/* interval 0: packets 0..9 -> 7 + (3 pending, flushed by the PCR at 10)
	 * interval 1: packets 10..19 -> 7 emitted, 3 still pending at end of feed */
	check(g_nrec == 3, "three payloads emitted (7, 3, 7)");
	check(g_rec[0].len == 7 * 188, "payload 0 is 7 packets");
	check(g_rec[1].len == 3 * 188, "payload 1 is the 3-packet remainder");
	check(g_rec[2].len == 7 * 188, "payload 2 is 7 packets");
	check(g_rec[0].buf[187] == 0, "payload 0 starts at the PCR packet (tag 0)");
	check(g_rec[1].buf[187] == 7, "payload 1 continues the interval (tag 7)");
	check(g_rec[2].buf[187] == 10, "payload 2 starts at the next PCR (tag 10)");
}

/* 2. adjacent PCRs emit nothing and do not advance the sequence */
static void test_adjacent_pcr(void)
{
	uint8_t ts[188 * 3];
	struct rist_pcr_cut c;

	printf("2. adjacent PCRs\n");
	mk_pkt(ts + 0 * 188, 0x0101, true, 0);
	mk_pkt(ts + 1 * 188, 0x0101, true, 1);   /* immediately another PCR */
	mk_pkt(ts + 2 * 188, 0x0101, true, 2);

	reset();
	rist_pcr_cut_init(&c, 0x0101);
	rist_pcr_cut_feed(&c, ts, sizeof(ts), (struct rist_ctx *)1);

	/* Each PCR flushes the one pending packet before appending itself, so we
	 * get two 1-packet payloads and one still pending -- never an empty one. */
	check(g_nrec == 2, "two payloads, no empty payload emitted");
	check(g_rec[0].len == 188 && g_rec[1].len == 188, "each is one packet");
	check(g_rec[0].buf[187] == 0 && g_rec[1].buf[187] == 1, "in order");
}

/* 3. one-packet interval */
static void test_one_packet_interval(void)
{
	uint8_t ts[188 * 2];
	struct rist_pcr_cut c;

	printf("3. one-packet interval\n");
	mk_pkt(ts + 0, 0x0101, true, 0);
	mk_pkt(ts + 188, 0x0101, true, 1);

	reset();
	rist_pcr_cut_init(&c, 0x0101);
	rist_pcr_cut_feed(&c, ts, sizeof(ts), (struct rist_ctx *)1);
	check(g_nrec == 1 && g_rec[0].len == 188, "single 188-byte payload");
}

/* 4. bytes before the first PCR are discarded */
static void test_pre_first_pcr(void)
{
	uint8_t ts[188 * 5];
	struct rist_pcr_cut c;
	int i;

	printf("4. pre-first-PCR discard\n");
	for (i = 0; i < 4; i++) mk_pkt(ts + i * 188, 0x0202, false, (uint8_t)i);
	mk_pkt(ts + 4 * 188, 0x0101, true, 99);

	reset();
	rist_pcr_cut_init(&c, 0x0101);
	rist_pcr_cut_feed(&c, ts, sizeof(ts), (struct rist_ctx *)1);

	check(g_nrec == 0, "nothing emitted before the first PCR");
	check(c.dropped_pre_pcr == 4, "the 4 leading packets were dropped");
	check(c.n_pkts == 1 && c.pending[187] == 99, "the PCR packet is pending as offset 0");
}

/* 5. resync on a corrupted sync byte */
static void test_resync(void)
{
	uint8_t ts[188 * 4];
	struct rist_pcr_cut c;

	printf("5. sync resync\n");
	mk_pkt(ts + 0 * 188, 0x0101, true, 0);
	mk_pkt(ts + 1 * 188, 0x0202, false, 1);
	mk_pkt(ts + 2 * 188, 0x0202, false, 2);
	mk_pkt(ts + 3 * 188, 0x0101, true, 3);
	ts[1 * 188] = 0x00;      /* destroy one sync byte */

	reset();
	rist_pcr_cut_init(&c, 0x0101);
	rist_pcr_cut_feed(&c, ts, sizeof(ts), (struct rist_ctx *)1);
	check(c.bad_sync > 0, "bad sync bytes were counted");
	check(g_nrec >= 1, "the stream recovered and still emitted");
}

/* 6. THE ONE THAT MATTERS: identical output regardless of input framing */
static void test_framing_independence(void)
{
	static uint8_t ts[188 * 200];
	struct rist_pcr_cut c;
	static struct { uint8_t buf[7 * 188]; size_t len; } ref[MAX_REC];
	int refn, i, k;
	const size_t chunks[] = { 1, 3, 188, 189, 7 * 188, 1316, 4096, 48128 };

	printf("6. framing independence (the property the design rests on)\n");
	for (i = 0; i < 200; i++)
		mk_pkt(ts + i * 188, (i % 23 == 0) ? 0x0101 : 0x0202, (i % 23 == 0), (uint8_t)i);

	/* reference: one big feed */
	reset();
	rist_pcr_cut_init(&c, 0x0101);
	rist_pcr_cut_feed(&c, ts, sizeof(ts), (struct rist_ctx *)1);
	refn = g_nrec;
	memcpy(ref, g_rec, sizeof(ref[0]) * (size_t)refn);
	printf("     reference: %d payloads from one %zu-byte feed\n", refn, sizeof(ts));

	for (k = 0; k < (int)(sizeof(chunks) / sizeof(chunks[0])); k++) {
		size_t step = chunks[k], off = 0;
		int ok = 1;

		reset();
		rist_pcr_cut_init(&c, 0x0101);
		while (off < sizeof(ts)) {
			size_t n = (sizeof(ts) - off < step) ? (sizeof(ts) - off) : step;
			rist_pcr_cut_feed(&c, ts + off, n, (struct rist_ctx *)1);
			off += n;
		}
		if (g_nrec != refn) ok = 0;
		for (i = 0; ok && i < refn; i++)
			if (g_rec[i].len != ref[i].len || memcmp(g_rec[i].buf, ref[i].buf, ref[i].len))
				ok = 0;

		char msg[96];
		snprintf(msg, sizeof(msg), "chunk size %-6zu -> identical payloads (%d)", step, g_nrec);
		check(ok, msg);
	}
}

/* 7. the accumulator never grows with interval length */
static void test_bounded(void)
{
	printf("7. bounded accumulator\n");
	printf("     sizeof(struct rist_pcr_cut) = %zu bytes\n", sizeof(struct rist_pcr_cut));
	check(sizeof(struct rist_pcr_cut) < 2048,
	      "state is under 2 KB regardless of interval length");
}

int main(void)
{
	printf("pcr_cut unit tests\n\n");
	test_boundaries();
	test_adjacent_pcr();
	test_one_packet_interval();
	test_pre_first_pcr();
	test_resync();
	test_framing_independence();
	test_bounded();
	printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
