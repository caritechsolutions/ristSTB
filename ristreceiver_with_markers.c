// rist_rx_markers.c
// RIST Receiver with VSF TR-06-4 Part 7 marker insertion
// CRITICAL FIX: Maintain 7-TS-packet UDP payload alignment
// Always send complete 1316-byte UDP packets (7 * 188)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

#include <librist/librist.h>

// -------------------- Constants --------------------
#define TS_PACKET_SIZE             188
#define TS_PACKETS_PER_RTP         7
#define UDP_PAYLOAD_SIZE           (TS_PACKETS_PER_RTP * TS_PACKET_SIZE)  // 1316 bytes
#define RTP_PAYLOADS_PER_MARKER    5
#define MARKER_PID                 0x1FF0

// -------------------- Global state --------------------
static struct rist_ctx *ctx = NULL;
static struct rist_logging_settings logging_settings = LOGGING_SETTINGS_INITIALIZER;
static int running = 1;
static pthread_mutex_t signal_lock = PTHREAD_MUTEX_INITIALIZER;
static int signal_received = 0;

// Output socket
static int output_fd = -1;
static struct sockaddr_in output_addr;

// Marker generation state
static uint32_t marker_sequence = 1;
static uint32_t rtp_packets_in_block = 0;
static uint16_t current_block_start_rtp_seq = 0;
static uint16_t current_non_null_count = 0;
static uint16_t current_null_count = 0;
static uint32_t current_ssrc = 0;
static uint8_t  marker_cc = 0;
static bool first_marker_sent = false;

// CRITICAL: UDP payload accumulator to maintain 7-packet alignment
static uint8_t udp_buffer[UDP_PAYLOAD_SIZE];
static size_t udp_buffer_fill = 0;

// CRC-32 table (ISO/IEC 13818-1)
static uint32_t crc32_table[256];
static int crc32_table_initialized = 0;

// -------------------- Signal handling --------------------
static void signal_handler(int sig) {
    pthread_mutex_lock(&signal_lock);
    signal_received = sig;
    running = 0;
    pthread_mutex_unlock(&signal_lock);
}

// -------------------- CRC utilities --------------------
static void init_crc32_table(void) {
    if (crc32_table_initialized) return;
    const uint32_t poly = 0x04C11DB7;
    for (int i = 0; i < 256; i++) {
        uint32_t crc = (uint32_t)i << 24;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80000000) ? ((crc << 1) ^ poly) : (crc << 1);
        crc32_table[i] = crc;
    }
    crc32_table_initialized = 1;
}

static uint32_t calculate_crc32(const uint8_t *data, int length) {
    if (!crc32_table_initialized) init_crc32_table();
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < length; i++) {
        uint8_t idx = (uint8_t)(((crc >> 24) ^ data[i]) & 0xFF);
        crc = (crc << 8) ^ crc32_table[idx];
    }
    return crc;
}

// -------------------- TS helpers --------------------
static int is_null_packet(const uint8_t *ts_packet) {
    if (ts_packet[0] != 0x47) return 0;
    uint16_t pid = ((ts_packet[1] & 0x1F) << 8) | ts_packet[2];
    return (pid == 0x1FFF);
}

// -------------------- UDP buffer management --------------------
// Flush the UDP buffer when it's full (7 TS packets)
static void flush_udp_buffer(void) {
    if (udp_buffer_fill == 0) return;
    
    if (udp_buffer_fill != UDP_PAYLOAD_SIZE) {
        fprintf(stderr, "[WARNING] Flushing partial UDP buffer: %zu bytes (expected %d)\n",
                udp_buffer_fill, UDP_PAYLOAD_SIZE);
    }
    
    ssize_t sent = sendto(output_fd, udp_buffer, udp_buffer_fill, 0,
                          (struct sockaddr*)&output_addr, sizeof(output_addr));
    if (sent != (ssize_t)udp_buffer_fill) {
        fprintf(stderr, "[UDP] Send error: %s\n", strerror(errno));
    }
    
    udp_buffer_fill = 0;
}

// Add a TS packet to the UDP buffer, flushing when full
static void add_ts_to_udp_buffer(const uint8_t *ts_packet) {
    if (udp_buffer_fill + TS_PACKET_SIZE > UDP_PAYLOAD_SIZE) {
        flush_udp_buffer();
    }
    
    memcpy(&udp_buffer[udp_buffer_fill], ts_packet, TS_PACKET_SIZE);
    udp_buffer_fill += TS_PACKET_SIZE;
    
    if (udp_buffer_fill == UDP_PAYLOAD_SIZE) {
        flush_udp_buffer();
    }
}

// -------------------- Marker builder --------------------
static void generate_metadata_marker(uint8_t *marker_packet) {
    memset(marker_packet, 0xFF, TS_PACKET_SIZE);

    // TS header for PID 0x1FF0, payload only, PUSI=1
    marker_packet[0] = 0x47;
    marker_packet[1] = 0x40 | ((MARKER_PID >> 8) & 0x1F);
    marker_packet[2] = (uint8_t)(MARKER_PID & 0xFF);
    marker_packet[3] = 0x10 | (marker_cc & 0x0F);
    marker_cc = (uint8_t)((marker_cc + 1) & 0x0F);

    marker_packet[4] = 0x00;  // pointer_field

    uint8_t *sec = &marker_packet[5];
    int off = 0;

    sec[off++] = 0xBF;  // table_id

    const uint16_t section_length = 24;
    sec[off++] = (uint8_t)(0x30 | ((section_length >> 8) & 0x0F));
    sec[off++] = (uint8_t)(section_length & 0xFF);

    // marker_sequence_number (32)
    sec[off++] = (uint8_t)((marker_sequence >> 24) & 0xFF);
    sec[off++] = (uint8_t)((marker_sequence >> 16) & 0xFF);
    sec[off++] = (uint8_t)((marker_sequence >>  8) & 0xFF);
    sec[off++] = (uint8_t)( marker_sequence        & 0xFF);

    // non_null_count (16)
    sec[off++] = (uint8_t)((current_non_null_count >> 8) & 0xFF);
    sec[off++] = (uint8_t)( current_non_null_count       & 0xFF);

    // null_count (16)
    sec[off++] = (uint8_t)((current_null_count >> 8) & 0xFF);
    sec[off++] = (uint8_t)( current_null_count       & 0xFF);

    // rtp_sequence_start_msb (16)
    sec[off++] = 0x00; sec[off++] = 0x00;

    // rtp_sequence_start_lsb (16)
    sec[off++] = (uint8_t)((current_block_start_rtp_seq >> 8) & 0xFF);
    sec[off++] = (uint8_t)( current_block_start_rtp_seq       & 0xFF);

    // rtp_sequence_next_msb (16)
    sec[off++] = 0x00; sec[off++] = 0x00;

    // rtp_sequence_next_lsb (16)
    uint16_t next_lsb = (uint16_t)(current_block_start_rtp_seq + RTP_PAYLOADS_PER_MARKER);
    sec[off++] = (uint8_t)((next_lsb >> 8) & 0xFF);
    sec[off++] = (uint8_t)( next_lsb       & 0xFF);

    // source_ssrc (32)
    sec[off++] = (uint8_t)((current_ssrc >> 24) & 0xFF);
    sec[off++] = (uint8_t)((current_ssrc >> 16) & 0xFF);
    sec[off++] = (uint8_t)((current_ssrc >>  8) & 0xFF);
    sec[off++] = (uint8_t)( current_ssrc        & 0xFF);

    // CRC
    uint32_t crc = calculate_crc32(sec, off);
    sec[off++] = (uint8_t)((crc >> 24) & 0xFF);
    sec[off++] = (uint8_t)((crc >> 16) & 0xFF);
    sec[off++] = (uint8_t)((crc >>  8) & 0xFF);
    sec[off++] = (uint8_t)( crc        & 0xFF);

    printf("[MARKER] seq=%u non_null=%u null=%u start=%u next=%u ssrc=0x%08X (includes marker at block start)\n",
           marker_sequence, current_non_null_count, current_null_count,
           current_block_start_rtp_seq, next_lsb, current_ssrc);
}

// -------------------- RIST data callback --------------------
static int cb_recv(void *arg, struct rist_data_block *b) {
    (void)arg;
    if (!b || !b->payload || b->payload_len <= 0) return -1;

    uint16_t rtp_sequence = (uint16_t)b->seq;
    uint32_t ssrc = b->flow_id;
    current_ssrc = ssrc;

    if (rtp_packets_in_block == 0) {
        current_block_start_rtp_seq = rtp_sequence;
        
        // CRITICAL: If this is NOT the first marker ever, we need to count
        // the previous marker that starts this block
        if (first_marker_sent) {
            current_non_null_count = 1;  // The marker at the start of this block
            current_null_count = 0;
        } else {
            // Very first block - no marker yet
            current_non_null_count = 0;
            current_null_count = 0;
        }
    }

    // Count null/non-null TS packets in this RTP payload
    const uint8_t *ts_data = (const uint8_t *)b->payload;
    int ts_packets = b->payload_len / TS_PACKET_SIZE;
    for (int i = 0; i < ts_packets; i++) {
        const uint8_t *ts = ts_data + i * TS_PACKET_SIZE;
        if (ts[0] != 0x47) continue;
        if (is_null_packet(ts)) current_null_count++;
        else                    current_non_null_count++;
    }

    // Add each TS packet to UDP buffer to maintain 7-packet alignment
    for (int i = 0; i < ts_packets; i++) {
        const uint8_t *ts = ts_data + i * TS_PACKET_SIZE;
        add_ts_to_udp_buffer(ts);
    }

    rtp_packets_in_block++;

    // Insert marker after RTP_PAYLOADS_PER_MARKER RTP packets
    if (rtp_packets_in_block >= RTP_PAYLOADS_PER_MARKER) {
        uint8_t marker_packet[TS_PACKET_SIZE];
        generate_metadata_marker(marker_packet);

        // Add marker to UDP buffer (maintains alignment)
        add_ts_to_udp_buffer(marker_packet);

        // Mark that we've sent at least one marker
        first_marker_sent = true;

        // Prepare for next block
        marker_sequence++;
        rtp_packets_in_block = 0;
        
        // NOTE: We DON'T reset counts here!
        // The next block will reset counts when rtp_packets_in_block == 0
        // and will start at 1 to count the marker we just inserted
        
        current_block_start_rtp_seq = (uint16_t)(current_block_start_rtp_seq + RTP_PAYLOADS_PER_MARKER);
    }

    return 0;
}

// -------------------- Output socket --------------------
static int setup_output_socket(const char *ip, int port) {
    output_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (output_fd < 0) { perror("socket"); return -1; }

    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        fprintf(stderr, "Invalid output IP: %s\n", ip);
        close(output_fd); output_fd = -1; return -1;
    }
    if (IN_MULTICAST(ntohl(addr.s_addr))) {
        unsigned char ttl = 1;
        if (setsockopt(output_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
            perror("setsockopt IP_MULTICAST_TTL");
        }
        printf("Configured multicast output\n");
    }

    memset(&output_addr, 0, sizeof(output_addr));
    output_addr.sin_family = AF_INET;
    output_addr.sin_port   = htons(port);
    output_addr.sin_addr   = addr;

    printf("Output UDP: %s:%d (aligned to %d-byte packets)\n", ip, port, UDP_PAYLOAD_SIZE);
    return 0;
}

// -------------------- Usage --------------------
static void usage(const char *prog) {
    printf("Usage: %s -i <rist_input_url> -o <udp_output_url> [-v <level>]\n", prog);
    printf("Example: %s -i rist://192.168.110.107:5554 -o udp://239.6.6.6:6000\n", prog);
    printf("\nRIST Receiver with VSF TR-06-4 Part 7 Marker Insertion\n");
    printf("- Extracts RTP metadata from RIST data blocks\n");
    printf("- Inserts metadata markers every %d RTP payloads\n", RTP_PAYLOADS_PER_MARKER);
    printf("- Maintains %d-byte UDP packet alignment (%d TS packets)\n", 
           UDP_PAYLOAD_SIZE, TS_PACKETS_PER_RTP);
}

// -------------------- main --------------------
int main(int argc, char *argv[]) {
    char *inputurl = NULL;
    char *outputurl = NULL;
    int c;

    init_crc32_table();

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, signal_handler);

    while ((c = getopt(argc, argv, "i:o:v:h")) != -1) {
        switch (c) {
            case 'i': inputurl  = strdup(optarg); break;
            case 'o': outputurl = strdup(optarg); break;
            case 'v': {
                int lvl = atoi(optarg);
#ifdef RIST_LOG_EMERG
                if (lvl < RIST_LOG_EMERG) lvl = RIST_LOG_EMERG;
#else
                if (lvl < RIST_LOG_ERROR) lvl = RIST_LOG_ERROR;
#endif
                if (lvl > RIST_LOG_DEBUG) lvl = RIST_LOG_DEBUG;
                logging_settings.log_level = (enum rist_log_level)lvl;
                break;
            }
            case 'h':
            default: usage(argv[0]); return 1;
        }
    }

    if (!inputurl || !outputurl) {
        usage(argv[0]); return 1;
    }

    char output_ip[256] = {0};
    int output_port = 0;
    if (sscanf(outputurl, "udp://%255[^:]:%d", output_ip, &output_port) != 2) {
        fprintf(stderr, "ERROR: Could not parse output URL: %s\n", outputurl);
        return 1;
    }

    struct rist_logging_settings *log_ptr = &logging_settings;
    if (rist_logging_set(&log_ptr, logging_settings.log_level, NULL, NULL, NULL, stderr) != 0) {
        fprintf(stderr, "Failed to setup logging!\n");
        return 1;
    }

    printf("VSF TR-06-4 Part 7 RIST Receiver with Marker Multiplexer\n");
    printf("libRIST version: %s  API version: %s\n", librist_version(), librist_api_version());

    if (setup_output_socket(output_ip, output_port) != 0) return 1;

    if (rist_receiver_create(&ctx, RIST_PROFILE_MAIN, &logging_settings) != 0) {
        fprintf(stderr, "ERROR: Could not create RIST receiver context\n");
        return 1;
    }

    struct rist_peer_config *peer_config = NULL;
    if (rist_parse_address2(inputurl, &peer_config) != 0) {
        fprintf(stderr, "ERROR: Could not parse peer options\n");
        rist_destroy(ctx);
        return 1;
    }

    struct rist_peer *peer = NULL;
    if (rist_peer_create(ctx, &peer, peer_config) != 0) {
        fprintf(stderr, "ERROR: Could not add peer connector\n");
        rist_peer_config_free2(&peer_config);
        rist_destroy(ctx);
        return 1;
    }
    rist_peer_config_free2(&peer_config);

    if (rist_start(ctx) != 0) {
        fprintf(stderr, "ERROR: Could not start RIST receiver\n");
        rist_destroy(ctx);
        return 1;
    }

    printf("RIST receiver started successfully\n");
    printf("Input : %s\n", inputurl);
    printf("Output: %s (7-packet aligned UDP)\n", outputurl);
    printf("Marker insertion: Every %d RTP payloads\n\n", RTP_PAYLOADS_PER_MARKER);

    while (running) {
        struct rist_data_block *b = NULL;
        int queue_size = rist_receiver_data_read2(ctx, &b, 5);

        if (queue_size > 0) {
            if (queue_size % 10 == 0 || queue_size > 50) {
                uint32_t flow_id = b ? b->flow_id : 0;
                fprintf(stderr, "WARNING: Falling behind: count %d, flow id %u\n",
                        queue_size, flow_id);
            }
            if (b && b->payload && b->payload_len > 0) {
                cb_recv(NULL, b);
                rist_receiver_data_block_free2(&b);
            }
        }

        pthread_mutex_lock(&signal_lock);
        if (signal_received) {
            printf("\nSignal %d received\n", signal_received);
            pthread_mutex_unlock(&signal_lock);
            break;
        }
        pthread_mutex_unlock(&signal_lock);
    }

    // Flush any remaining data in UDP buffer
    flush_udp_buffer();

    printf("\nShutting down\n");
    printf("Final marker sequence: %u\n", (marker_sequence > 0) ? (marker_sequence - 1) : 0);

    if (ctx) rist_destroy(ctx);
    if (output_fd >= 0) close(output_fd);
    free(inputurl);
    free(outputurl);

    return 0;
}