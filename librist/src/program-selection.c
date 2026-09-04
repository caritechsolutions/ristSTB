/* librist. Copyright � 2024 SipRadius LLC. All right reserved.
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

/* =============================================================================
 * VSF TR-06-4 Part 6: Global PSI Cache for PAT/CAT/PMT parsed PIDs
 * =============================================================================
 * This cache stores PIDs extracted from PSI tables for program-based filtering.
 */

/* Program mapping: program_number -> its elementary PIDs */
struct program_pid_map {
    uint16_t program_number;       /* Service ID from PAT */
    uint16_t pmt_pid;              /* PMT PID for this program */
    uint16_t pcr_pid;              /* PCR PID */
    uint16_t elementary_pids[32];  /* Video, audio, data PIDs from PMT */
    int elementary_pid_count;
    uint8_t pmt_version;
    bool pmt_version_valid;
};

#define MAX_PROGRAMS 64
#define MAX_EMM_PIDS 16

static struct {
    /* PAT parsing state */
    uint8_t pat_version;
    bool pat_version_valid;

    /* Program map (from PAT + PMT parsing) */
    struct program_pid_map programs[MAX_PROGRAMS];
    int program_count;

    /* CAT parsing state */
    uint8_t cat_version;
    bool cat_version_valid;
    uint16_t emm_pids[MAX_EMM_PIDS];  /* EMM PIDs from CAT CA descriptors */
    int emm_pid_count;

    pthread_mutex_t lock;
} psi_cache = {
    .pat_version = 0,
    .pat_version_valid = false,
    .program_count = 0,
    .cat_version = 0,
    .cat_version_valid = false,
    .emm_pid_count = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

/* Internal helper functions */
static void free_peer_program_selection(struct peer_program_selection *selection);
static uint16_t* parse_pid_array(cJSON *json_array);
static uint16_t* parse_program_array(cJSON *json_array);
static struct peer_program_selection* find_peer_selection_locked(uint32_t peer_id);
static bool should_include_pid(uint16_t pid, const struct peer_program_selection *selection);
static void parse_pat_packet(const uint8_t *packet);
static void parse_cat_packet(const uint8_t *packet);
static void parse_pmt_packet(const uint8_t *packet, uint16_t pmt_pid);
static bool is_pmt_pid(uint16_t pid);
static bool is_emm_pid(uint16_t pid);
static bool pid_belongs_to_program(uint16_t pid, uint16_t program_number);
static struct program_pid_map* find_program_by_pmt_pid(uint16_t pmt_pid);

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
        free(selection->raw_json);
        selection->raw_json = NULL;
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
    /* Remember the exact bytes this was built from, so an identical keepalive
     * can be recognised as a no-op instead of rebuilding everything. */
    selection->raw_json = strdup(json_content);
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
 * @brief Check if a PID is a PMT PID (extracted from PAT)
 */
static bool is_pmt_pid(uint16_t pid)
{
    pthread_mutex_lock(&psi_cache.lock);
    for (int i = 0; i < psi_cache.program_count; i++) {
        if (psi_cache.programs[i].pmt_pid == pid) {
            pthread_mutex_unlock(&psi_cache.lock);
            return true;
        }
    }
    pthread_mutex_unlock(&psi_cache.lock);
    return false;
}

/**
 * @brief Find program entry by PMT PID (must hold psi_cache.lock)
 */
static struct program_pid_map* find_program_by_pmt_pid(uint16_t pmt_pid)
{
    for (int i = 0; i < psi_cache.program_count; i++) {
        if (psi_cache.programs[i].pmt_pid == pmt_pid) {
            return &psi_cache.programs[i];
        }
    }
    return NULL;
}

/**
 * @brief Check if a PID belongs to a specific program (must hold psi_cache.lock)
 *
 * This checks:
 * 1. Is the PID the PCR PID for this program?
 * 2. Is the PID one of the elementary stream PIDs (video/audio/data)?
 */
static bool pid_belongs_to_program(uint16_t pid, uint16_t program_number)
{
    /* Find the program in our cache */
    for (int i = 0; i < psi_cache.program_count; i++) {
        struct program_pid_map *prog = &psi_cache.programs[i];
        if (prog->program_number != program_number) {
            continue;
        }

        /* Check PCR PID */
        if (prog->pcr_pid == pid && prog->pcr_pid != 0x1FFF) {
            return true;
        }

        /* Check elementary PIDs (video, audio, data from PMT) */
        for (int j = 0; j < prog->elementary_pid_count; j++) {
            if (prog->elementary_pids[j] == pid) {
                return true;
            }
        }

        /* Program found but PID doesn't belong to it */
        return false;
    }

    /* Program not in cache (PMT not parsed yet) */
    return false;
}

/**
 * @brief Check if a PID is an EMM PID (extracted from CAT)
 */
static bool is_emm_pid(uint16_t pid)
{
    pthread_mutex_lock(&psi_cache.lock);
    for (int i = 0; i < psi_cache.emm_pid_count; i++) {
        if (psi_cache.emm_pids[i] == pid) {
            pthread_mutex_unlock(&psi_cache.lock);
            return true;
        }
    }
    pthread_mutex_unlock(&psi_cache.lock);
    return false;
}

/**
 * @brief Check if a PID should be included based on program selection rules
 *
 * Implements VSF TR-06-4 Part 6 filtering rules:
 * - Always include PSI/SI PIDs (0x00-0x1F) per DVB spec
 * - Always include PMT PIDs (extracted from PAT)
 * - Always include EMM PIDs (extracted from CAT)
 * - Apply requested/blocked program and PID rules
 *
 * @param pid The PID to check (0-8191)
 * @param selection Program selection configuration
 * @return true if PID should be included, false if should be filtered out
 */
static bool should_include_pid(uint16_t pid, const struct peer_program_selection *selection)
{
    if (!selection || !selection->has_selection) {
        return true;  /* No filtering - include everything */
    }

    /* =======================================================================
     * VSF TR-06-4 Part 6 Section 4.3: Mandatory PIDs (cannot be blocked)
     * =======================================================================
     * These PIDs MUST always pass through regardless of blockedPIDs:
     * - 0x0000: PAT (Program Association Table)
     * - 0x0001: CAT (Conditional Access Table)
     * - 0x0010: NIT (Network Information Table)
     * - 0x0011: SDT/BAT (Service Description / Bouquet Association)
     * - 0x0012: EIT (Event Information Table / EPG)
     * - 0x0013: RST (Running Status Table)
     * - 0x0014: TDT/TOT (Time and Date / Time Offset)
     * - 0x0015-0x001F: Reserved for future DVB use
     * - All PMT PIDs (from PAT)
     * - All EMM PIDs (from CAT)
     */

    /* Always include all PSI/SI PIDs (0x00-0x1F) - DVB mandatory tables */
    if (pid <= 0x1F) {
        return true;
    }

    /* Always include PMT PIDs (extracted from PAT parsing) */
    if (is_pmt_pid(pid)) {
        return true;
    }

    /* Always include EMM PIDs (extracted from CAT parsing) */
    if (is_emm_pid(pid)) {
        return true;
    }

    /* =======================================================================
     * Apply contentSelection filtering rules
     * ======================================================================= */

    /* Check if PID is explicitly requested */
    if (selection->requested_pids) {
        for (int i = 0; selection->requested_pids[i] != 0; i++) {
            if (selection->requested_pids[i] == pid) {
                return true;
            }
        }
    }

    /* Check if PID is explicitly blocked */
    if (selection->blocked_pids) {
        for (int i = 0; selection->blocked_pids[i] != 0; i++) {
            if (selection->blocked_pids[i] == pid) {
                return false;  /* Explicitly blocked */
            }
        }
    }

    /* =======================================================================
     * Check if PID belongs to a requested program (program-based filtering)
     * This requires PMT parsing to map program_number -> elementary PIDs
     * ======================================================================= */
    if (selection->requested_programs) {
        pthread_mutex_lock(&psi_cache.lock);
        for (int i = 0; selection->requested_programs[i] != 0; i++) {
            uint16_t program_number = selection->requested_programs[i];
            if (pid_belongs_to_program(pid, program_number)) {
                pthread_mutex_unlock(&psi_cache.lock);
                return true;  /* PID belongs to a requested program */
            }
        }
        pthread_mutex_unlock(&psi_cache.lock);
        /* PID doesn't belong to any requested program - exclude it */
        return false;
    }

    /* =======================================================================
     * Check if PID belongs to a blocked program
     * ======================================================================= */
    if (selection->blocked_programs) {
        pthread_mutex_lock(&psi_cache.lock);
        for (int i = 0; selection->blocked_programs[i] != 0; i++) {
            uint16_t program_number = selection->blocked_programs[i];
            if (pid_belongs_to_program(pid, program_number)) {
                pthread_mutex_unlock(&psi_cache.lock);
                return false;  /* PID belongs to a blocked program */
            }
        }
        pthread_mutex_unlock(&psi_cache.lock);
    }

    /* Default behavior based on what's specified */
    if (selection->requested_pids) {
        /* If specific PIDs requested, default to exclude others */
        return false;
    }

    /* If only blocks specified, default to include */
    return true;
}

/**
 * @brief Parse PAT (Program Association Table) to extract program -> PMT PID mapping
 *
 * PAT structure (PID 0x0000, table_id 0x00):
 * - Byte 0: table_id (0x00)
 * - Bytes 1-2: section_syntax_indicator(1) + '0'(1) + reserved(2) + section_length(12)
 * - Bytes 3-4: transport_stream_id
 * - Byte 5: reserved(2) + version_number(5) + current_next_indicator(1)
 * - Byte 6: section_number
 * - Byte 7: last_section_number
 * - Bytes 8+: [program_number(16) + reserved(3) + PMT_PID(13)] × N
 * - Last 4 bytes: CRC32
 *
 * @param packet Pointer to TS packet (must be 188 bytes, sync byte verified)
 */
static void parse_pat_packet(const uint8_t *packet)
{
    /* Check for payload unit start indicator */
    if (!(packet[1] & 0x40)) {
        return;  /* Not start of section */
    }

    /* Get adaptation field control */
    uint8_t afc = (packet[3] >> 4) & 0x03;
    int payload_offset = 4;

    /* Skip adaptation field if present */
    if (afc == 0x02) {
        return;  /* No payload */
    } else if (afc == 0x03) {
        payload_offset += 1 + packet[4];  /* Skip adaptation field */
    }

    /* Skip pointer field */
    payload_offset += packet[payload_offset] + 1;

    if (payload_offset >= 188) {
        return;  /* Invalid */
    }

    const uint8_t *section = &packet[payload_offset];

    /* Verify table_id is 0x00 (PAT) */
    if (section[0] != 0x00) {
        return;
    }

    /* Get section length */
    uint16_t section_length = ((section[1] & 0x0F) << 8) | section[2];
    if (section_length < 9 || payload_offset + 3 + section_length > 188) {
        return;  /* Invalid or spans multiple packets - not supported for now */
    }

    /* Get version number */
    uint8_t version = (section[5] >> 1) & 0x1F;
    bool current = section[5] & 0x01;

    if (!current) {
        return;  /* Not current, ignore */
    }

    pthread_mutex_lock(&psi_cache.lock);

    /* Check if version changed */
    if (psi_cache.pat_version_valid && psi_cache.pat_version == version) {
        pthread_mutex_unlock(&psi_cache.lock);
        return;  /* No change */
    }

    /* Clear existing program mappings (PAT changed) */
    psi_cache.program_count = 0;

    /* Parse program entries */
    int data_len = section_length - 5 - 4;  /* Subtract header and CRC */
    const uint8_t *data = &section[8];

    for (int i = 0; i < data_len && psi_cache.program_count < MAX_PROGRAMS; i += 4) {
        uint16_t program_number = (data[i] << 8) | data[i + 1];
        uint16_t pmt_pid = ((data[i + 2] & 0x1F) << 8) | data[i + 3];

        if (program_number == 0) {
            /* program_number 0 = NIT PID, skip */
            continue;
        }

        /* Create program entry with PMT PID (elementary PIDs filled by PMT parsing) */
        struct program_pid_map *prog = &psi_cache.programs[psi_cache.program_count];
        prog->program_number = program_number;
        prog->pmt_pid = pmt_pid;
        prog->pcr_pid = 0x1FFF;  /* Will be set by PMT parsing */
        prog->elementary_pid_count = 0;
        prog->pmt_version_valid = false;  /* PMT not parsed yet */
        psi_cache.program_count++;
    }

    psi_cache.pat_version = version;
    psi_cache.pat_version_valid = true;

    pthread_mutex_unlock(&psi_cache.lock);
}

/**
 * @brief Parse CAT (Conditional Access Table) to extract EMM PIDs
 *
 * CAT structure (PID 0x0001, table_id 0x01):
 * - Byte 0: table_id (0x01)
 * - Bytes 1-2: section_syntax_indicator(1) + '0'(1) + reserved(2) + section_length(12)
 * - Bytes 3-4: reserved (0xFFFF)
 * - Byte 5: reserved(2) + version_number(5) + current_next_indicator(1)
 * - Byte 6: section_number
 * - Byte 7: last_section_number
 * - Bytes 8+: CA_descriptors
 * - Last 4 bytes: CRC32
 *
 * CA_descriptor (tag 0x09):
 * - Byte 0: descriptor_tag (0x09)
 * - Byte 1: descriptor_length
 * - Bytes 2-3: CA_system_ID
 * - Bytes 4-5: reserved(3) + CA_PID(13) (this is the EMM PID)
 * - Bytes 6+: private_data (optional)
 *
 * @param packet Pointer to TS packet (must be 188 bytes, sync byte verified)
 */
static void parse_cat_packet(const uint8_t *packet)
{
    /* Check for payload unit start indicator */
    if (!(packet[1] & 0x40)) {
        return;  /* Not start of section */
    }

    /* Get adaptation field control */
    uint8_t afc = (packet[3] >> 4) & 0x03;
    int payload_offset = 4;

    /* Skip adaptation field if present */
    if (afc == 0x02) {
        return;  /* No payload */
    } else if (afc == 0x03) {
        payload_offset += 1 + packet[4];  /* Skip adaptation field */
    }

    /* Skip pointer field */
    payload_offset += packet[payload_offset] + 1;

    if (payload_offset >= 188) {
        return;  /* Invalid */
    }

    const uint8_t *section = &packet[payload_offset];

    /* Verify table_id is 0x01 (CAT) */
    if (section[0] != 0x01) {
        return;
    }

    /* Get section length */
    uint16_t section_length = ((section[1] & 0x0F) << 8) | section[2];
    if (section_length < 9 || payload_offset + 3 + section_length > 188) {
        return;  /* Invalid or spans multiple packets - not supported for now */
    }

    /* Get version number */
    uint8_t version = (section[5] >> 1) & 0x1F;
    bool current = section[5] & 0x01;

    if (!current) {
        return;  /* Not current, ignore */
    }

    pthread_mutex_lock(&psi_cache.lock);

    /* Check if version changed */
    if (psi_cache.cat_version_valid && psi_cache.cat_version == version) {
        pthread_mutex_unlock(&psi_cache.lock);
        return;  /* No change */
    }

    /* Parse CA descriptors to find EMM PIDs */
    psi_cache.emm_pid_count = 0;
    int data_len = section_length - 5 - 4;  /* Subtract header and CRC */
    const uint8_t *data = &section[8];

    for (int i = 0; i < data_len && psi_cache.emm_pid_count < 16;) {
        uint8_t tag = data[i];
        uint8_t len = data[i + 1];

        if (i + 2 + len > data_len) {
            break;  /* Invalid descriptor */
        }

        if (tag == 0x09 && len >= 4) {
            /* CA_descriptor found - extract EMM PID */
            uint16_t emm_pid = ((data[i + 4] & 0x1F) << 8) | data[i + 5];
            psi_cache.emm_pids[psi_cache.emm_pid_count++] = emm_pid;
        }

        i += 2 + len;  /* Move to next descriptor */
    }

    psi_cache.cat_version = version;
    psi_cache.cat_version_valid = true;

    pthread_mutex_unlock(&psi_cache.lock);
}

/**
 * @brief Parse PMT (Program Map Table) to extract elementary stream PIDs
 *
 * PMT structure (table_id 0x02):
 * - Byte 0: table_id (0x02)
 * - Bytes 1-2: section_syntax_indicator(1) + '0'(1) + reserved(2) + section_length(12)
 * - Bytes 3-4: program_number (service_id)
 * - Byte 5: reserved(2) + version_number(5) + current_next_indicator(1)
 * - Byte 6: section_number
 * - Byte 7: last_section_number
 * - Bytes 8-9: reserved(3) + PCR_PID(13)
 * - Bytes 10-11: reserved(4) + program_info_length(12)
 * - Bytes 12+program_info_length: program descriptors
 * - Then: [stream_type(8) + reserved(3) + elementary_PID(13) + reserved(4) + ES_info_length(12) + descriptors] × N
 * - Last 4 bytes: CRC32
 *
 * @param packet Pointer to TS packet (must be 188 bytes, sync byte verified)
 * @param pmt_pid The PID this packet was received on (to find program entry)
 */
static void parse_pmt_packet(const uint8_t *packet, uint16_t pmt_pid)
{
    /* Check for payload unit start indicator */
    if (!(packet[1] & 0x40)) {
        return;  /* Not start of section */
    }

    /* Get adaptation field control */
    uint8_t afc = (packet[3] >> 4) & 0x03;
    int payload_offset = 4;

    /* Skip adaptation field if present */
    if (afc == 0x02) {
        return;  /* No payload */
    } else if (afc == 0x03) {
        payload_offset += 1 + packet[4];  /* Skip adaptation field */
    }

    /* Skip pointer field */
    payload_offset += packet[payload_offset] + 1;

    if (payload_offset >= 188) {
        return;  /* Invalid */
    }

    const uint8_t *section = &packet[payload_offset];

    /* Verify table_id is 0x02 (PMT) */
    if (section[0] != 0x02) {
        return;
    }

    /* Get section length */
    uint16_t section_length = ((section[1] & 0x0F) << 8) | section[2];
    if (section_length < 13 || payload_offset + 3 + section_length > 188) {
        return;  /* Invalid or spans multiple packets - not supported for now */
    }

    /* Get program number */
    uint16_t program_number = (section[3] << 8) | section[4];

    /* Get version number */
    uint8_t version = (section[5] >> 1) & 0x1F;
    bool current = section[5] & 0x01;

    if (!current) {
        return;  /* Not current, ignore */
    }

    pthread_mutex_lock(&psi_cache.lock);

    /* Find program entry by PMT PID */
    struct program_pid_map *prog = find_program_by_pmt_pid(pmt_pid);
    if (!prog) {
        pthread_mutex_unlock(&psi_cache.lock);
        return;  /* Program not in PAT yet */
    }

    /* Verify program number matches */
    if (prog->program_number != program_number) {
        pthread_mutex_unlock(&psi_cache.lock);
        return;  /* Mismatch - shouldn't happen */
    }

    /* Check if version changed */
    if (prog->pmt_version_valid && prog->pmt_version == version) {
        pthread_mutex_unlock(&psi_cache.lock);
        return;  /* No change */
    }

    /* Get PCR PID */
    prog->pcr_pid = ((section[8] & 0x1F) << 8) | section[9];

    /* Get program info length (skip program-level descriptors) */
    uint16_t prog_info_len = ((section[10] & 0x0F) << 8) | section[11];

    /* Parse elementary stream info */
    prog->elementary_pid_count = 0;
    int pos = 12 + prog_info_len;
    int end = section_length - 4;  /* Exclude CRC */

    while (pos < end && prog->elementary_pid_count < 32) {
        if (pos + 5 > end) {
            break;
        }

        /* Get stream type and elementary PID */
        /* uint8_t stream_type = section[pos]; */
        uint16_t es_pid = ((section[pos + 1] & 0x1F) << 8) | section[pos + 2];
        uint16_t es_info_len = ((section[pos + 3] & 0x0F) << 8) | section[pos + 4];

        /* Store elementary PID */
        prog->elementary_pids[prog->elementary_pid_count++] = es_pid;

        pos += 5 + es_info_len;
    }

    prog->pmt_version = version;
    prog->pmt_version_valid = true;

    pthread_mutex_unlock(&psi_cache.lock);
}

int filter_transport_stream_pids(uint8_t *ts_data, size_t ts_len,
                               const struct peer_program_selection *selection)
{
    /* Handle edge cases - pass through unchanged */
    if (!ts_data || ts_len == 0) {
        return 0;  /* Empty data - nothing to filter */
    }

    /* Check if this looks like MPEG-TS data:
     * - Must be multiple of 188 bytes
     * - First byte should be sync byte 0x47
     * If not TS, return success without filtering (pass through) */
    if (ts_len % 188 != 0) {
        /* Not MPEG-TS - pass through unchanged (not an error) */
        return 0;
    }

    /* Check for TS sync byte */
    if (ts_data[0] != 0x47) {
        /* Doesn't look like TS - pass through unchanged */
        return 0;
    }

    if (!selection || !selection->has_selection) {
        return 0;  /* No filtering needed */
    }

    size_t num_packets = ts_len / 188;

    for (size_t i = 0; i < num_packets; i++) {
        uint8_t *packet = ts_data + (i * 188);

        /* Verify sync byte */
        if (packet[0] != 0x47) {
            continue;  /* Invalid packet, skip */
        }

        /* Extract PID (13 bits in bytes 1-2) */
        uint16_t pid = ((packet[1] & 0x1F) << 8) | packet[2];

        /* =================================================================
         * VSF TR-06-4 Part 6: Parse PAT/CAT/PMT for PID extraction
         * =================================================================
         * Before filtering, parse PSI tables to build our program -> PID map:
         * - PAT (0x0000): Maps program_number -> PMT_PID
         * - CAT (0x0001): Extracts EMM PIDs from CA descriptors
         * - PMT (various): Extracts elementary PIDs (video/audio) per program
         */
        if (pid == 0x0000) {
            parse_pat_packet(packet);
        } else if (pid == 0x0001) {
            parse_cat_packet(packet);
        } else if (is_pmt_pid(pid)) {
            /* This is a PMT - parse to get elementary stream PIDs */
            parse_pmt_packet(packet, pid);
        }

        /* Check if this PID should be included */
        if (!should_include_pid(pid, selection)) {
            /* Convert to standard NULL packet (PID 0x1FFF)
             * The NULL packet format must match what suppress_null_packets() expects:
             * - Byte 1: TEI=0, PUSI=0, TP=0, PID_high=0x1F -> 0x1F
             * - Byte 2: PID_low=0xFF -> 0xFF
             * suppress_null_packets() checks if be16toh(flags1) == 0x1FFF exactly,
             * so we must clear TEI/PUSI/TP bits, not just set the PID bits. */
            packet[1] = 0x1F;  /* TEI=0, PUSI=0, TP=0, PID high 5 bits = 0x1F */
            packet[2] = 0xFF;  /* PID low 8 bits = 0xFF -> PID = 0x1FFF */
            packet[3] = 0x10;  /* Scrambling=0, AFC=payload only(01), CC=0 */

            /* Fill payload with 0xFF (standard NULL packet pattern) */
            memset(&packet[4], 0xFF, 184);
        }
    }

    return 0;
}

/**
 * @brief Apply NULL packet deletion (DEPRECATED)
 *
 * @deprecated This function is deprecated in favor of standard librist NPD.
 * Use rist_sender_npd_enable() instead, which:
 * - Properly creates RTP extension headers for NULL packet reinsertion
 * - Works with expand_null_packets() on the receiver
 * - Maintains PCR timing integrity
 *
 * This function is retained for backwards compatibility but should not be
 * used in new code. It does NOT create the RTP extension headers needed
 * for proper NULL packet reinsertion on the receiver side.
 */
int apply_null_packet_deletion(const uint8_t *input_data, size_t input_len,
                             uint8_t *output_data, size_t *output_len,
                             size_t max_output_len)
{
    if (!input_data || !output_data || !output_len || input_len == 0) {
        return -1;
    }

    if (input_len % 188 != 0) {
        return -1;  /* Must be multiple of transport packet size */
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

/**
 * @brief Filter and prepare transport stream for a specific peer
 *
 * VSF TR-06-4 Part 6 implementation:
 * 1. Apply PID filtering based on peer's contentSelection (convert unwanted PIDs to NULL)
 * 2. Return filtered data - NULL packet deletion is handled by standard librist NPD
 *
 * Note: This function no longer performs NULL packet deletion. Instead:
 * - Unwanted PIDs are converted to NULL packets (PID 0x1FFF)
 * - Standard librist NPD (rist_sender_npd_enable) handles NULL compression
 * - The receiver automatically reinstates NULLs via RTP extension header
 *
 * @param input_data Input transport stream data
 * @param input_len Length of input data (must be multiple of 188)
 * @param output_data Output buffer for filtered data
 * @param output_len Output: actual length of filtered data
 * @param max_output_len Maximum size of output buffer
 * @param peer_id Peer ID to get contentSelection for
 * @return 0 on success, -1 on error
 */
int filter_and_compress_for_peer(const uint8_t *input_data, size_t input_len,
                                uint8_t *output_data, size_t *output_len,
                                size_t max_output_len, uint32_t peer_id)
{
    if (!output_len) {
        return -1;
    }

    /* Handle empty input - just set output_len to 0 and return success */
    if (!input_data || input_len == 0) {
        *output_len = 0;
        return 0;
    }

    if (!output_data) {
        return -1;
    }

    /* Get program selection for this peer */
    struct peer_program_selection *selection = program_selection_get_peer(peer_id);

    if (!selection || !selection->has_selection) {
        /* No filtering needed - just copy input to output */
        if (input_len <= max_output_len) {
            memcpy(output_data, input_data, input_len);
            *output_len = input_len;
            return 0;
        } else {
            return -1;
        }
    }

    if (input_len > max_output_len) {
        return -1;  /* Output buffer too small */
    }

    /* Copy input to output buffer */
    memcpy(output_data, input_data, input_len);

    /* Apply PID filtering in-place (converts unwanted PIDs to NULL packets) */
    if (filter_transport_stream_pids(output_data, input_len, selection) != 0) {
        return -1;
    }

    /*
     * NOTE: NULL packet deletion is now handled by standard librist NPD
     * ================================================================
     * When the sender has NPD enabled (via rist_sender_npd_enable() or -n flag):
     * - suppress_null_packets() in mpegts.c removes NULL packets
     * - Creates "RI" RTP extension header with NPD bitmap
     * - Receiver automatically reinstates NULLs via expand_null_packets()
     *
     * This keeps PCR timing intact and follows the standard librist flow.
     */

    *output_len = input_len;
    return 0;
}

bool program_selection_selection_unchanged(uint32_t peer_id, const char *json_content)
{
    if (!json_content || !program_selection_initialized) {
        return false;
    }
    pthread_mutex_lock(&program_selection_lock);
    struct peer_program_selection *sel = find_peer_selection_locked(peer_id);
    bool same = (sel && sel->has_selection && sel->raw_json &&
                 strcmp(sel->raw_json, json_content) == 0);
    pthread_mutex_unlock(&program_selection_lock);
    return same;
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
    free(selection->raw_json);
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