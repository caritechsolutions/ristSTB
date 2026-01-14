/* ristsender_marker.c
 * VSF TR-06-4 Part 7 marker-aware sender with circuit breaker protection
 * 
 * Key Features:
 * - Correctly counts marker at BEGINNING of block per VSF TR-06-4 Part 7 spec
 * - Detects lost markers by tracking marker sequence numbers
 * - Uses synchronization state to handle marker loss correctly
 * - Circuit breaker: shuts down on catastrophic failures
 *   - Trigger 1: 10 consecutive marker losses
 *   - Trigger 2: 10 validation failures within 10 seconds
 * - Recovery: requires 10 consecutive good blocks over 10 seconds
 * - Memory efficient: doesn't buffer during recovery state
 * - Modified: Simple recovery message instead of sender recreation
 *
 * Circuit Breaker States:
 * - NORMAL: Normal operation, sending blocks
 * - SHUTDOWN: Catastrophic failure, discarding all packets, waiting for first good marker
 * - RECOVERY: Validating recovery with good blocks, printing restart messages
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <librist/librist.h>
#include <librist/udpsocket.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <getopt.h>
#include <stdbool.h>
#include <signal.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

#define RIST_MARK_UNUSED(unused_param) ((void)(unused_param))
#define RISTSENDER_VERSION "36-SimpleRecovery"
#define MAX_INPUT_COUNT 20
#define MAX_OUTPUT_COUNT 20

// VSF TR-06-4 Part 7 constants
#define TS_PACKET_SIZE 188
#define TS_PACKETS_PER_RTP 7
#define MARKER_PID 0x1FF0
#define MAX_BLOCK_RTP_PAYLOADS 200
#define MAX_BLOCK_TS_PACKETS (MAX_BLOCK_RTP_PAYLOADS * TS_PACKET_SIZE)

#define PACKET_INTERVAL_US 5560ULL
#define STATUS_UPDATE_INTERVAL_US 5000000ULL  // 5 seconds

// Circuit breaker thresholds
#define CONSECUTIVE_MARKER_LOSS_THRESHOLD 10
#define VALIDATION_FAILURE_THRESHOLD 10
#define VALIDATION_FAILURE_WINDOW_US 10000000ULL  // 10 seconds
#define RECOVERY_GOOD_BLOCKS_REQUIRED 10
#define RECOVERY_TIME_REQUIRED_US 10000000ULL     // 10 seconds
#define RESTART_MESSAGE_INTERVAL_US 5000000ULL    // 5 seconds

static int signalReceived = 0;
static int peer_connected_count = 0;
static struct rist_logging_settings logging_settings = LOGGING_SETTINGS_INITIALIZER;

// Circuit breaker states
enum circuit_state {
    STATE_NORMAL,      // Normal operation
    STATE_SHUTDOWN,    // Catastrophic failure, discarding everything
    STATE_RECOVERY     // Validating recovery
};

struct rist_ctx_wrap {
    struct rist_ctx *ctx;
    uintptr_t id;
    bool sender;
};

struct marker_data {
    uint32_t marker_sequence;
    uint16_t non_null_count;
    uint16_t null_count;
    uint16_t rtp_sequence_start;
    uint16_t rtp_sequence_next;
    uint32_t source_ssrc;
    uint32_t crc32;
};

struct rist_peer_args {
    char *token;
    enum rist_profile profile;
    enum rist_log_level loglevel;
    char *shared_secret;
    int encryption_type;
    uint32_t recovery_maxbitrate;
    uint32_t recovery_maxbitrate_return;
    uint32_t recovery_length_min;
    uint32_t recovery_length_max;
    uint32_t recovery_reorder_buffer;
    uint32_t recovery_rtt_min;
    uint32_t recovery_rtt_max;
    uint32_t buffer_size;
    uint32_t statsinterval;
    uint32_t stream_id;
    size_t peer_config_count;
    int compression_type;
};

struct rist_callback_object {
    int sd;
    struct evsocket_ctx *evctx;
    struct rist_ctx_wrap *receiver_ctx;
    struct rist_ctx_wrap *sender_ctx;
    struct rist_udp_config *udp_config;
    uint8_t recv[1500];

    // Part 7 block state
    uint8_t *block_buffer;
    size_t block_buffer_capacity;
    size_t block_ts_count;
    uint16_t block_non_null_count;
    uint16_t block_null_count;

    // Marker tracking
    bool first_marker_seen;
    bool synchronized;
    bool sender_started;
    uint16_t pending_rtp_seq_start;
    uint32_t pending_ssrc;
    uint32_t last_marker_sequence;

    // CBR timing control
    uint64_t next_send_time_us;

    // RTP timestamp synthesis
    uint64_t rtp_base_ntp_time;
    uint16_t rtp_base_sequence;
    bool rtp_timing_initialized;

    // Statistics
    uint64_t markers_processed;
    uint64_t markers_lost;
    uint64_t blocks_validated;
    uint64_t blocks_dropped;
    uint64_t blocks_sent;
    uint64_t rtp_packets_sent;
    uint64_t dropped_time_held_us;
    uint64_t resync_count;
    
    // Periodic status tracking
    uint64_t last_status_time;
    uint64_t markers_last_status;
    uint64_t blocks_sent_last_status;
    uint64_t blocks_dropped_last_status;

    // Circuit breaker state
    enum circuit_state circuit_state;
    uint32_t consecutive_marker_losses;
    uint64_t validation_failure_timestamps[VALIDATION_FAILURE_THRESHOLD];
    size_t validation_failure_index;
    uint32_t circuit_breaker_trips;
    
    // Recovery tracking
    uint32_t recovery_good_blocks;
    uint64_t recovery_start_time;
    uint64_t last_restart_message_time;  // New: for restart message timing
    
    // Saved config for sender recreation (kept for future use)
    uint32_t saved_ssrc;
    char *saved_output_url;
    struct rist_peer_args saved_peer_args;
    bool saved_npd;
};

static struct rist_callback_object callback_object[MAX_INPUT_COUNT];
static struct evsocket_event *event[MAX_INPUT_COUNT];
static bool thread_started[MAX_INPUT_COUNT + 1];
static pthread_t thread_main_loop[MAX_INPUT_COUNT + 1];

// CRC-32 ISO/IEC 13818-1
static uint32_t crc32_table[256];
static int crc32_init = 0;

static void crc32_init_table(void) {
    if (crc32_init) return;
    const uint32_t poly = 0x04C11DB7U;
    for (int i = 0; i < 256; ++i) {
        uint32_t c = (uint32_t)i << 24;
        for (int j = 0; j < 8; ++j)
            c = (c & 0x80000000U) ? ((c << 1) ^ poly) : (c << 1);
        crc32_table[i] = c;
    }
    crc32_init = 1;
}

static uint32_t psi_crc32(const uint8_t *buf, int len) {
    if (!crc32_init) crc32_init_table();
    uint32_t crc = 0xFFFFFFFFU;
    for (int i = 0; i < len; ++i) {
        uint8_t idx = (uint8_t)(((crc >> 24) ^ buf[i]) & 0xFF);
        crc = (crc << 8) ^ crc32_table[idx];
    }
    return crc;
}

static uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int is_marker_packet(const uint8_t *ts_packet) {
    if (ts_packet[0] != 0x47) return 0;
    uint16_t pid = ((ts_packet[1] & 0x1F) << 8) | ts_packet[2];
    return (pid == MARKER_PID);
}

static int is_null_packet(const uint8_t *ts_packet) {
    if (ts_packet[0] != 0x47) return 0;
    uint16_t pid = ((ts_packet[1] & 0x1F) << 8) | ts_packet[2];
    return (pid == 0x1FFF);
}

static int parse_marker_packet(const uint8_t *ts_packet, struct marker_data *marker) {
    if (!ts_packet || !marker) return -1;
    if (ts_packet[0] != 0x47) return -1;
    if ((ts_packet[1] & 0x40) == 0) return -1;

    uint8_t pointer = ts_packet[4];
    if (pointer != 0) {
        if (5 + pointer + 27 > TS_PACKET_SIZE)
            return -1;
    }

    const uint8_t *sec = &ts_packet[5 + pointer];
    if (sec[0] != 0xBF) return -1;

    uint8_t b1 = sec[1];
    uint8_t b2 = sec[2];
    if ((b1 & 0x30) != 0x30) return -1;
    uint16_t section_length = (uint16_t)(((b1 & 0x0F) << 8) | b2);
    if (section_length != 24) return -1;

    uint32_t crc_calc = psi_crc32(sec, 23);
    uint32_t crc_pkt =
        ((uint32_t)sec[23] << 24) |
        ((uint32_t)sec[24] << 16) |
        ((uint32_t)sec[25] <<  8) |
         (uint32_t)sec[26];

    if (crc_calc != crc_pkt) return -1;

    marker->marker_sequence =
        ((uint32_t)sec[3]  << 24) |
        ((uint32_t)sec[4]  << 16) |
        ((uint32_t)sec[5]  <<  8) |
         (uint32_t)sec[6];

    marker->non_null_count = (uint16_t)(((uint16_t)sec[7]  << 8) | sec[8]);
    marker->null_count     = (uint16_t)(((uint16_t)sec[9]  << 8) | sec[10]);
    marker->rtp_sequence_start = (uint16_t)(((uint16_t)sec[13] << 8) | sec[14]);
    marker->rtp_sequence_next  = (uint16_t)(((uint16_t)sec[17] << 8) | sec[18]);

    marker->source_ssrc =
        ((uint32_t)sec[19] << 24) |
        ((uint32_t)sec[20] << 16) |
        ((uint32_t)sec[21] <<  8) |
         (uint32_t)sec[22];

    marker->crc32 = crc_pkt;
    return 0;
}

// Check if we have 10 validation failures within last 10 seconds
static bool check_validation_failure_threshold(struct rist_callback_object *cb) {
    uint64_t now = get_timestamp_us();
    uint64_t threshold_time = now - VALIDATION_FAILURE_WINDOW_US;
    
    int count = 0;
    for (size_t i = 0; i < VALIDATION_FAILURE_THRESHOLD; i++) {
        if (cb->validation_failure_timestamps[i] >= threshold_time) {
            count++;
        }
    }
    
    return (count >= VALIDATION_FAILURE_THRESHOLD);
}

// Record a validation failure
static void record_validation_failure(struct rist_callback_object *cb) {
    cb->validation_failure_timestamps[cb->validation_failure_index] = get_timestamp_us();
    cb->validation_failure_index = (cb->validation_failure_index + 1) % VALIDATION_FAILURE_THRESHOLD;
}

// Trigger circuit breaker shutdown
static void trigger_circuit_breaker(struct rist_callback_object *cb, const char *reason) {
    rist_log(&logging_settings, RIST_LOG_ERROR,
             "\n!!! CIRCUIT BREAKER TRIGGERED !!!\nReason: %s\nShutting down sender, waiting for recovery...\n\n",
             reason);
    
    // Destroy sender context
    if (cb->sender_ctx) {
        rist_destroy(cb->sender_ctx->ctx);
        free(cb->sender_ctx);
        cb->sender_ctx = NULL;
    }
    
    // Reset all state
    cb->sender_started = false;
    cb->first_marker_seen = false;
    cb->synchronized = false;
    cb->block_ts_count = 0;
    cb->block_non_null_count = 0;
    cb->block_null_count = 0;
    cb->consecutive_marker_losses = 0;
    
    // Enter shutdown state
    cb->circuit_state = STATE_SHUTDOWN;
    cb->circuit_breaker_trips++;
    
    // Clear validation failure history
    for (size_t i = 0; i < VALIDATION_FAILURE_THRESHOLD; i++) {
        cb->validation_failure_timestamps[i] = 0;
    }
    cb->validation_failure_index = 0;
}

// Forward declaration
static void connection_status_callback(void *arg, struct rist_peer *peer, enum rist_connection_status st);

// Print restart message instead of recreating sender
static void print_restart_message(struct rist_callback_object *cb) {
    uint64_t now = get_timestamp_us();
    
    // Only print every 5 seconds
    if (cb->last_restart_message_time == 0 || 
        (now - cb->last_restart_message_time) >= RESTART_MESSAGE_INTERVAL_US) {
        
        uint64_t recovery_elapsed = (now - cb->recovery_start_time) / 1000000ULL;
        
        rist_log(&logging_settings, RIST_LOG_INFO,
                 "RESTART SENDER - Recovery complete! %u good blocks over %.1fs (SSRC=0x%08X)\n",
                 cb->recovery_good_blocks, recovery_elapsed / 10.0, cb->saved_ssrc);
        
        cb->last_restart_message_time = now;
    }
}

// Periodic status update
static void print_periodic_status(struct rist_callback_object *cb) {
    uint64_t now = get_timestamp_us();
    
    if (cb->last_status_time == 0) {
        cb->last_status_time = now;
        cb->markers_last_status = cb->markers_processed;
        cb->blocks_sent_last_status = cb->blocks_sent;
        cb->blocks_dropped_last_status = cb->blocks_dropped;
        return;
    }
    
    if ((now - cb->last_status_time) < STATUS_UPDATE_INTERVAL_US) {
        return;
    }
    
    const char *state_str = "UNKNOWN";
    switch (cb->circuit_state) {
        case STATE_NORMAL: state_str = "NORMAL"; break;
        case STATE_SHUTDOWN: state_str = "SHUTDOWN"; break;
        case STATE_RECOVERY: state_str = "RECOVERY"; break;
    }
    
    uint64_t markers_delta = cb->markers_processed - cb->markers_last_status;
    uint64_t sent_delta = cb->blocks_sent - cb->blocks_sent_last_status;
    uint64_t dropped_delta = cb->blocks_dropped - cb->blocks_dropped_last_status;
    uint64_t total_delta = sent_delta + dropped_delta;
    
    double loss_percent = 0.0;
    if (total_delta > 0) {
        loss_percent = (double)dropped_delta / (double)total_delta * 100.0;
    }
    
    rist_log(&logging_settings, RIST_LOG_INFO,
             "[Status] state=%s markers=%llu (lost=%llu) blocks: sent=%llu dropped=%llu (%.1f%%) breaker_trips=%llu\n",
             state_str,
             (unsigned long long)markers_delta,
             (unsigned long long)cb->markers_lost,
             (unsigned long long)sent_delta,
             (unsigned long long)dropped_delta,
             loss_percent,
             (unsigned long long)cb->circuit_breaker_trips);
    
    if (cb->circuit_state == STATE_RECOVERY) {
        uint64_t recovery_elapsed = (now - cb->recovery_start_time) / 1000000ULL;
        rist_log(&logging_settings, RIST_LOG_INFO,
                 "[Recovery] good_blocks=%u/%u elapsed=%llus/10s\n",
                 cb->recovery_good_blocks, RECOVERY_GOOD_BLOCKS_REQUIRED,
                 (unsigned long long)recovery_elapsed);
    }
    
    cb->last_status_time = now;
    cb->markers_last_status = cb->markers_processed;
    cb->blocks_sent_last_status = cb->blocks_sent;
    cb->blocks_dropped_last_status = cb->blocks_dropped;
}

static void send_block_to_rist(struct rist_callback_object *cb) {
    if (!peer_connected_count) {
        return;
    }
    
    size_t total_packets = cb->block_ts_count;
    if (total_packets == 0) return;

    size_t num_rtp_payloads = total_packets / TS_PACKETS_PER_RTP;

    uint16_t current_rtp_seq = cb->pending_rtp_seq_start;

    if (!cb->next_send_time_us) {
        cb->next_send_time_us = get_timestamp_us();
    }

    for (size_t i = 0; i < num_rtp_payloads; i++) {
        uint64_t now = get_timestamp_us();
        uint64_t target = cb->next_send_time_us;
        if (now < target) {
            uint64_t wait_us = target - now;
            if (wait_us < 500000ULL) {
                usleep((useconds_t)wait_us);
            }
        }

        struct rist_data_block db = {0};
        db.payload     = &cb->block_buffer[i * TS_PACKETS_PER_RTP * TS_PACKET_SIZE];
        db.payload_len = TS_PACKETS_PER_RTP * TS_PACKET_SIZE;
        db.seq         = current_rtp_seq;
        db.flags       = RIST_DATA_FLAGS_USE_SEQ;

        uint64_t current_ntp = get_timestamp_us() * ((1ULL << 32) / 1000000ULL);
        uint64_t buffering_delay_ntp = 100000ULL * ((1ULL << 32) / 1000000ULL);
        db.ts_ntp = current_ntp - buffering_delay_ntp;

        db.flow_id     = cb->pending_ssrc;

        int ret = rist_sender_data_write(cb->sender_ctx->ctx, &db);
        if (ret < 0) {
            rist_log(&logging_settings, RIST_LOG_ERROR,
                     "Failed to send RTP payload (seq=%u): %d\n", current_rtp_seq, ret);
        } else {
            cb->rtp_packets_sent++;
        }

        cb->next_send_time_us = target + PACKET_INTERVAL_US;
        current_rtp_seq++;
    }

    cb->blocks_sent++;
}

static void process_marker(struct rist_callback_object *cb, const uint8_t *marker_packet) {
    struct marker_data marker;

    // STEP 1: CRC32 check - must trust the marker data
    if (parse_marker_packet(marker_packet, &marker) != 0) {
        rist_log(&logging_settings, RIST_LOG_WARN, 
                 "Marker parse/CRC failed - ignoring\n");
        return;
    }

    cb->markers_processed++;

    // Handle different circuit breaker states
    if (cb->circuit_state == STATE_SHUTDOWN) {
        // In SHUTDOWN, accept ANY good marker as potential recovery start
        // Don't require exact sequence - we may have missed packets during failure
        rist_log(&logging_settings, RIST_LOG_INFO,
                 "Good marker in SHUTDOWN (seq=%u), entering RECOVERY\n",
                 marker.marker_sequence);
        
        cb->circuit_state = STATE_RECOVERY;
        cb->recovery_good_blocks = 0;
        cb->recovery_start_time = get_timestamp_us();
        cb->last_restart_message_time = 0;  // Reset restart message timer
        cb->synchronized = false;
        cb->first_marker_seen = true;
        cb->last_marker_sequence = marker.marker_sequence;
        cb->saved_ssrc = marker.source_ssrc;
        cb->block_ts_count = 0;
        cb->block_non_null_count = 1;
        cb->block_null_count = 0;
        cb->consecutive_marker_losses = 0;
        return;
    }

    // STEP 2: First marker initialization (NORMAL state only)
    if (!cb->first_marker_seen) {
        cb->first_marker_seen = true;
        cb->synchronized = false;
        cb->pending_ssrc = marker.source_ssrc;
        cb->last_marker_sequence = marker.marker_sequence;
        cb->saved_ssrc = marker.source_ssrc;

        cb->rtp_base_ntp_time = get_timestamp_us() * ((1ULL << 32) / 1000000ULL);
        cb->rtp_base_sequence = marker.rtp_sequence_start;
        cb->rtp_timing_initialized = true;

        rist_log(&logging_settings, RIST_LOG_INFO,
                 "First marker received: SSRC=0x%08X rtp_seq=%u marker_seq=%u (waiting for sync)\n",
                 marker.source_ssrc, marker.rtp_sequence_start, marker.marker_sequence);

        if (!cb->sender_started && cb->circuit_state == STATE_NORMAL) {
            if (rist_sender_flow_id_set(cb->sender_ctx->ctx, marker.source_ssrc) != 0) {
                rist_log(&logging_settings, RIST_LOG_ERROR,
                         "Failed to set sender flow_id to 0x%08X\n", marker.source_ssrc);
            }

            if (rist_start(cb->sender_ctx->ctx) == -1) {
                rist_log(&logging_settings, RIST_LOG_ERROR, "Could not start RIST sender\n");
            } else {
                cb->sender_started = true;
                rist_log(&logging_settings, RIST_LOG_INFO,
                         "RIST sender started with SSRC=0x%08X\n", marker.source_ssrc);
            }
        }

        cb->block_ts_count = 0;
        cb->block_non_null_count = 1;
        cb->block_null_count = 0;
        return;
    }

    // STEP 3: Check marker sequence
    uint32_t expected_marker_seq = cb->last_marker_sequence + 1;
    bool sequence_good = (marker.marker_sequence == expected_marker_seq);
    
    if (!sequence_good) {
        uint32_t lost_markers = marker.marker_sequence - expected_marker_seq;
        
        rist_log(&logging_settings, RIST_LOG_WARN,
                 "Marker sequence discontinuity! Expected %u, got %u (%u lost)\n",
                 expected_marker_seq, marker.marker_sequence, lost_markers);
        
        cb->markers_lost += lost_markers;
        cb->consecutive_marker_losses += lost_markers;
        
        // Check for circuit breaker trigger
        if (cb->consecutive_marker_losses >= CONSECUTIVE_MARKER_LOSS_THRESHOLD) {
            char reason[256];
            snprintf(reason, sizeof(reason), 
                     "%u consecutive marker losses", cb->consecutive_marker_losses);
            trigger_circuit_breaker(cb, reason);
            return;
        }
        
        if (cb->circuit_state == STATE_RECOVERY) {
            // Recovery failed, back to shutdown
            rist_log(&logging_settings, RIST_LOG_WARN,
                     "Marker loss during RECOVERY, back to SHUTDOWN\n");
            cb->circuit_state = STATE_SHUTDOWN;
            cb->first_marker_seen = false;
            cb->synchronized = false;
            return;
        }
        
        // Not circuit breaker, just resync
        cb->blocks_dropped += lost_markers;
        cb->resync_count++;
        cb->synchronized = false;
        cb->last_marker_sequence = marker.marker_sequence;
        
        if (cb->block_ts_count > 0) {
            rist_log(&logging_settings, RIST_LOG_INFO,
                     "Discarding %zu buffered packets for resync\n", cb->block_ts_count);
        }
        
        cb->block_ts_count = 0;
        cb->block_non_null_count = 1;
        cb->block_null_count = 0;
        
        return;
    }
    
    // Sequence is good
    cb->last_marker_sequence = marker.marker_sequence;
    cb->consecutive_marker_losses = 0;

    // STEP 4: Validate block (if synchronized)
    bool validation_succeeded = false;
    
    if (cb->synchronized && cb->block_ts_count > 0) {
        size_t expected_total = (size_t)marker.non_null_count + (size_t)marker.null_count;
        
        bool counts_match = (cb->block_non_null_count == marker.non_null_count) &&
                            (cb->block_null_count == marker.null_count);
        bool aligned = (cb->block_ts_count % TS_PACKETS_PER_RTP == 0);
        bool buffer_match = ((cb->block_ts_count + 1) == expected_total);
        
        if (counts_match && aligned && buffer_match) {
            validation_succeeded = true;
            
            // Only send in NORMAL state
            if (cb->circuit_state == STATE_NORMAL) {
                cb->pending_rtp_seq_start = marker.rtp_sequence_start;
                cb->pending_ssrc = marker.source_ssrc;
                send_block_to_rist(cb);
            }
            
            cb->blocks_validated++;
            
            // Handle recovery state - just print message instead of recreating sender
            if (cb->circuit_state == STATE_RECOVERY) {
                cb->recovery_good_blocks++;
                uint64_t elapsed = get_timestamp_us() - cb->recovery_start_time;
                
                if (cb->recovery_good_blocks >= RECOVERY_GOOD_BLOCKS_REQUIRED && 
                    elapsed >= RECOVERY_TIME_REQUIRED_US) {
                    
                    // Print restart message every 5 seconds instead of recreating sender
                    print_restart_message(cb);
                    
                    // Stay in recovery state and keep printing messages
                    // Don't transition back to NORMAL state
                }
            }
        } else {
            // Validation failed
            rist_log(&logging_settings, RIST_LOG_WARN,
                     "Block #%u validation failed: buffered=%zu non_null=%u null=%u expected: non_null=%u null=%u total=%zu\n",
                     marker.marker_sequence, cb->block_ts_count, 
                     cb->block_non_null_count, cb->block_null_count,
                     marker.non_null_count, marker.null_count, expected_total);
            
            record_validation_failure(cb);
            cb->blocks_dropped++;
            
            // Check for circuit breaker trigger
            if (check_validation_failure_threshold(cb)) {
                trigger_circuit_breaker(cb, "10 validation failures within 10 seconds");
                return;
            }
            
            // Check if in recovery
            if (cb->circuit_state == STATE_RECOVERY) {
                rist_log(&logging_settings, RIST_LOG_WARN,
                         "Validation failure during RECOVERY, back to SHUTDOWN\n");
                cb->circuit_state = STATE_SHUTDOWN;
                cb->first_marker_seen = false;
                cb->synchronized = false;
                return;
            }
        }
    } else if (!cb->synchronized) {
        // Second marker after resync/first marker
        rist_log(&logging_settings, RIST_LOG_INFO,
                 "Synchronized! Marker %u received, will validate next block\n",
                 marker.marker_sequence);
        cb->synchronized = true;
    }

    // ALWAYS reset for next block
    cb->block_ts_count = 0;
    cb->block_non_null_count = 1;
    cb->block_null_count = 0;
}

static void buffer_ts_packet(struct rist_callback_object *cb, const uint8_t *ts_packet) {
    // In RECOVERY state, don't buffer - just count to save memory
    if (cb->circuit_state == STATE_RECOVERY) {
        cb->block_ts_count++;
        if (is_null_packet(ts_packet)) cb->block_null_count++;
        else cb->block_non_null_count++;
        return;
    }
    
    // In NORMAL state, buffer packets for sending
    if (cb->block_ts_count >= MAX_BLOCK_TS_PACKETS) {
        rist_log(&logging_settings, RIST_LOG_ERROR,
                 "Block buffer overflow! count=%zu\n", cb->block_ts_count);
        return;
    }

    memcpy(&cb->block_buffer[cb->block_ts_count * TS_PACKET_SIZE], ts_packet, TS_PACKET_SIZE);
    cb->block_ts_count++;

    if (is_null_packet(ts_packet)) cb->block_null_count++;
    else cb->block_non_null_count++;
}

static void input_udp_recv(struct evsocket_ctx *evctx, int fd, short revents, void *arg)
{
    struct rist_callback_object *cb = (void *) arg;
    RIST_MARK_UNUSED(evctx);
    RIST_MARK_UNUSED(revents);
    RIST_MARK_UNUSED(fd);

    ssize_t recv_bufsize = -1;
    struct sockaddr_in addr4 = {0};
    struct sockaddr_in6 addr6 = {0};
    uint8_t *recv_buf = cb->recv;
    socklen_t addrlen = 0;

    uint16_t address_family = (uint16_t)cb->udp_config->address_family;
    if (address_family == AF_INET6) {
        addrlen = sizeof(struct sockaddr_in6);
        recv_bufsize = udpsocket_recvfrom(cb->sd, recv_buf, sizeof(cb->recv), MSG_DONTWAIT, (struct sockaddr *) &addr6, &addrlen);
    } else {
        addrlen = sizeof(struct sockaddr_in);
        recv_bufsize = udpsocket_recvfrom(cb->sd, recv_buf, sizeof(cb->recv), MSG_DONTWAIT, (struct sockaddr *) &addr4, &addrlen);
    }

    if (recv_bufsize > 0) {
        int offset = 0;

        while (offset + TS_PACKET_SIZE <= recv_bufsize) {
            uint8_t *ts_packet = &recv_buf[offset];

            if (ts_packet[0] != 0x47) {
                static int sync_error_count = 0;
                if (sync_error_count++ < 1) {
                    rist_log(&logging_settings, RIST_LOG_ERROR,
                             "Invalid TS sync byte: 0x%02X (further sync errors suppressed)\n", 
                             ts_packet[0]);
                }
                offset += TS_PACKET_SIZE;
                continue;
            }

            if (is_marker_packet(ts_packet)) {
                process_marker(cb, ts_packet);
            } else {
                // In SHUTDOWN, discard non-marker packets
                if (cb->circuit_state == STATE_SHUTDOWN) {
                    offset += TS_PACKET_SIZE;
                    continue;
                }
                
                if (cb->first_marker_seen) {
                    buffer_ts_packet(cb, ts_packet);
                }
            }

            offset += TS_PACKET_SIZE;
        }
    } else {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            rist_log(&logging_settings, RIST_LOG_ERROR,
                     "Input receive failed: errno=%d\n", errno);
        }
    }
    
    print_periodic_status(cb);
}

static void input_udp_sockerr(struct evsocket_ctx *evctx, int fd, short revents, void *arg)
{
    struct rist_callback_object *cb = (void *) arg;
    RIST_MARK_UNUSED(evctx);
    RIST_MARK_UNUSED(revents);
    RIST_MARK_UNUSED(fd);
    rist_log(&logging_settings, RIST_LOG_ERROR, "Socket error on sd=%d\n", cb->sd);
}

static char help_str[] = "librist, VSF TR-06-4 Part 7 RIST Sender (marker stripper with circuit breaker)";

static void usage(char *cmd)
{
    fprintf(stderr, "%s\n%s version %s\nlibRIST: %s API: %s\n",
             cmd, help_str, RISTSENDER_VERSION, librist_version(), librist_api_version());
    exit(1);
}

static void connection_status_callback(void *arg, struct rist_peer *peer, enum rist_connection_status st)
{
    (void)arg;
    (void)peer;
    if (st == RIST_CONNECTION_ESTABLISHED || st == RIST_CLIENT_CONNECTED) {
        peer_connected_count++;
        rist_log(&logging_settings, RIST_LOG_INFO,
                 "Peer connected (total: %d)\n", peer_connected_count);
    } else {
        peer_connected_count--;
        rist_log(&logging_settings, RIST_LOG_WARN,
                 "Peer disconnected (total: %d)\n", peer_connected_count);
    }
}

static int cb_auth_connect(void *arg, const char* connecting_ip, uint16_t connecting_port, const char* local_ip, uint16_t local_port, struct rist_peer *peer)
{
    struct rist_ctx_wrap *w = (struct rist_ctx_wrap *)arg;
    (void)w; (void)peer; (void)local_ip; (void)local_port;
    rist_log(&logging_settings, RIST_LOG_INFO,"Peer auth: %s:%d\n", connecting_ip, connecting_port);
    return 0;
}

static int cb_auth_disconnect(void *arg, struct rist_peer *peer)
{
    struct rist_ctx_wrap *w = (struct rist_ctx_wrap *)arg;
    (void)w; (void)peer;
    return 0;
}

static void intHandler(int signal) {
    if (signal == SIGINT) {
        rist_log(&logging_settings, RIST_LOG_INFO, "Shutting down (signal %d)\n", signal);
    }
    signalReceived = signal;
}

static struct rist_peer *setup_rist_peer(struct rist_ctx_wrap *ctx_wrap, struct rist_peer_args *peer_args)
{
    struct rist_peer_config *peer_config_link = NULL;
    if (rist_parse_address2(peer_args->token, &peer_config_link))
    {
        rist_log(&logging_settings, RIST_LOG_ERROR, "Could not parse address %s\n", peer_args->token);
        goto cleanup;
    }

    if (peer_args->profile != RIST_PROFILE_SIMPLE && peer_config_link)
    {
        if (peer_args->shared_secret && strlen(peer_args->shared_secret) > 0)
        {
            peer_config_link->secret[0] = 0;
            strncpy(peer_config_link->secret, peer_args->shared_secret, RIST_MAX_STRING_SHORT -1);
            if (peer_args->encryption_type)
                peer_config_link->key_size = peer_args->encryption_type;
            else
                peer_config_link->key_size = 128;
        }
        peer_config_link->recovery_mode = RIST_RECOVERY_MODE_TIME;
    }

    struct rist_peer *peer;
    if (rist_peer_create(ctx_wrap->ctx, &peer, peer_config_link) == -1) {
        rist_log(&logging_settings, RIST_LOG_ERROR, "Could not add peer %s\n", peer_args->token);
        goto cleanup;
    }

    rist_peer_config_free2(&peer_config_link);
    return peer;

cleanup:
    rist_peer_config_free2(&peer_config_link);
    return NULL;
}

static void *input_loop(void *arg)
{
    struct rist_callback_object *cb = (void *) arg;

    while (!signalReceived) {
        if (cb->receiver_ctx)
        {
            struct rist_data_block *b = NULL;
            int queue_size = rist_receiver_data_read2(cb->receiver_ctx->ctx, &b, 5);
            if (queue_size > 0) {
                if (queue_size % 10 == 0 || queue_size > 50)
                    rist_log(&logging_settings, RIST_LOG_WARN, "Falling behind: queue=%d\n", queue_size);
                if (b && b->payload) {
                    if (peer_connected_count) {
                        int w = rist_sender_data_write(cb->sender_ctx->ctx, b);
                        (void) w;
                    }
                    rist_receiver_data_block_free2(&b);
                }
            }
        }
        else
        {
            evsocket_loop_single(cb->evctx, 5, 100);
        }
    }
    return 0;
}

static struct rist_ctx_wrap *configure_rist_output_context(char* outputurl,
    struct rist_peer_args *peer_args, const struct rist_udp_config *udp_config,
    bool npd, enum rist_profile profile)
{
    (void)udp_config; (void)profile;

    struct rist_ctx *sender_ctx;
    if (rist_sender_create(&sender_ctx, peer_args->profile, 0, &logging_settings) != 0) {
        rist_log(&logging_settings, RIST_LOG_ERROR, "Could not create sender context\n");
        return NULL;
    }
    struct rist_ctx_wrap *w = malloc(sizeof(*w));
    memset(w, 0, sizeof(*w));
    w->ctx = sender_ctx;
    w->sender = true;

    if (rist_auth_handler_set(sender_ctx, cb_auth_connect, cb_auth_disconnect, w) != 0) {
        rist_log(&logging_settings, RIST_LOG_ERROR, "Could not init auth handler\n");
        goto fail;
    }

    if (rist_connection_status_callback_set(sender_ctx, connection_status_callback, w) == -1) {
        rist_log(&logging_settings, RIST_LOG_ERROR, "Could not init connection callback\n");
        goto fail;
    }

    if (npd) {
        rist_log(&logging_settings, RIST_LOG_INFO, "NPD enabled\n");
        if (rist_sender_npd_enable(sender_ctx) != 0) {
            rist_log(&logging_settings, RIST_LOG_ERROR, "Failed to enable NPD\n");
        }
    }

    char *saveptroutput;
    char *tmpoutputurl = malloc(strlen(outputurl) + 1);
    strcpy(tmpoutputurl, outputurl);
    char *outputtoken = strtok_r(tmpoutputurl, ",", &saveptroutput);

    while (outputtoken) {
        peer_args->token = outputtoken;
        peer_args->stream_id = udp_config ? udp_config->stream_id : 0;
        struct rist_peer *peer = setup_rist_peer(w, peer_args);
        if (peer == NULL) {
            free(tmpoutputurl);
            goto fail;
        }
        outputtoken = strtok_r(NULL, ",", &saveptroutput);
    }
    free(tmpoutputurl);

    return w;

fail:
    rist_destroy(sender_ctx);
    free(w);
    return NULL;
}

static void print_statistics(struct rist_callback_object *cb) {
    double loss_percent = 0.0;
    if (cb->blocks_validated + cb->blocks_dropped > 0) {
        loss_percent = (double)cb->blocks_dropped / 
                      (double)(cb->blocks_validated + cb->blocks_dropped) * 100.0;
    }
    
    const char *state_str = "UNKNOWN";
    switch (cb->circuit_state) {
        case STATE_NORMAL: state_str = "NORMAL"; break;
        case STATE_SHUTDOWN: state_str = "SHUTDOWN"; break;
        case STATE_RECOVERY: state_str = "RECOVERY"; break;
    }
    
    rist_log(&logging_settings, RIST_LOG_INFO,
             "\n=== Final Statistics ===\n");
    rist_log(&logging_settings, RIST_LOG_INFO, 
             "State: %s | Circuit breaker trips: %llu\n",
             state_str, (unsigned long long)cb->circuit_breaker_trips);
    rist_log(&logging_settings, RIST_LOG_INFO, 
             "Markers: %llu (lost=%llu resyncs=%llu) | Blocks: sent=%llu dropped=%llu (%.2f%%) | RTP: %llu\n",
             (unsigned long long)cb->markers_processed,
             (unsigned long long)cb->markers_lost,
             (unsigned long long)cb->resync_count,
             (unsigned long long)cb->blocks_sent,
             (unsigned long long)cb->blocks_dropped,
             loss_percent,
             (unsigned long long)cb->rtp_packets_sent);
}

int main(int argc, char *argv[])
{
    int c;
    char *inputurl = NULL;
    char *outputurl = NULL;
    enum rist_profile profile = RIST_PROFILE_MAIN;
    char *shared_secret = NULL;
    int encryption_type = 0;
    int statsinterval = 0;
    int verbosity = RIST_LOG_INFO;
    int buffer_size = 0;
    bool npd = false;

    struct rist_peer_args peer_args;
    memset(&peer_args, 0, sizeof(peer_args));
    peer_args.recovery_length_min = 1000;
    peer_args.recovery_length_max = 1000;
    peer_args.recovery_reorder_buffer = 25;
    peer_args.recovery_rtt_min = 50;
    peer_args.recovery_rtt_max = 500;

    for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
        memset(&callback_object[i], 0, sizeof(callback_object[i]));
        callback_object[i].circuit_state = STATE_NORMAL;
        event[i] = NULL;
        thread_started[i] = false;
    }
    thread_started[MAX_INPUT_COUNT] = false;

    struct option long_options[] = {
        { "help",            no_argument,        NULL, 'h' },
        { "url",             required_argument,  NULL, 'u' },
        { "input-url",       required_argument,  NULL, 'i' },
        { "profile",         required_argument,  NULL, 'p' },
        { "shared-secret",   required_argument,  NULL, 's' },
        { "encryption-type", required_argument,  NULL, 'e' },
        { "stats",           required_argument,  NULL, 'S' },
        { "verbose-level",   required_argument,  NULL, 'v' },
        { "buffer",          required_argument,  NULL, 'b' },
        { "npd",             no_argument,        NULL, 'n' },
        { 0, 0, 0, 0 },
    };

    while ((c = getopt_long(argc, argv, "hi:u:p:s:e:S:v:b:n", long_options, NULL)) != -1) {
        switch (c) {
        case 'i': inputurl = strdup(optarg); break;
        case 'u': outputurl = strdup(optarg); break;
        case 'p': profile = (enum rist_profile)atoi(optarg); break;
        case 's': shared_secret = strdup(optarg); break;
        case 'e': encryption_type = atoi(optarg); break;
        case 'S': statsinterval = atoi(optarg); break;
        case 'v': verbosity = atoi(optarg); break;
        case 'b': buffer_size = atoi(optarg); break;
        case 'n': npd = true; break;
        case 'h':
        default: usage(argv[0]);
        }
    }

    if (!inputurl || !outputurl) {
        usage(argv[0]);
    }

    struct rist_logging_settings *log_ptr = &logging_settings;
    if (rist_logging_set(&log_ptr, verbosity, NULL, NULL, NULL, stderr) != 0) {
        fprintf(stderr, "Failed to setup logging!\n");
        exit(1);
    }

    rist_log(&logging_settings, RIST_LOG_INFO,
             "VSF TR-06-4 Part 7 Sender - %s\n", RISTSENDER_VERSION);
    rist_log(&logging_settings, RIST_LOG_INFO,
             "libRIST: %s API: %s | Profile: %d\n", 
             librist_version(), librist_api_version(), profile);

    signal(SIGINT,  intHandler);
    signal(SIGTERM, intHandler);
    signal(SIGPIPE, intHandler);

    peer_args.loglevel = verbosity;
    peer_args.profile = profile;
    peer_args.encryption_type = encryption_type;
    peer_args.shared_secret = shared_secret;
    peer_args.buffer_size = buffer_size;
    peer_args.statsinterval = statsinterval;

    bool rist_listens = false;
    if (strstr(outputurl, "://@") != NULL) {
        rist_listens = true;
    }

    int32_t stream_id_check[MAX_INPUT_COUNT];
    for (size_t j = 0; j < MAX_INPUT_COUNT; j++)
        stream_id_check[j] = -1;
    struct evsocket_ctx *evctx = NULL;
    bool atleast_one_socket_opened = false;
    char *saveptrinput;
    char *inputtoken = strtok_r(inputurl, ",", &saveptrinput);
    struct rist_udp_config *udp_config = NULL;

    for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
        if (!inputtoken)
            break;

        if (rist_parse_udp_address2(inputtoken, &udp_config)) {
            rist_log(&logging_settings, RIST_LOG_ERROR, "Could not parse input %s\n", inputtoken);
            goto next;
        }

        bool found_empty = false;
        for (size_t j = 0; j < MAX_INPUT_COUNT; j++) {
            if (stream_id_check[j] == -1 && !found_empty) {
                stream_id_check[j] = (int32_t)udp_config->stream_id;
                found_empty = true;
            } else if ((uint16_t)stream_id_check[j] == udp_config->stream_id) {
                rist_log(&logging_settings, RIST_LOG_ERROR, "Duplicate stream-id %d\n", udp_config->stream_id);
                goto shutdown;
            }
        }

        if (rist_listens && i > 0) {
            callback_object[i].sender_ctx = callback_object[0].sender_ctx;
        } else {
            callback_object[i].sender_ctx = configure_rist_output_context(outputurl, &peer_args, udp_config, npd, profile);
            if (callback_object[i].sender_ctx == NULL)
                goto shutdown;
                
            // Save configuration for potential future use
            callback_object[i].saved_output_url = strdup(outputurl);
            memcpy(&callback_object[i].saved_peer_args, &peer_args, sizeof(peer_args));
            if (peer_args.shared_secret) {
                callback_object[i].saved_peer_args.shared_secret = strdup(peer_args.shared_secret);
            }
            callback_object[i].saved_npd = npd;
        }

        if (strcmp(udp_config->prefix, "rist") == 0) {
            struct rist_ctx_wrap *w = calloc(1, sizeof(*w));
            w->id = 0;
            callback_object[i].receiver_ctx = w;
            if (rist_receiver_create(&callback_object[i].receiver_ctx->ctx, peer_args.profile, &logging_settings) != 0) {
                rist_log(&logging_settings, RIST_LOG_ERROR, "Could not create receiver\n");
                goto next;
            }
            peer_args.token = inputtoken;
            struct rist_peer *peer = setup_rist_peer(callback_object[i].receiver_ctx, &peer_args);
            if (peer == NULL)
                atleast_one_socket_opened = true;
            rist_udp_config_free2(&udp_config);
            udp_config = NULL;
        } else {
            callback_object[i].block_buffer_capacity = MAX_BLOCK_TS_PACKETS * TS_PACKET_SIZE;
            callback_object[i].block_buffer = (uint8_t *)malloc(callback_object[i].block_buffer_capacity);
            if (!callback_object[i].block_buffer) {
                rist_log(&logging_settings, RIST_LOG_ERROR, "Failed to allocate buffer\n");
                goto shutdown;
            }

            if (!evctx)
                evctx = evsocket_create();

            char hostname[200] = {0};
            int inputlisten;
            uint16_t inputport;
            if (udpsocket_parse_url((void *)udp_config->address, hostname, 200, &inputport, &inputlisten) || !inputport || strlen(hostname) == 0) {
                rist_log(&logging_settings, RIST_LOG_ERROR, "Could not parse %s\n", inputtoken);
                goto next;
            }

            callback_object[i].sd = udpsocket_open_bind(hostname, inputport, udp_config->miface);
            if (callback_object[i].sd < 0) {
                rist_log(&logging_settings, RIST_LOG_ERROR, "Could not bind %s:%d\n", hostname, inputport);
                goto next;
            } else {
                udpsocket_set_nonblocking(callback_object[i].sd);
                rist_log(&logging_settings, RIST_LOG_INFO, "Input bound: %s:%d\n", hostname, inputport);
                atleast_one_socket_opened = true;
            }
            callback_object[i].udp_config = udp_config;
            udp_config = NULL;
            callback_object[i].evctx = evctx;
            event[i] = evsocket_addevent(callback_object[i].evctx, callback_object[i].sd, EVSOCKET_EV_READ,
                                         input_udp_recv, input_udp_sockerr, (void *)&callback_object[i]);
        }

next:
        inputtoken = strtok_r(NULL, ",", &saveptrinput);
    }

    if (!atleast_one_socket_opened) {
        goto shutdown;
    }

    if (evctx && pthread_create(&thread_main_loop[0], NULL, input_loop, (void *)callback_object) != 0)
    {
        rist_log(&logging_settings, RIST_LOG_ERROR, "Could not start thread\n");
        goto shutdown;
    }
    thread_started[0] = true;

    for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
        if (callback_object[i].receiver_ctx && rist_start(callback_object[i].receiver_ctx->ctx) == -1) {
            rist_log(&logging_settings, RIST_LOG_ERROR, "Could not start receiver\n");
            goto shutdown;
        }
        if (callback_object[i].receiver_ctx && pthread_create(&thread_main_loop[i+1], NULL, input_loop, (void *)&callback_object[i]) != 0)
        {
            rist_log(&logging_settings, RIST_LOG_ERROR, "Could not start thread\n");
            goto shutdown;
        } else if (callback_object[i].receiver_ctx) {
            thread_started[i+1] = true;
        }
    }

    rist_log(&logging_settings, RIST_LOG_INFO, "Sender started. Waiting for first marker...\n");

#ifndef _WIN32
    pause();
#endif

shutdown:
    for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
        if (callback_object[i].markers_processed > 0) {
            print_statistics(&callback_object[i]);
        }
    }

    if (udp_config) {
        rist_udp_config_free2(&udp_config);
    }
    for (size_t i = 0; i < MAX_INPUT_COUNT; i++) {
        if (event[i])
            evsocket_delevent(callback_object[i].evctx, event[i]);
        if (callback_object[i].block_buffer)
            free(callback_object[i].block_buffer);
        if ((void *)callback_object[i].udp_config)
            rist_udp_config_free2(&callback_object[i].udp_config);
        if (callback_object[i].receiver_ctx) {
            rist_destroy(callback_object[i].receiver_ctx->ctx);
            free(callback_object[i].receiver_ctx);
        }
        if (callback_object[i].sender_ctx) {
            rist_destroy(callback_object[i].sender_ctx->ctx);
            free(callback_object[i].sender_ctx);
        }
        if (callback_object[i].saved_output_url) {
            free(callback_object[i].saved_output_url);
        }
        if (callback_object[i].saved_peer_args.shared_secret) {
            free(callback_object[i].saved_peer_args.shared_secret);
        }
    }

    for (size_t i = 0; i <= MAX_INPUT_COUNT; i++) {
        if (thread_started[i])
            pthread_join(thread_main_loop[i], NULL);
    }

    rist_logging_unset_global();
    free(inputurl);
    free(outputurl);
    if (shared_secret) free(shared_secret);

    return 0;
}