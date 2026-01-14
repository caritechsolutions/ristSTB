// rist_rtp_to_ts_markers.c
// RIST Main Profile (GRE/VSF) -> RAW TS with TR-06-4-7 metadata markers.
// Richard Rawlins / ChatGPT (GPT-5 Thinking)

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TS_PACKET_SIZE            188
#define TS_PACKETS_PER_RTP        7
#define DEFAULT_MARKER_INTERVAL   5         // RTP payloads per marker
#define METADATA_MARKER_PID       0x1FF0
#define METADATA_MARKER_TABLE_ID  0xBF
#define RTP_HEADER_MIN            12

#define TS_SYNC_BYTE              0x47
#define TS_PAYLOAD_UNIT_START     0x40
#define TS_ADAPTATION_FIELD_NONE  0x10

// RIST/VSF
#define VSF_PROTOCOL_TYPE         0x88B5
#define VSF_SUBTYPE_KEEPALIVE     0x8000

// GRE flags bits (RFC 2784 + RIST MP usage)
#define GRE_FLAG_C    0x80 // checksum present (not used here)
#define GRE_FLAG_R    0x40 // routing present
#define GRE_FLAG_K    0x20 // key present
#define GRE_FLAG_S    0x10 // sequence present
#define GRE_FLAG_s    0x08 // strict source route (unused)
#define GRE_VERSION_MASK 0x07

// -------------------- CRC32/MPEG-2 PSI --------------------
static uint32_t crc32_table[256];
static int crc32_initd = 0;

static void crc32_init(void) {
    if (crc32_initd) return;
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i << 24;
        for (int j = 0; j < 8; j++)
            c = (c & 0x80000000) ? ((c << 1) ^ 0x04C11DB7) : (c << 1);
        crc32_table[i] = c;
    }
    crc32_initd = 1;
}
static uint32_t mpeg_crc32(const uint8_t *buf, size_t len) {
    crc32_init();
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ crc32_table[((crc >> 24) ^ buf[i]) & 0xFF];
    return crc;
}

// -------------------- Time utils --------------------
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

// -------------------- Config & State --------------------
struct config {
    char rist_sender_addr[64];
    int  rist_sender_port;
    char out_addr[64];       // multicast or unicast
    int  out_port;
    char iface_addr[64];     // egress interface for multicast
    int  insert_every;       // RTP payloads per marker
    int  keepalive_ms;       // GRE keepalive period
    int  include_gre_key;    // send Key field in GRE keepalive
};

struct pacing {
    uint32_t last_rtp_ts;
    uint64_t last_send_us;
    int initialized;
};

struct state {
    // marker-related
    uint32_t marker_sequence;
    uint16_t rtp_count_in_block;   // 0..insert_every-1
    uint16_t block_start_rtp_seq;
    uint32_t current_ssrc;
    uint16_t non_null_packet_count;
    uint16_t null_packet_count;
    uint8_t  ts_cc;                // continuity counter for marker PID

    // pacing
    struct pacing pace;

    // GRE keepalive
    uint8_t  mac_addr[6];
    uint64_t last_ka_us;
    uint32_t gre_seq;
    uint32_t gre_key;

    // misc
    int running;
};

static void gen_mac(uint8_t mac[6]) {
    static int seeded = 0;
    if (!seeded) { srand((unsigned)time(NULL) ^ getpid()); seeded = 1; }
    for (int i = 0; i < 6; i++) mac[i] = rand() & 0xFF;
    mac[0] |= 0x02;   // locally administered
    mac[0] &= 0xFE;   // unicast
}

// -------------------- RTP parsing --------------------
static int rtp_header_len(const uint8_t *p, int pkt_len) {
    if (pkt_len < RTP_HEADER_MIN) return -1;
    if ((p[0] & 0xC0) != 0x80)    return -1; // V=2
    int cc = p[0] & 0x0F;
    int len = 12 + 4*cc;
    if (pkt_len < len) return -1;
    if (p[0] & 0x10) {
        if (pkt_len < len + 4) return -1;
        int ext_words = (p[len+2] << 8) | p[len+3];
        len += 4 + 4*ext_words;
        if (pkt_len < len) return -1;
    }
    return len;
}

struct rtp_info {
    uint16_t seq;
    uint32_t ts;
    uint32_t ssrc;
    const uint8_t *payload;
    int payload_len;
};

static int parse_rtp(const uint8_t *buf, int len, struct rtp_info *out) {
    int hlen = rtp_header_len(buf, len);
    if (hlen < 0) return -1;
    out->seq  = (uint16_t)ntohs(*(uint16_t*)(buf+2));
    out->ts   = ntohl(*(uint32_t*)(buf+4));
    out->ssrc = ntohl(*(uint32_t*)(buf+8));
    out->payload = buf + hlen;
    out->payload_len = len - hlen;
    return 0;
}

// -------------------- GRE/VSF parsing --------------------
static int parse_gre_vsf(const uint8_t *pkt, int len,
                         uint16_t *vsf_subtype,
                         const uint8_t **payload, int *payload_len)
{
    if (len < 8) return -1;
    uint8_t flags1 = pkt[0];
    uint8_t flags2 = pkt[1];
    (void)flags2;
    uint16_t ptype = ntohs(*(uint16_t*)(pkt+2));
    if ((flags1 & GRE_VERSION_MASK) != 0) return -1; // only version 0
    if (ptype != VSF_PROTOCOL_TYPE)       return -1;

    int off = 4;
    uint32_t key = 0, seq = 0;
    if (flags1 & GRE_FLAG_K) { if (len < off+4) return -1; key = ntohl(*(uint32_t*)(pkt+off)); off += 4; (void)key; }
    if (flags1 & GRE_FLAG_S) { if (len < off+4) return -1; seq = ntohl(*(uint32_t*)(pkt+off)); off += 4; (void)seq; }

    if (len < off + 4) return -1;
    uint16_t vsf_type = ntohs(*(uint16_t*)(pkt+off));      // should be 0x88B5 as well
    *vsf_subtype      = ntohs(*(uint16_t*)(pkt+off+2));
    (void)vsf_type;

    off += 4;
    if (len < off) return -1;
    *payload     = pkt + off;
    *payload_len = len - off;
    return 0;
}

// -------------------- Sockets --------------------
static int make_rtp_sock(const struct config *cfg, struct sockaddr_in *peer) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket rtp"); return -1; }
    memset(peer, 0, sizeof(*peer));
    peer->sin_family = AF_INET;
    peer->sin_port   = htons(cfg->rist_sender_port);
    inet_pton(AF_INET, cfg->rist_sender_addr, &peer->sin_addr);
    if (connect(s, (struct sockaddr*)peer, sizeof(*peer)) < 0) {
        perror("connect rtp"); close(s); return -1;
    }
    return s;
}

static int make_out_sock(const struct config *cfg, struct sockaddr_in *dst) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket out"); return -1; }
    int ttl = 1, loop = 0;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    if (cfg->iface_addr[0]) {
        struct in_addr iface; inet_pton(AF_INET, cfg->iface_addr, &iface);
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
    }
    memset(dst, 0, sizeof(*dst));
    dst->sin_family = AF_INET;
    dst->sin_port   = htons(cfg->out_port);
    inet_pton(AF_INET, cfg->out_addr, &dst->sin_addr);
    return s;
}

// -------------------- GRE Keepalive --------------------

static int send_gre_keepalive(int sock, const struct sockaddr_in *peer,
                              struct state *st, const struct config *cfg)
{
    (void)cfg; // not used now
    uint8_t pkt[64];
    int off = 0;

    // GRE header (version 0) — **Sequence present ONLY**, no Key
    uint8_t flags1 = GRE_FLAG_S;          // <-- K removed, only S
    pkt[off++] = flags1;
    pkt[off++] = 0x00;
    *(uint16_t*)(pkt+off) = htons(VSF_PROTOCOL_TYPE); off += 2;

    // Only sequence field follows
    *(uint32_t*)(pkt+off) = htonl(st->gre_seq++); off += 4;

    // VSF header
    *(uint16_t*)(pkt+off) = htons(VSF_PROTOCOL_TYPE);      off += 2;
    *(uint16_t*)(pkt+off) = htons(VSF_SUBTYPE_KEEPALIVE);  off += 2;

    // Keepalive payload: MAC + caps
    memcpy(pkt+off, st->mac_addr, 6); off += 6;
    pkt[off++] = 0x25;  // capabilities1
    pkt[off++] = 0x20;  // capabilities2

    int rc = sendto(sock, pkt, off, 0, (const struct sockaddr*)peer, sizeof(*peer));
    if (rc < 0) perror("send keepalive");
    return rc;
}

// -------------------- Marker section & TS packet --------------------
static void build_and_send_marker(int out_sock, const struct sockaddr_in *dst,
                                  struct state *st, uint32_t rtp_ts)
{
    uint8_t ts[TS_PACKET_SIZE];
    uint8_t section[64]; // enough for our fields + CRC
    size_t off = 0;

    // PSI private section (0xBF)
    section[off++] = METADATA_MARKER_TABLE_ID;

    // We’ll fill section_length after body is known.
    size_t sfl_pos = off;
    off += 2;

    // Body (network order fields)
    struct __attribute__((packed)) body {
        uint32_t marker_seq;
        uint16_t non_null;
        uint16_t nulls;
        uint16_t rtp_start_msb;
        uint16_t rtp_start_lsb;
        uint16_t rtp_next_msb;
        uint16_t rtp_next_lsb;
        uint32_t ssrc;
    } b;

    uint16_t next = (uint16_t)(st->block_start_rtp_seq + st->rtp_count_in_block);
    // Since we insert before incrementing count for next block, 'next' is start + interval

    b.marker_seq     = htobe32(st->marker_sequence);
    b.non_null       = htobe16(st->non_null_packet_count);
    b.nulls          = htobe16(st->null_packet_count);
    b.rtp_start_msb  = htobe16(0);
    b.rtp_start_lsb  = htobe16(st->block_start_rtp_seq);
    b.rtp_next_msb   = htobe16(0);
    b.rtp_next_lsb   = htobe16((uint16_t)(st->block_start_rtp_seq + st->rtp_count_in_block));
    b.ssrc           = htobe32(st->current_ssrc);

    memcpy(section + off, &b, sizeof(b)); off += sizeof(b);

    uint16_t section_length = (uint16_t)(sizeof(b) + 4); // bytes after this field up to CRC included
    uint16_t sfl = (0u << 15) | (0u << 14) | (3u << 12) | (section_length & 0x0FFF);
    *(uint16_t*)(section + sfl_pos) = htobe16(sfl);

    uint32_t crc = htobe32(mpeg_crc32(section, off));
    memcpy(section + off, &crc, 4); off += 4;

    // Build TS packet
    memset(ts, 0xFF, sizeof(ts));
    ts[0] = TS_SYNC_BYTE;
    ts[1] = TS_PAYLOAD_UNIT_START | ((METADATA_MARKER_PID >> 8) & 0x1F);
    ts[2] = (uint8_t)(METADATA_MARKER_PID & 0xFF);
    ts[3] = TS_ADAPTATION_FIELD_NONE | (st->ts_cc & 0x0F);
    ts[4] = 0x00; // pointer_field to start immediately
    memcpy(&ts[5], section, off);
    st->ts_cc = (st->ts_cc + 1) & 0x0F;

    // pace & send
    // (We call the same pacing routine used for TS from RTP.)
    // For simplicity here we just send with the pacing state.
    // The caller sends this once per N RTP packets using current RTP TS.
    (void)rtp_ts;
    if (sendto(out_sock, ts, TS_PACKET_SIZE, 0,
               (const struct sockaddr*)dst, sizeof(*dst)) < 0) {
        perror("sendto marker");
    }
}

// -------------------- Pacing --------------------
static void pace_ts(struct state *st, uint32_t rtp_ts, int ts_index_in_rtp) {
    // We spread the inter-RTP interval across 7 TS equally.
    uint64_t now = now_us();
    if (!st->pace.initialized) {
        st->pace.initialized = 1;
        st->pace.last_rtp_ts = rtp_ts;
        st->pace.last_send_us = now;
        return;
    }
    uint32_t dts = rtp_ts - st->pace.last_rtp_ts; // wrap-safe arithmetic
    uint64_t interval_us = (uint64_t)dts * 1000000ULL / 90000ULL;
    // per-TS share
    uint64_t per_ts = (TS_PACKETS_PER_RTP > 0) ? (interval_us / TS_PACKETS_PER_RTP) : 0;
    uint64_t due = st->pace.last_send_us + per_ts;
    if (now < due) usleep((useconds_t)(due - now));
    st->pace.last_send_us = now_us();
    st->pace.last_rtp_ts  = rtp_ts; // for next RTP, we keep the same ts until it changes
    (void)ts_index_in_rtp;
}

// -------------------- Main processing --------------------
static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -r <ip>      RIST sender IP (default 192.168.110.107)\n"
        "  -p <port>    RIST sender port (default 5554)\n"
        "  -o <ip>      Output TS IP (mcast/unicast) (default 239.2.2.2)\n"
        "  -P <port>    Output TS port (default 5001)\n"
        "  -I <ip>      Egress interface IP for multicast (default 0.0.0.0)\n"
        "  --insert-every N  RTP payloads per marker (default 5)\n"
        "  --keepalive-ms N  GRE keepalive period ms (default 1000)\n"
        "  --gre-key <hex>   GRE Key to include in keepalives (default random)\n"
        , p);
}

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig){ (void)sig; g_stop = 1; }

int main(int argc, char **argv) {
    struct config cfg = {
        .rist_sender_addr = "192.168.110.107",
        .rist_sender_port = 5554,
        .out_addr         = "239.2.2.2",
        .out_port         = 5001,
        .iface_addr       = "0.0.0.0",
        .insert_every     = DEFAULT_MARKER_INTERVAL,
        .keepalive_ms     = 1000,
        .include_gre_key  = 1,
    };

    // Parse args
    int opt;
    while ((opt = getopt(argc, argv, "r:p:o:P:I:h")) != -1) {
        switch (opt) {
            case 'r': strncpy(cfg.rist_sender_addr, optarg, sizeof(cfg.rist_sender_addr)-1); break;
            case 'p': cfg.rist_sender_port = atoi(optarg); break;
            case 'o': strncpy(cfg.out_addr, optarg, sizeof(cfg.out_addr)-1); break;
            case 'P': cfg.out_port = atoi(optarg); break;
            case 'I': strncpy(cfg.iface_addr, optarg, sizeof(cfg.iface_addr)-1); break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }
    // Long options parsed manually
    for (int i = optind; i < argc; i++) {
        if (!strcmp(argv[i], "--insert-every") && i+1 < argc) {
            cfg.insert_every = atoi(argv[++i]);
            if (cfg.insert_every <= 0) cfg.insert_every = DEFAULT_MARKER_INTERVAL;
        } else if (!strcmp(argv[i], "--keepalive-ms") && i+1 < argc) {
            cfg.keepalive_ms = atoi(argv[++i]);
            if (cfg.keepalive_ms < 100) cfg.keepalive_ms = 100;
        } else if (!strcmp(argv[i], "--gre-key") && i+1 < argc) {
            unsigned key = 0; sscanf(argv[++i], "%x", &key);
            // set later into state
        }
    }

    printf("Starting RIST RTP ? RAW TS with Markers\n");
    printf(" Upstream: %s:%d (RIST MP)\n", cfg.rist_sender_addr, cfg.rist_sender_port);
    printf(" Output  : %s:%d (RAW TS)\n", cfg.out_addr, cfg.out_port);
    printf(" Insert marker every %d RTP payloads\n", cfg.insert_every);

    struct sockaddr_in peer, dst;
    int rtp_sock = make_rtp_sock(&cfg, &peer);
    if (rtp_sock < 0) return 1;
    int out_sock = make_out_sock(&cfg, &dst);
    if (out_sock < 0) { close(rtp_sock); return 1; }

    struct state st = {0};
    gen_mac(st.mac_addr);
    st.gre_seq = 1;
    st.gre_key = (uint32_t)rand();
    st.running = 1;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    uint8_t buf[2048];

    // Kick initial keepalive burst
    for (int i = 0; i < 3; i++) send_gre_keepalive(rtp_sock, &peer, &st, &cfg);
    st.last_ka_us = now_us();

    while (!g_stop) {
        // Keepalive tick
        uint64_t t = now_us();
        if (t - st.last_ka_us >= (uint64_t)cfg.keepalive_ms * 1000ULL) {
            send_gre_keepalive(rtp_sock, &peer, &st, &cfg);
            st.last_ka_us = t;
        }

        // Wait for data with a small timeout
        fd_set rfds; FD_ZERO(&rfds); FD_SET(rtp_sock, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        int n = select(rtp_sock+1, &rfds, NULL, NULL, &tv);
        if (n < 0) { if (errno == EINTR) continue; perror("select"); break; }
        if (n == 0) continue;

        ssize_t got = recv(rtp_sock, buf, sizeof(buf), 0);
        if (got <= 0) { if (errno == EINTR) continue; if (got < 0) perror("recv"); continue; }

        // Parse GRE ? VSF
        uint16_t vsf_subtype = 0;
        const uint8_t *inner = NULL; int inner_len = 0;
        if (parse_gre_vsf(buf, (int)got, &vsf_subtype, &inner, &inner_len) < 0) {
            // Not VSF GRE (could be plain RTP if sender not using GRE). Try raw RTP as fallback.
            inner = buf; inner_len = (int)got;
        } else {
            if (vsf_subtype == VSF_SUBTYPE_KEEPALIVE) {
                // ignore keepalive frames
                continue;
            }
        }

        // Parse RTP
        struct rtp_info rtp;
        if (parse_rtp(inner, inner_len, &rtp) < 0) {
            // Not RTP — ignore
            continue;
        }

        // First in block: remember start sequence
        if (st.rtp_count_in_block == 0) {
            st.block_start_rtp_seq = rtp.seq;
        }
        st.current_ssrc = rtp.ssrc;

        // Insert marker before starting a new interval (i.e., after 'insert_every' RTP already sent)
        if (st.rtp_count_in_block == cfg.insert_every) {
            // Finalize counts for previous block and insert marker
            st.non_null_packet_count = cfg.insert_every * TS_PACKETS_PER_RTP;
            st.null_packet_count = 0;
            build_and_send_marker(out_sock, &dst, &st, rtp.ts);
            st.marker_sequence++;
            st.rtp_count_in_block = 0;
            st.block_start_rtp_seq = rtp.seq;
        }

        // Expect 7*188 bytes in payload; proceed defensively
        if (rtp.payload_len < TS_PACKETS_PER_RTP * TS_PACKET_SIZE) {
            // Some senders may add padding; if smaller, skip
            fprintf(stderr, "RTP payload too small: %d\n", rtp.payload_len);
            continue;
        }

        // Forward 7 TS packets with pacing
        for (int i = 0; i < TS_PACKETS_PER_RTP; i++) {
            const uint8_t *ts = rtp.payload + i * TS_PACKET_SIZE;
            if (ts[0] != TS_SYNC_BYTE) {
                fprintf(stderr, "Bad TS sync @%d (0x%02X)\n", i, ts[0]);
                continue;
            }
            pace_ts(&st, rtp.ts, i);
            if (sendto(out_sock, ts, TS_PACKET_SIZE, 0,
                       (const struct sockaddr*)&dst, sizeof(dst)) < 0) {
                perror("sendto TS");
            }
        }

        st.rtp_count_in_block++;
    }

    close(out_sock);
    close(rtp_sock);
    printf("Stopped.\n");
    return 0;
}
