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

// Include RIST headers
#include <librist/librist.h>

// Transport Stream packet size
#define TS_PACKET_SIZE 188
#define RTP_PAYLOADS_PER_MARKER 5
#define MARKER_PID 0x1FF0
#define TS_PACKETS_PER_RTP 7

// Output buffer for continuous TS stream
#define OUTPUT_BUFFER_SIZE (8192 * TS_PACKET_SIZE)  // Large buffer for streaming

// Global state
typedef struct {
    struct rist_ctx *ctx;
    char *input_ip;
    int input_port;
    char *output_ip;
    int output_port;
    int output_fd;
    struct sockaddr_in output_addr;
    
    // Marker generation state
    uint32_t marker_sequence;
    uint32_t rtp_packets_in_block;
    uint32_t current_block_start_seq;
    uint32_t next_block_start_seq;
    uint16_t current_non_null_count;
    uint16_t current_null_count;
    uint32_t current_ssrc;
    
    // Continuous TS output buffer
    uint8_t output_buffer[OUTPUT_BUFFER_SIZE];
    int output_buffer_pos;
    
    int running;
} app_state_t;

static app_state_t g_state = {0};

// CRC-32 table for ISO/IEC 13818-1
static uint32_t crc32_table[256];
static int crc32_table_initialized = 0;

void init_crc32_table(void) {
    if (crc32_table_initialized) return;
    
    uint32_t polynomial = 0x04C11DB7;
    
    for (int i = 0; i < 256; i++) {
        uint32_t crc = i << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000) {
                crc = (crc << 1) ^ polynomial;
            } else {
                crc <<= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = 1;
}

uint32_t calculate_crc32(const uint8_t *data, int length) {
    uint32_t crc = 0xFFFFFFFF;
    
    for (int i = 0; i < length; i++) {
        uint8_t index = ((crc >> 24) ^ data[i]) & 0xFF;
        crc = (crc << 8) ^ crc32_table[index];
    }
    
    return crc;
}

void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("Options:\n");
    printf("  -i, --input-ip IP      RIST input IP address\n");
    printf("  -p, --input-port PORT  RIST input port\n");
    printf("  -o, --output-ip IP     Output UDP IP address\n");
    printf("  -P, --output-port PORT Output UDP port\n");
    printf("  -h, --help            Show this help\n");
}

int parse_arguments(int argc, char *argv[]) {
    int c;
    struct option long_options[] = {
        {"input-ip", required_argument, 0, 'i'},
        {"input-port", required_argument, 0, 'p'},
        {"output-ip", required_argument, 0, 'o'},
        {"output-port", required_argument, 0, 'P'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((c = getopt_long(argc, argv, "i:p:o:P:h", long_options, NULL)) != -1) {
        switch (c) {
            case 'i':
                g_state.input_ip = strdup(optarg);
                break;
            case 'p':
                g_state.input_port = atoi(optarg);
                break;
            case 'o':
                g_state.output_ip = strdup(optarg);
                break;
            case 'P':
                g_state.output_port = atoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                return 1;
            default:
                print_usage(argv[0]);
                return -1;
        }
    }

    if (!g_state.input_ip || !g_state.input_port || 
        !g_state.output_ip || !g_state.output_port) {
        fprintf(stderr, "Error: All IP and port arguments are required\n");
        print_usage(argv[0]);
        return -1;
    }

    return 0;
}

// Use MAIN profile - FSR will exist but shouldn't trigger for simple receivers
int setup_rist_receiver(void) {
    int ret;
    struct rist_logging_settings *logging_settings = NULL;
    struct rist_peer_config *peer_config = NULL;
    struct rist_peer *peer = NULL;
    char url[256];

    // Allocate persistent logging settings using the library function
    ret = rist_logging_set(&logging_settings, RIST_LOG_ERROR, NULL, NULL, NULL, NULL);
    if (ret != 0) {
        fprintf(stderr, "Failed to create logging settings\n");
        return -1;
    }

    // Set global logging to prevent race conditions in FSR threads
    rist_logging_set_global(logging_settings);

    // Initialize RIST context with MAIN profile and proper logging
    ret = rist_receiver_create(&g_state.ctx, RIST_PROFILE_MAIN, logging_settings);
    if (ret != 0) {
        fprintf(stderr, "Failed to create RIST receiver context\n");
        rist_logging_settings_free2(&logging_settings);
        return -1;
    }

    // Free our local logging settings - the context has its own copy
    rist_logging_settings_free2(&logging_settings);

    // Connect to sender as a normal receiver (no special weight needed)
    snprintf(url, sizeof(url), "rist://%s:%d", g_state.input_ip, g_state.input_port);
    printf("Parsing RIST URL: %s (MAIN profile with error-only logging)\n", url);
    
    ret = rist_parse_address2(url, &peer_config);
    if (ret != 0) {
        fprintf(stderr, "Failed to parse RIST address: %s\n", url);
        return -1;
    }

    printf("Creating RIST peer...\n");
    ret = rist_peer_create(g_state.ctx, &peer, peer_config);
    if (ret != 0) {
        fprintf(stderr, "Failed to create RIST peer\n");
        rist_peer_config_free2(&peer_config);
        return -1;
    }

    rist_peer_config_free2(&peer_config);

    printf("Starting RIST receiver...\n");
    ret = rist_start(g_state.ctx);
    if (ret != 0) {
        fprintf(stderr, "Failed to start RIST receiver\n");
        return -1;
    }

    printf("RIST receiver connecting to %s:%d\n", g_state.input_ip, g_state.input_port);
    return 0;
}

int setup_output_socket(void) {
    g_state.output_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_state.output_fd < 0) {
        perror("socket");
        return -1;
    }

    // Set large send buffer for streaming
    int sendbuf = 256 * 1024; // 256KB
    setsockopt(g_state.output_fd, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));

    // Setup multicast if needed
    struct in_addr addr;
    inet_pton(AF_INET, g_state.output_ip, &addr);
    if (IN_MULTICAST(ntohl(addr.s_addr))) {
        unsigned char ttl = 1;
        setsockopt(g_state.output_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    }

    memset(&g_state.output_addr, 0, sizeof(g_state.output_addr));
    g_state.output_addr.sin_family = AF_INET;
    g_state.output_addr.sin_port = htons(g_state.output_port);
    inet_pton(AF_INET, g_state.output_ip, &g_state.output_addr.sin_addr);

    printf("Output UDP socket setup for %s:%d\n", g_state.output_ip, g_state.output_port);
    return 0;
}

void cleanup(void) {
    if (g_state.ctx) {
        rist_destroy(g_state.ctx);
        g_state.ctx = NULL;
    }
    
    // Unset global logging to prevent use-after-free in other threads
    rist_logging_unset_global();
    
    if (g_state.output_fd > 0) {
        close(g_state.output_fd);
        g_state.output_fd = 0;
    }
    if (g_state.input_ip) {
        free(g_state.input_ip);
        g_state.input_ip = NULL;
    }
    if (g_state.output_ip) {
        free(g_state.output_ip);
        g_state.output_ip = NULL;
    }
}

void signal_handler(int sig) {
    (void)sig;
    g_state.running = 0;
}

int is_null_packet(const uint8_t *ts_packet) {
    uint16_t pid = ((ts_packet[1] & 0x1F) << 8) | ts_packet[2];
    return (pid == 0x1FFF);
}

void generate_metadata_marker(uint8_t *marker_packet) {
    memset(marker_packet, 0, TS_PACKET_SIZE);
    
    // TS packet header for PID 0x1FF0
    marker_packet[0] = 0x47; // sync byte
    marker_packet[1] = 0x5F; // TEI=0, PUSI=1, Priority=0, PID high 5 bits
    marker_packet[2] = 0xF0; // PID low 8 bits (0x1FF0)
    marker_packet[3] = 0x10; // Scrambling=00, AFC=01, CC=0000
    
    // Pointer field
    marker_packet[4] = 0x00; // pointer field = 0
    
    // Private section starts at byte 5
    uint8_t *private_section = &marker_packet[5];
    int section_pos = 0;
    
    // Private section header
    private_section[section_pos++] = 0xBF; // table_id = 0xBF
    private_section[section_pos++] = 0x00; // section_syntax_indicator(0) + private_indicator(0) + reserved(00) + length high 4 bits
    private_section[section_pos++] = 0x14; // private_section_length = 20
    
    // marker_sequence_number (32 bits, big endian)
    private_section[section_pos++] = (g_state.marker_sequence >> 24) & 0xFF;
    private_section[section_pos++] = (g_state.marker_sequence >> 16) & 0xFF;
    private_section[section_pos++] = (g_state.marker_sequence >> 8) & 0xFF;
    private_section[section_pos++] = g_state.marker_sequence & 0xFF;
    
    // non_null_count (16 bits, big endian)
    private_section[section_pos++] = (g_state.current_non_null_count >> 8) & 0xFF;
    private_section[section_pos++] = g_state.current_non_null_count & 0xFF;
    
    // null_count (16 bits, big endian)
    private_section[section_pos++] = (g_state.current_null_count >> 8) & 0xFF;
    private_section[section_pos++] = g_state.current_null_count & 0xFF;
    
    // rtp_sequence_start_msb (16 bits) - set to 0
    private_section[section_pos++] = 0x00;
    private_section[section_pos++] = 0x00;
    
    // rtp_sequence_start_lsb (16 bits, big endian)
    private_section[section_pos++] = (g_state.current_block_start_seq >> 8) & 0xFF;
    private_section[section_pos++] = g_state.current_block_start_seq & 0xFF;
    
    // rtp_sequence_next_msb (16 bits) - set to 0
    private_section[section_pos++] = 0x00;
    private_section[section_pos++] = 0x00;
    
    // rtp_sequence_next_lsb (16 bits, big endian)
    private_section[section_pos++] = (g_state.next_block_start_seq >> 8) & 0xFF;
    private_section[section_pos++] = g_state.next_block_start_seq & 0xFF;
    
    // source_ssrc (32 bits, big endian)
    private_section[section_pos++] = (g_state.current_ssrc >> 24) & 0xFF;
    private_section[section_pos++] = (g_state.current_ssrc >> 16) & 0xFF;
    private_section[section_pos++] = (g_state.current_ssrc >> 8) & 0xFF;
    private_section[section_pos++] = g_state.current_ssrc & 0xFF;
    
    // Calculate CRC-32
    uint32_t crc = calculate_crc32(private_section, section_pos);
    
    // CRC_32 (32 bits, big endian)
    private_section[section_pos++] = (crc >> 24) & 0xFF;
    private_section[section_pos++] = (crc >> 16) & 0xFF;
    private_section[section_pos++] = (crc >> 8) & 0xFF;
    private_section[section_pos++] = crc & 0xFF;
    
    // Fill remaining with 0xFF
    int remaining = TS_PACKET_SIZE - 5 - section_pos;
    if (remaining > 0) {
        memset(&private_section[section_pos], 0xFF, remaining);
    }
    
    printf("Generated marker: seq=%u, non_null=%u, null=%u, start_rtp=%u, next_rtp=%u, ssrc=0x%08X\n",
           g_state.marker_sequence, g_state.current_non_null_count, g_state.current_null_count,
           g_state.current_block_start_seq, g_state.next_block_start_seq, g_state.current_ssrc);
}

void add_to_output_buffer(const uint8_t *data, int length) {
    // Add data to continuous output buffer
    if (g_state.output_buffer_pos + length <= OUTPUT_BUFFER_SIZE) {
        memcpy(&g_state.output_buffer[g_state.output_buffer_pos], data, length);
        g_state.output_buffer_pos += length;
    }
    
    // Send buffer when it gets reasonably full (or force send for markers)
    if (g_state.output_buffer_pos >= 32 * TS_PACKET_SIZE || length == TS_PACKET_SIZE) {
        ssize_t sent = sendto(g_state.output_fd, g_state.output_buffer, g_state.output_buffer_pos, 0,
                             (struct sockaddr*)&g_state.output_addr, sizeof(g_state.output_addr));
        
        if (sent != g_state.output_buffer_pos) {
            printf("Warning: sendto returned %zd, expected %d\n", sent, g_state.output_buffer_pos);
        }
        
        g_state.output_buffer_pos = 0; // Reset buffer
    }
}

void insert_marker_and_send_block() {
    // Generate marker
    uint8_t marker_packet[TS_PACKET_SIZE];
    generate_metadata_marker(marker_packet);
    
    // Add marker to continuous TS stream
    add_to_output_buffer(marker_packet, TS_PACKET_SIZE);
    
    printf("Inserted marker into continuous TS stream\n");
    
    // Reset counters for next block
    g_state.marker_sequence++;
    g_state.rtp_packets_in_block = 0;
    g_state.current_non_null_count = 0;
    g_state.current_null_count = 0;
    g_state.current_block_start_seq = g_state.next_block_start_seq;
}

int process_rtp_packet(const struct rist_data_block *data_block) {
    if (!data_block || !data_block->payload || data_block->payload_len <= 0) {
        return -1;
    }
    
    const uint8_t *ts_data = data_block->payload;
    int ts_payload_size = data_block->payload_len;
    uint32_t rtp_sequence = (uint32_t)data_block->seq;
    uint32_t ssrc = data_block->flow_id;
    
    printf("RTP Seq: %u, SSRC: 0x%08X, Size: %d bytes\n", 
           rtp_sequence, ssrc, ts_payload_size);
    
    g_state.current_ssrc = ssrc;
    
    // Verify exactly 7 TS packets
    int actual_ts_packets = ts_payload_size / TS_PACKET_SIZE;
    if (actual_ts_packets != 7) {
        fprintf(stderr, "Warning: Expected 7 TS packets, got %d\n", actual_ts_packets);
    }
    
    // If this is the first packet of a new block
    if (g_state.rtp_packets_in_block == 0) {
        g_state.current_block_start_seq = rtp_sequence;
    }
    
    // Process each TS packet and add to continuous stream
    for (int i = 0; i < actual_ts_packets; i++) {
        const uint8_t *ts_packet = ts_data + (i * TS_PACKET_SIZE);
        
        if (ts_packet[0] != 0x47) {
            fprintf(stderr, "Warning: Invalid TS sync byte at packet %d\n", i);
            continue;
        }
        
        // Count NULL vs non-NULL packets
        if (is_null_packet(ts_packet)) {
            g_state.current_null_count++;
        } else {
            g_state.current_non_null_count++;
        }
        
        // Add each TS packet to the continuous output stream
        add_to_output_buffer(ts_packet, TS_PACKET_SIZE);
    }
    
    g_state.rtp_packets_in_block++;
    g_state.next_block_start_seq = rtp_sequence + 1;
    
    // Check if block is complete - insert marker
    if (g_state.rtp_packets_in_block >= RTP_PAYLOADS_PER_MARKER) {
        insert_marker_and_send_block();
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    int ret;
    
    init_crc32_table();
    
    g_state.running = 1;
    g_state.marker_sequence = 1;
    
    ret = parse_arguments(argc, argv);
    if (ret != 0) {
        return (ret > 0) ? 0 : 1;
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (setup_rist_receiver() != 0) {
        cleanup();
        return 1;
    }
    
    if (setup_output_socket() != 0) {
        cleanup();
        return 1;
    }
    
    printf("Starting RIST to Raw TS converter...\n");
    printf("Input: %s:%d (RIST)\n", g_state.input_ip, g_state.input_port);
    printf("Output: %s:%d (Raw TS with embedded markers)\n", g_state.output_ip, g_state.output_port);
    
    // Main loop
    while (g_state.running) {
        struct rist_data_block *data_block = NULL;
        
        int queue_size = rist_receiver_data_read2(g_state.ctx, &data_block, 5);
        
        if (queue_size > 0) {
            if (data_block && data_block->payload) {
                process_rtp_packet(data_block);
                rist_receiver_data_block_free2(&data_block);
            }
        }
        
        usleep(1000);
    }
    
    // Flush any remaining data
    if (g_state.output_buffer_pos > 0) {
        sendto(g_state.output_fd, g_state.output_buffer, g_state.output_buffer_pos, 0,
               (struct sockaddr*)&g_state.output_addr, sizeof(g_state.output_addr));
    }
    
    printf("Shutting down...\n");
    cleanup();
    return 0;
}