/* librist. Copyright © 2024 SipRadius LLC. All right reserved.
 * Author: Richard Rawlins
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "program-selection.h"
#include "rist-private.h"
#include "log-private.h"
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

/* Global program selection state */
static struct peer_program_selection *program_selections = NULL;
static pthread_mutex_t program_selection_lock = PTHREAD_MUTEX_INITIALIZER;
static bool program_selection_initialized = false;

/* Internal helper functions */
static void free_peer_program_selection(struct peer_program_selection *selection);
static uint16_t* parse_pid_array(cJSON *json_array);
static uint16_t* parse_program_array(cJSON *json_array);
static struct peer_program_selection* find_peer_selection_locked(uint32_t peer_id);
static bool should_include_pid(uint16_t pid, const struct peer_program_selection *selection);

int program_selection_init(void)
{
    pthread_mutex_lock(&program_selection_lock);
    
    if (program_selection_initialized) {
        pthread_mutex_unlock(&program_selection_lock);
        return 0;  /* Already initialized */
    }
    
    program_selections = NULL;
    program_selection_initialized = true;
    
    pthread_mutex_unlock(&program_selection_lock);
    return 0;
}

int program_selection_add_peer(uint32_t peer_id, const char *json_content)
{
    if (!json_content || !program_selection_initialized) {
        return -1;
    }
    
    /* Parse JSON content */
    cJSON *json = cJSON_Parse(json_content);
    if (!json) {
        return -1;
    }
    
    cJSON *content_selection = cJSON_GetObjectItem(json, "contentSelection");
    if (!content_selection || !cJSON_IsArray(content_selection)) {
        cJSON_Delete(json);
        return -1;
    }
    
    pthread_mutex_lock(&program_selection_lock);
    
    /* Find existing entry or create new one */
    struct peer_program_selection *selection = find_peer_selection_locked(peer_id);
    if (selection) {
        /* Update existing entry - free old arrays */
        free(selection->requested_programs);
        free(selection->blocked_programs);
        free(selection->requested_pids);
        free(selection->blocked_pids);
        free(selection->destination_ip);
        free(selection->source_ip);
    } else {
        /* Create new entry */
        selection = calloc(1, sizeof(struct peer_program_selection));
        if (!selection) {
            pthread_mutex_unlock(&program_selection_lock);
            cJSON_Delete(json);
            return -1;
        }
        selection->peer_id = peer_id;
        
        /* Add to head of list */
        selection->next = program_selections;
        program_selections = selection;
    }
    
    /* Initialize selection */
    selection->has_selection = true;
    selection->requested_programs = NULL;
    selection->blocked_programs = NULL;
    selection->requested_pids = NULL;
    selection->blocked_pids = NULL;
    selection->destination_ip = NULL;
    selection->source_ip = NULL;
    selection->udp_port = 0;
    
    /* Process first contentSelection array element (for now, support single selection) */
    cJSON *first_selection = cJSON_GetArrayItem(content_selection, 0);
    if (first_selection && cJSON_IsObject(first_selection)) {
        /* Parse UDP port (required) */
        cJSON *udp_port = cJSON_GetObjectItem(first_selection, "UDPPort");
        if (udp_port && cJSON_IsNumber(udp_port)) {
            selection->udp_port = (uint16_t)cJSON_GetNumberValue(udp_port);
        }
        
        /* Parse destination IP (optional) */
        cJSON *dest_ip = cJSON_GetObjectItem(first_selection, "DestinationIP");
        if (dest_ip && cJSON_IsString(dest_ip)) {
            const char *ip_str = cJSON_GetStringValue(dest_ip);
            if (ip_str) {
                selection->destination_ip = strdup(ip_str);
            }
        }
        
        /* Parse source IP (optional) */
        cJSON *src_ip = cJSON_GetObjectItem(first_selection, "SourceIP");
        if (src_ip && cJSON_IsString(src_ip)) {
            const char *ip_str = cJSON_GetStringValue(src_ip);
            if (ip_str) {
                selection->source_ip = strdup(ip_str);
            }
        }
        
        /* Parse requested programs */
        cJSON *req_programs = cJSON_GetObjectItem(first_selection, "requestedPrograms");
        if (req_programs && cJSON_IsArray(req_programs)) {
            selection->requested_programs = parse_program_array(req_programs);
        }
        
        /* Parse blocked programs */
        cJSON *blocked_programs = cJSON_GetObjectItem(first_selection, "blockedPrograms");
        if (blocked_programs && cJSON_IsArray(blocked_programs)) {
            selection->blocked_programs = parse_program_array(blocked_programs);
        }
        
        /* Parse requested PIDs */
        cJSON *req_pids = cJSON_GetObjectItem(first_selection, "requestedPIDs");
        if (req_pids && cJSON_IsArray(req_pids)) {
            selection->requested_pids = parse_pid_array(req_pids);
        }
        
        /* Parse blocked PIDs */
        cJSON *blocked_pids = cJSON_GetObjectItem(first_selection, "blockedPIDs");
        if (blocked_pids && cJSON_IsArray(blocked_pids)) {
            selection->blocked_pids = parse_pid_array(blocked_pids);
        }
    }
    
    pthread_mutex_unlock(&program_selection_lock);
    cJSON_Delete(json);
    
    return 0;
}

int program_selection_remove_peer(uint32_t peer_id)
{
    if (!program_selection_initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&program_selection_lock);
    
    struct peer_program_selection *current = program_selections;
    struct peer_program_selection *previous = NULL;
    
    while (current) {
        if (current->peer_id == peer_id) {
            /* Remove from list */
            if (previous) {
                previous->next = current->next;
            } else {
                program_selections = current->next;
            }
            
            /* Free memory */
            free_peer_program_selection(current);
            
            pthread_mutex_unlock(&program_selection_lock);
            return 0;
        }
        previous = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&program_selection_lock);
    return -1;  /* Not found */
}

struct peer_program_selection* program_selection_get_peer(uint32_t peer_id)
{
    if (!program_selection_initialized) {
        return NULL;
    }
    
    pthread_mutex_lock(&program_selection_lock);
    struct peer_program_selection *selection = find_peer_selection_locked(peer_id);
    pthread_mutex_unlock(&program_selection_lock);
    
    return selection;
}

bool program_selection_peer_has_selection(uint32_t peer_id)
{
    struct peer_program_selection *selection = program_selection_get_peer(peer_id);
    return (selection && selection->has_selection);
}

int program_selection_get_peer_count(void)
{
    if (!program_selection_initialized) {
        return 0;
    }
    
    int count = 0;
    pthread_mutex_lock(&program_selection_lock);
    
    struct peer_program_selection *current = program_selections;
    while (current) {
        if (current->has_selection) {
            count++;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&program_selection_lock);
    return count;
}

void program_selection_cleanup(void)
{
    if (!program_selection_initialized) {
        return;
    }
    
    pthread_mutex_lock(&program_selection_lock);
    
    struct peer_program_selection *current = program_selections;
    while (current) {
        struct peer_program_selection *next = current->next;
        free_peer_program_selection(current);
        current = next;
    }
    
    program_selections = NULL;
    program_selection_initialized = false;
    
    pthread_mutex_unlock(&program_selection_lock);
}

void program_selection_debug_dump(void)
{
    if (!program_selection_initialized) {
        return;
    }
    
    pthread_mutex_lock(&program_selection_lock);
    
    struct peer_program_selection *current = program_selections;
    int count = 0;
    
    while (current) {
        if (current->has_selection) {
            count++;
            /* Note: Using printf since we don't have rist_log context here */
            printf("Program Selection Peer %u: port=%u, has_selection=%s\n", 
                   current->peer_id, current->udp_port, 
                   current->has_selection ? "true" : "false");
        }
        current = current->next;
    }
    
    printf("Total peers with program selection: %d\n", count);
    
    pthread_mutex_unlock(&program_selection_lock);
}

/**
 * @brief Check if a PID should be included based on program selection rules
 * 
 * Implements VSF TR-06-4 Part 6 filtering rules:
 * - Always include PAT (PID 0), CAT (PID 1), and PMT PIDs
 * - Apply requested/blocked program and PID rules
 * 
 * @param pid The PID to check (0-8191)
 * @param selection Program selection configuration
 * @return true if PID should be included, false if should be filtered out
 */
static bool should_include_pid(uint16_t pid, const struct peer_program_selection *selection)
{
    if (!selection || !selection->has_selection) {
        return true;  // No filtering - include everything
    }
    
    // Always include critical PIDs per spec (take priority over blockedPIDs)
    if (pid == 0 || pid == 1) {  // PAT, CAT
        return true;
    }
    
    // TODO: Also always include PMT PIDs (requires PAT parsing)
    // For now, we'll implement basic PID filtering
    
    // Check if PID is explicitly requested
    if (selection->requested_pids) {
        for (int i = 0; selection->requested_pids[i] != 0; i++) {
            if (selection->requested_pids[i] == pid) {
                return true;
            }
        }
    }
    
    // Check if PID is explicitly blocked
    if (selection->blocked_pids) {
        for (int i = 0; selection->blocked_pids[i] != 0; i++) {
            if (selection->blocked_pids[i] == pid) {
                return false;  // Explicitly blocked
            }
        }
    }
    
    // If no specific PID rules, check program rules
    // TODO: Implement program-based filtering (requires PAT/PMT parsing)
    
    // Default behavior based on what's specified
    if (selection->requested_programs || selection->requested_pids) {
        // If specific requests made, default to exclude
        return false;
    }
    
    // If only blocks specified, default to include
    return true;
}

int filter_transport_stream_pids(uint8_t *ts_data, size_t ts_len, 
                               const struct peer_program_selection *selection)
{
    if (!ts_data || ts_len == 0) {
        return -1;
    }
    
    // Ensure buffer is multiple of 188 bytes (transport packet size)
    if (ts_len % 188 != 0) {
        return -1;
    }
    
    if (!selection || !selection->has_selection) {
        return 0;  // No filtering needed
    }
    
    size_t num_packets = ts_len / 188;
    
    for (size_t i = 0; i < num_packets; i++) {
        uint8_t *packet = ts_data + (i * 188);
        
        // Verify sync byte
        if (packet[0] != 0x47) {
            continue;  // Invalid packet, skip
        }
        
        // Extract PID (13 bits in bytes 1-2)
        uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];
        
        // Check if this PID should be included
        if (!should_include_pid(pid, selection)) {
            // Convert to NULL packet (PID 0x1FFF)
            packet[1] = (packet[1] & 0xE0) | 0x1F;  // Set PID high bits
            packet[2] = 0xFF;                       // Set PID low bits
            
            // Clear transport_error_indicator and set other fields appropriately
            packet[1] &= 0xFE;  // Clear transport_error_indicator
            packet[3] &= 0xCF;  // Clear adaptation_field_control (payload only)
            
            // Fill payload with 0xFF (NULL packet pattern)
            memset(&packet[4], 0xFF, 184);
        }
    }
    
    return 0;
}

int apply_null_packet_deletion(const uint8_t *input_data, size_t input_len,
                             uint8_t *output_data, size_t *output_len,
                             size_t max_output_len)
{
    if (!input_data || !output_data || !output_len || input_len == 0) {
        return -1;
    }
    
    if (input_len % 188 != 0) {
        return -1;  // Must be multiple of transport packet size
    }
    
    size_t num_input_packets = input_len / 188;
    size_t output_pos = 0;
    uint32_t null_pattern = 0;  // Track NULL packet positions (for RTP extension)
    int null_count = 0;
    
    for (size_t i = 0; i < num_input_packets && i < 32; i++) {  // Limit to 32 packets for pattern
        const uint8_t *packet = input_data + (i * 188);
        
        // Check if this is a NULL packet (PID 0x1FFF)
        if (packet[0] == 0x47) {  // Valid sync byte
            uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];
            
            if (pid == 0x1FFF) {
                // This is a NULL packet - mark it in pattern but don't copy
                null_pattern |= (1 << i);
                null_count++;
            } else {
                // Copy non-NULL packet to output
                if (output_pos + 188 <= max_output_len) {
                    memcpy(output_data + output_pos, packet, 188);
                    output_pos += 188;
                } else {
                    return -1;  // Output buffer too small
                }
            }
        } else {
            // Invalid packet - copy anyway
            if (output_pos + 188 <= max_output_len) {
                memcpy(output_data + output_pos, packet, 188);
                output_pos += 188;
            } else {
                return -1;
            }
        }
    }
    
    // Handle remaining packets (beyond 32) - just copy non-NULL packets
    for (size_t i = 32; i < num_input_packets; i++) {
        const uint8_t *packet = input_data + (i * 188);
        
        if (packet[0] == 0x47) {
            uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];
            
            if (pid != 0x1FFF) {  // Not a NULL packet
                if (output_pos + 188 <= max_output_len) {
                    memcpy(output_data + output_pos, packet, 188);
                    output_pos += 188;
                } else {
                    return -1;
                }
            }
        } else {
            // Invalid packet - copy anyway
            if (output_pos + 188 <= max_output_len) {
                memcpy(output_data + output_pos, packet, 188);
                output_pos += 188;
            } else {
                return -1;
            }
        }
    }
    
    *output_len = output_pos;
    
    // TODO: Create RTP extension header with null_pattern for receiver reconstruction
    // This would be used by the receiver to reconstruct the original stream
    
    return null_count;  // Return number of NULL packets removed
}

int filter_and_compress_for_peer(const uint8_t *input_data, size_t input_len,
                                uint8_t *output_data, size_t *output_len,
                                size_t max_output_len, uint32_t peer_id)
{
    if (!input_data || !output_data || !output_len) {
        return -1;
    }
    
    // Get program selection for this peer
    struct peer_program_selection *selection = program_selection_get_peer(peer_id);
    
    if (!selection || !selection->has_selection) {
        // No filtering needed - just copy input to output
        if (input_len <= max_output_len) {
            memcpy(output_data, input_data, input_len);
            *output_len = input_len;
            return 0;
        } else {
            return -1;
        }
    }
    
    // For small buffers, work directly on output buffer
    if (input_len <= max_output_len) {
        // Copy input to output buffer first
        memcpy(output_data, input_data, input_len);
        
        // Apply PID filtering in-place
        if (filter_transport_stream_pids(output_data, input_len, selection) != 0) {
            return -1;
        }
        
        // Apply NULL packet deletion
        uint8_t *temp_buffer = malloc(input_len);
        if (!temp_buffer) {
            return -1;
        }
        
        size_t compressed_len;
        int null_count = apply_null_packet_deletion(output_data, input_len, 
                                                   temp_buffer, &compressed_len, 
                                                   input_len);
        
        if (null_count >= 0 && compressed_len <= max_output_len) {
            memcpy(output_data, temp_buffer, compressed_len);
            *output_len = compressed_len;
        } else {
            null_count = -1;
        }
        
        free(temp_buffer);
        return null_count;
    }
    
    return -1;  // Buffer too large for current implementation
}

/* Internal helper functions */

static void free_peer_program_selection(struct peer_program_selection *selection)
{
    if (!selection) {
        return;
    }
    
    free(selection->requested_programs);
    free(selection->blocked_programs);
    free(selection->requested_pids);
    free(selection->blocked_pids);
    free(selection->destination_ip);
    free(selection->source_ip);
    free(selection);
}

static uint16_t* parse_pid_array(cJSON *json_array)
{
    if (!json_array || !cJSON_IsArray(json_array)) {
        return NULL;
    }
    
    int array_size = cJSON_GetArraySize(json_array);
    if (array_size == 0) {
        return NULL;
    }
    
    /* Allocate array with null terminator */
    uint16_t *pid_array = calloc(array_size + 1, sizeof(uint16_t));
    if (!pid_array) {
        return NULL;
    }
    
    int valid_count = 0;
    for (int i = 0; i < array_size; i++) {
        cJSON *item = cJSON_GetArrayItem(json_array, i);
        if (!item) continue;
        
        uint16_t pid_value = 0;
        bool valid_pid = false;
        
        if (cJSON_IsString(item)) {
            const char *pid_str = cJSON_GetStringValue(item);
            if (pid_str) {
                /* Handle hex format (0x prefix) */
                if (strncmp(pid_str, "0x", 2) == 0 || strncmp(pid_str, "0X", 2) == 0) {
                    unsigned long val = strtoul(pid_str, NULL, 16);
                    if (val <= 0x1FFF) {
                        pid_value = (uint16_t)val;
                        valid_pid = true;
                    }
                } else {
                    /* Handle decimal format */
                    unsigned long val = strtoul(pid_str, NULL, 10);
                    if (val <= 8191) {
                        pid_value = (uint16_t)val;
                        valid_pid = true;
                    }
                }
                
                /* Handle range format (e.g., "100-200" or "0x100-0x200") */
                char *dash = strchr(pid_str, '-');
                if (dash && !valid_pid) {
                    /* Range parsing - for now just take the first value */
                    /* TODO: Implement full range support in future version */
                    *dash = '\0';
                    unsigned long val = strtoul(pid_str, NULL, 
                                              (strncmp(pid_str, "0x", 2) == 0) ? 16 : 10);
                    if (val <= 8191) {
                        pid_value = (uint16_t)val;
                        valid_pid = true;
                    }
                    *dash = '-';  /* Restore string */
                }
            }
        } else if (cJSON_IsNumber(item)) {
            double val = cJSON_GetNumberValue(item);
            if (val >= 0 && val <= 8191 && val == (int)val) {
                pid_value = (uint16_t)val;
                valid_pid = true;
            }
        }
        
        if (valid_pid) {
            pid_array[valid_count++] = pid_value;
        }
    }
    
    /* Null terminate array */
    pid_array[valid_count] = 0;
    
    /* If no valid PIDs found, free array and return NULL */
    if (valid_count == 0) {
        free(pid_array);
        return NULL;
    }
    
    return pid_array;
}

static uint16_t* parse_program_array(cJSON *json_array)
{
    if (!json_array || !cJSON_IsArray(json_array)) {
        return NULL;
    }
    
    int array_size = cJSON_GetArraySize(json_array);
    if (array_size == 0) {
        return NULL;
    }
    
    /* Allocate array with null terminator */
    uint16_t *program_array = calloc(array_size + 1, sizeof(uint16_t));
    if (!program_array) {
        return NULL;
    }
    
    int valid_count = 0;
    for (int i = 0; i < array_size; i++) {
        cJSON *item = cJSON_GetArrayItem(json_array, i);
        if (!item || !cJSON_IsNumber(item)) {
            continue;
        }
        
        double val = cJSON_GetNumberValue(item);
        /* Valid program numbers are 1-65535 per spec */
        if (val >= 1 && val <= 65535 && val == (int)val) {
            program_array[valid_count++] = (uint16_t)val;
        }
    }
    
    /* Null terminate array */
    program_array[valid_count] = 0;
    
    /* If no valid programs found, free array and return NULL */
    if (valid_count == 0) {
        free(program_array);
        return NULL;
    }
    
    return program_array;
}

static struct peer_program_selection* find_peer_selection_locked(uint32_t peer_id)
{
    struct peer_program_selection *current = program_selections;
    while (current) {
        if (current->peer_id == peer_id) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}