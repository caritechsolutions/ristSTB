#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#define TS_PACKET_SIZE 188
#define MARKER_PID 0x1FF0

// CRC-32 table for ISO/IEC 13818-1
static uint32_t crc32_table[256];
static int crc32_table_initialized = 0;

typedef struct {
    int socket_fd;
    int running;
    uint32_t packet_count;
    uint32_t marker_count;
    uint32_t last_marker_seq;
} validator_state_t;

static validator_state_t g_state = {0};

void init_crc32_table(void);
uint32_t calculate_crc32(const uint8_t *data, int length);
int setup_receiver_socket(const char *ip, int port);
void analyze_ts_packet(const uint8_t *packet);
void analyze_marker_packet(const uint8_t *packet);
void signal_handler(int sig);

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

int setup_receiver_socket(const char *ip, int port) {
    struct sockaddr_in addr;
    struct ip_mreq mreq;
    int sockfd;
    int opt = 1;
    
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    // Allow reuse of address
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind to multicast address
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }
    
    // Join multicast group
    mreq.imr_multiaddr.s_addr = inet_addr(ip);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt IP_ADD_MEMBERSHIP");
        close(sockfd);
        return -1;
    }
    
    printf("Listening on %s:%d\n", ip, port);
    return sockfd;
}

void analyze_marker_packet(const uint8_t *packet) {
    // Verify TS header
    if (packet[0] != 0x47) {
        printf("  ERROR: Invalid sync byte: 0x%02X\n", packet[0]);
        return;
    }
    
    // Extract PID
    uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];
    if (pid != MARKER_PID) {
        printf("  ERROR: Expected PID 0x%04X, got 0x%04X\n", MARKER_PID, pid);
        return;
    }
    
    // Check PUSI (should be 1 for marker)
    if (!(packet[1] & 0x40)) {
        printf("  ERROR: PUSI bit not set\n");
        return;
    }
    
    // Get pointer field
    uint8_t pointer = packet[4];
    if (pointer != 0) {
        printf("  WARNING: Pointer field is %d, expected 0\n", pointer);
    }
    
    // Private section starts at byte 5
    const uint8_t *section = &packet[5];
    
    // Parse private section header
    uint8_t table_id = section[0];
    uint8_t section_byte1 = section[1];
    uint8_t section_byte2 = section[2];
    
    uint8_t section_syntax_indicator = (section_byte1 >> 7) & 1;
    uint8_t private_indicator = (section_byte1 >> 6) & 1;
    uint16_t section_length = ((section_byte1 & 0x0F) << 8) | section_byte2;
    
    printf("  Private Section:\n");
    printf("    table_id: 0x%02X %s\n", table_id, (table_id == 0xBF) ? "(OK)" : "(ERROR)");
    printf("    section_syntax_indicator: %d %s\n", section_syntax_indicator, (section_syntax_indicator == 0) ? "(OK)" : "(ERROR)");
    printf("    private_indicator: %d %s\n", private_indicator, (private_indicator == 0) ? "(OK)" : "(ERROR)");
    printf("    section_length: %d %s\n", section_length, (section_length == 20) ? "(OK)" : "(ERROR)");
    
    if (section_length != 20) {
        printf("  ERROR: Invalid section length, cannot parse further\n");
        return;
    }
    
    // Parse marker data fields
    uint32_t marker_seq = (section[3] << 24) | (section[4] << 16) | (section[5] << 8) | section[6];
    uint16_t non_null_count = (section[7] << 8) | section[8];
    uint16_t null_count = (section[9] << 8) | section[10];
    uint16_t rtp_start_msb = (section[11] << 8) | section[12];
    uint16_t rtp_start_lsb = (section[13] << 8) | section[14];
    uint16_t rtp_next_msb = (section[15] << 8) | section[16];
    uint16_t rtp_next_lsb = (section[17] << 8) | section[18];
    uint32_t source_ssrc = (section[19] << 24) | (section[20] << 16) | (section[21] << 8) | section[22];
    uint32_t stored_crc = (section[23] << 24) | (section[24] << 16) | (section[25] << 8) | section[26];
    
    printf("    marker_sequence: %u\n", marker_seq);
    printf("    non_null_count: %u\n", non_null_count);
    printf("    null_count: %u\n", null_count);
    printf("    rtp_start: %u:%u\n", rtp_start_msb, rtp_start_lsb);
    printf("    rtp_next: %u:%u\n", rtp_next_msb, rtp_next_lsb);
    printf("    source_ssrc: 0x%08X\n", source_ssrc);
    
    // Verify CRC
    uint32_t calculated_crc = calculate_crc32(section, 23); // CRC over all except CRC field
    printf("    stored_crc: 0x%08X\n", stored_crc);
    printf("    calculated_crc: 0x%08X %s\n", calculated_crc, (stored_crc == calculated_crc) ? "(OK)" : "(ERROR)");
    
    // Check sequence continuity
    if (g_state.marker_count > 0) {
        uint32_t expected_seq = g_state.last_marker_seq + 1;
        if (marker_seq != expected_seq) {
            printf("    WARNING: Sequence gap. Expected %u, got %u\n", expected_seq, marker_seq);
        }
    }
    
    g_state.last_marker_seq = marker_seq;
    g_state.marker_count++;
    
    printf("  Marker validation: %s\n", 
           (table_id == 0xBF && section_syntax_indicator == 0 && private_indicator == 0 && 
            section_length == 20 && stored_crc == calculated_crc) ? "PASS" : "FAIL");
}

void analyze_ts_packet(const uint8_t *packet) {
    g_state.packet_count++;
    
    // Verify sync byte
    if (packet[0] != 0x47) {
        printf("Packet %u: ERROR - Invalid sync byte: 0x%02X\n", g_state.packet_count, packet[0]);
        return;
    }
    
    // Extract PID
    uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];
    
    if (pid == MARKER_PID) {
        printf("Packet %u: MARKER (PID 0x%04X)\n", g_state.packet_count, pid);
        analyze_marker_packet(packet);
    } else if (pid == 0x1FFF) {
        // NULL packet - just count it
        if (g_state.packet_count % 1000 == 0) {
            printf("Packet %u: NULL (processed %u packets, %u markers)\n", 
                   g_state.packet_count, g_state.packet_count, g_state.marker_count);
        }
    } else {
        // Regular TS packet
        if (g_state.packet_count % 1000 == 0) {
            printf("Packet %u: Data PID 0x%04X (processed %u packets, %u markers)\n", 
                   g_state.packet_count, pid, g_state.packet_count, g_state.marker_count);
        }
    }
}

void signal_handler(int sig) {
    (void)sig;
    g_state.running = 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <multicast_ip> <port>\n", argv[0]);
        printf("Example: %s 239.6.6.6 6000\n", argv[0]);
        return 1;
    }
    
    const char *ip = argv[1];
    int port = atoi(argv[2]);
    
    init_crc32_table();
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    g_state.socket_fd = setup_receiver_socket(ip, port);
    if (g_state.socket_fd < 0) {
        return 1;
    }
    
    g_state.running = 1;
    
    printf("TS Marker Validator started\n");
    printf("Looking for markers on PID 0x%04X\n", MARKER_PID);
    printf("Press Ctrl+C to stop\n\n");
    
    uint8_t buffer[65536];
    
    while (g_state.running) {
        ssize_t received = recv(g_state.socket_fd, buffer, sizeof(buffer), 0);
        if (received < 0) {
            if (g_state.running) {
                perror("recv");
            }
            break;
        }
        
        // Process all TS packets in the received data
        for (int i = 0; i + TS_PACKET_SIZE <= received; i += TS_PACKET_SIZE) {
            analyze_ts_packet(&buffer[i]);
        }
        
        // Handle partial packets
        if (received % TS_PACKET_SIZE != 0) {
            printf("WARNING: Received %zd bytes, not multiple of %d\n", received, TS_PACKET_SIZE);
        }
    }
    
    printf("\nFinal Statistics:\n");
    printf("Total packets processed: %u\n", g_state.packet_count);
    printf("Total markers found: %u\n", g_state.marker_count);
    
    close(g_state.socket_fd);
    return 0;
}