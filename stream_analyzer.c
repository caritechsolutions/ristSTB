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

#define TS_PACKET_SIZE 188
#define MARKER_PID 0x1FF0

static int running = 1;
static int input_fd = -1;

// Analysis counters
static uint32_t total_packets = 0;
static uint32_t sync_errors = 0;
static uint32_t marker_packets = 0;
static uint32_t null_packets = 0;
static uint32_t video_packets = 0;
static uint32_t audio_packets = 0;
static uint32_t other_packets = 0;
static uint32_t pcr_packets = 0;

// PID tracking
#define MAX_PIDS 8192
static uint32_t pid_counts[MAX_PIDS] = {0};

void signal_handler(int sig) {
    running = 0;
}

void analyze_packet(const uint8_t *packet) {
    total_packets++;
    
    // Check sync byte
    if (packet[0] != 0x47) {
        sync_errors++;
        printf("SYNC ERROR at packet %u: 0x%02X\n", total_packets, packet[0]);
        return;
    }
    
    // Extract PID
    uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];
    pid_counts[pid]++;
    
    // Classify packet type
    if (pid == 0x1FFF) {
        null_packets++;
    } else if (pid == MARKER_PID) {
        marker_packets++;
        printf("MARKER PACKET #%u found at packet %u\n", marker_packets, total_packets);
        
        // Analyze marker content
        if (packet[1] & 0x40) { // PUSI set
            uint8_t pointer = packet[4];
            if (pointer == 0) {
                uint8_t table_id = packet[5];
                uint16_t section_length = ((packet[6] & 0x0F) << 8) | packet[7];
                printf("  Table ID: 0x%02X, Section Length: %u\n", table_id, section_length);
                
                if (table_id == 0xBF && section_length == 20) {
                    // Extract marker data
                    uint32_t marker_seq = (packet[8] << 24) | (packet[9] << 16) | (packet[10] << 8) | packet[11];
                    uint16_t non_null = (packet[12] << 8) | packet[13];
                    uint16_t null_count = (packet[14] << 8) | packet[15];
                    uint32_t rtp_start = (packet[18] << 8) | packet[19];
                    uint32_t rtp_next = (packet[22] << 8) | packet[23];
                    uint32_t ssrc = (packet[24] << 24) | (packet[25] << 16) | (packet[26] << 8) | packet[27];
                    
                    printf("  Marker Seq: %u, Non-NULL: %u, NULL: %u, RTP Start: %u, RTP Next: %u, SSRC: 0x%08X\n",
                           marker_seq, non_null, null_count, rtp_start, rtp_next, ssrc);
                } else {
                    printf("  INVALID marker format!\n");
                }
            }
        }
    } else if (pid == 0x0000) {
        // PAT packet
        printf("PAT packet found at packet %u\n", total_packets);
    } else if (pid == 0x0001) {
        // CAT packet  
        printf("CAT packet found at packet %u\n", total_packets);
    } else {
        // Check for PCR
        uint8_t afc = (packet[3] >> 4) & 0x03;
        if (afc == 0x02 || afc == 0x03) {
            uint8_t af_length = packet[4];
            if (af_length >= 7 && (packet[5] & 0x10)) {
                pcr_packets++;
                
                // Extract PCR
                uint64_t pcr_base = 0;
                pcr_base = ((uint64_t)packet[6] << 25);
                pcr_base |= ((uint64_t)packet[7] << 17);
                pcr_base |= ((uint64_t)packet[8] << 9);
                pcr_base |= ((uint64_t)packet[9] << 1);
                pcr_base |= ((packet[10] & 0x80) >> 7);
                
                uint16_t pcr_ext = ((packet[10] & 0x01) << 8) | packet[11];
                uint64_t pcr_total = (pcr_base * 300) + pcr_ext;
                
                printf("PCR packet at %u: PID 0x%04X, PCR=%llu (base=%llu, ext=%u)\n", 
                       total_packets, pid, (unsigned long long)pcr_total, 
                       (unsigned long long)pcr_base, pcr_ext);
            }
        }
        
        if (pid >= 0x0020 && pid <= 0x1FFE) {
            other_packets++;
        }
    }
}

void print_summary() {
    printf("\n=== TRANSPORT STREAM ANALYSIS SUMMARY ===\n");
    printf("Total packets analyzed: %u\n", total_packets);
    printf("Sync errors: %u (%.2f%%)\n", sync_errors, (float)sync_errors/total_packets*100);
    printf("NULL packets: %u (%.2f%%)\n", null_packets, (float)null_packets/total_packets*100);
    printf("Marker packets: %u (%.2f%%)\n", marker_packets, (float)marker_packets/total_packets*100);
    printf("PCR packets: %u (%.2f%%)\n", pcr_packets, (float)pcr_packets/total_packets*100);
    printf("Other content packets: %u (%.2f%%)\n", other_packets, (float)other_packets/total_packets*100);
    
    printf("\n=== TOP 20 PIDs BY PACKET COUNT ===\n");
    typedef struct { uint16_t pid; uint32_t count; } pid_count_t;
    pid_count_t sorted_pids[MAX_PIDS];
    int pid_index = 0;
    
    for (int i = 0; i < MAX_PIDS; i++) {
        if (pid_counts[i] > 0) {
            sorted_pids[pid_index].pid = i;
            sorted_pids[pid_index].count = pid_counts[i];
            pid_index++;
        }
    }
    
    // Simple bubble sort for top 20
    for (int i = 0; i < pid_index-1; i++) {
        for (int j = 0; j < pid_index-i-1; j++) {
            if (sorted_pids[j].count < sorted_pids[j+1].count) {
                pid_count_t temp = sorted_pids[j];
                sorted_pids[j] = sorted_pids[j+1];
                sorted_pids[j+1] = temp;
            }
        }
    }
    
    int display_count = (pid_index < 20) ? pid_index : 20;
    for (int i = 0; i < display_count; i++) {
        const char* pid_type = "Unknown";
        if (sorted_pids[i].pid == 0x1FFF) pid_type = "NULL";
        else if (sorted_pids[i].pid == MARKER_PID) pid_type = "MARKER";
        else if (sorted_pids[i].pid == 0x0000) pid_type = "PAT";
        else if (sorted_pids[i].pid == 0x0001) pid_type = "CAT";
        
        printf("PID 0x%04X (%s): %u packets (%.2f%%)\n", 
               sorted_pids[i].pid, pid_type, sorted_pids[i].count,
               (float)sorted_pids[i].count/total_packets*100);
    }
}

int setup_input_socket(const char *ip, int port) {
    input_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (input_fd < 0) {
        perror("socket");
        return -1;
    }
    
    // Set receive buffer
    int recvbuf = 1024 * 1024; // 1MB
    setsockopt(input_fd, SOL_SOCKET, SO_RCVBUF, &recvbuf, sizeof(recvbuf));
    
    // Setup address
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        return -1;
    }
    
    // Bind socket
    if (bind(input_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return -1;
    }
    
    // Join multicast group if needed
    if (IN_MULTICAST(ntohl(addr.sin_addr.s_addr))) {
        struct ip_mreq mreq;
        mreq.imr_multiaddr = addr.sin_addr;
        mreq.imr_interface.s_addr = INADDR_ANY;
        
        if (setsockopt(input_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            perror("setsockopt IP_ADD_MEMBERSHIP");
            return -1;
        }
        printf("Joined multicast group %s\n", ip);
    }
    
    printf("Listening on %s:%d\n", ip, port);
    return 0;
}

void usage(char *prog) {
    printf("Usage: %s -i <udp_ip> -p <port> [-c <packet_count>]\n", prog);
    printf("Example: %s -i 239.6.6.6 -p 6000 -c 1000\n", prog);
    printf("\nAnalyzes transport stream packets received via UDP\n");
}

int main(int argc, char *argv[]) {
    char *ip = NULL;
    int port = 0;
    int max_packets = 0; // 0 = unlimited
    int c;
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    while ((c = getopt(argc, argv, "i:p:c:h")) != -1) {
        switch (c) {
            case 'i':
                ip = strdup(optarg);
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'c':
                max_packets = atoi(optarg);
                break;
            case 'h':
            default:
                usage(argv[0]);
                return 1;
        }
    }
    
    if (!ip || port == 0) {
        usage(argv[0]);
        return 1;
    }
    
    if (setup_input_socket(ip, port) != 0) {
        return 1;
    }
    
    printf("Starting transport stream analysis...\n");
    if (max_packets > 0) {
        printf("Will analyze %d packets then exit\n", max_packets);
    } else {
        printf("Analyzing indefinitely (Ctrl+C to stop)\n");
    }
    
    uint8_t buffer[1500];
    
    while (running && (max_packets == 0 || total_packets < max_packets)) {
        ssize_t bytes = recv(input_fd, buffer, sizeof(buffer), 0);
        
        if (bytes < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            break;
        }
        
        if (bytes == 0) continue;
        
        // Process TS packets in received data
        int offset = 0;
        while (offset + TS_PACKET_SIZE <= bytes) {
            analyze_packet(&buffer[offset]);
            offset += TS_PACKET_SIZE;
        }
        
        if (offset < bytes) {
            printf("WARNING: %zd bytes remaining (not multiple of %d)\n", 
                   bytes - offset, TS_PACKET_SIZE);
        }
        
        // Print progress every 1000 packets
        if (total_packets % 1000 == 0 && total_packets > 0) {
            printf("Analyzed %u packets so far...\n", total_packets);
        }
    }
    
    print_summary();
    
    if (input_fd >= 0) {
        close(input_fd);
    }
    
    if (ip) free(ip);
    
    return 0;
}