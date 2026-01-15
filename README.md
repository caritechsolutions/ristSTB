# RIST Part 7 Implementation (VSF TR-06-4)

This repository contains a modified version of librist with implementations for VSF TR-06-4 Part 7 (Full Stream Request/Recovery) and Part 6 (Program Selection). It provides a working RIST Part 7 STB (Set-Top Box) implementation for multi-link redundancy scenarios.

## Overview

This implementation enables satellite receivers to request full stream recovery from terrestrial backup links when the primary satellite connection degrades. Key capabilities include:

- **Full Stream Request (FSR)** - Signaling protocol for receivers to request/release full stream recovery
- **Program Selection** - Content filtering based on VSF TR-06-4 Part 6 contentSelection
- **Marker-based Stream Validation** - Metadata markers for stream integrity verification
- **Circuit Breaker Protection** - Automatic failure detection and recovery

## Repository Structure

```
/
├── librist/                    # Modified librist library
├── ristreceiver_with_markers.c # RIST receiver with Part 7 marker insertion
├── ristsender_marker.c         # RIST sender with marker validation & circuit breaker
├── rist_watchdog.c             # Process watchdog for automatic restart
├── VSF_TR-06-4-Part-*.pdf      # VSF specification documents
└── README.md                   # This file
```

## Specification Documents

The following VSF TR-06-4 specification documents are included:
- Part 1-6: Core RIST specifications
- **Part 7**: Full Stream Request (FSR) - Primary focus of this implementation

---

## Modifications to librist

### Summary of Changes

| File | Type | Description |
|------|------|-------------|
| `src/proto/rtp.h` | Modified | Added FSR subtype definitions |
| `src/proto/rtp.c` | Modified | Added FSR packet writing functions |
| `src/udp.c` | Modified | Added FSR list management, data filtering, **weight-1000 send logic** |
| `src/rist-common.c` | Modified | Added FSR message processing, recovery logic, **weight-1000 NACK routing** |
| `src/rist.c` | Modified | Added program selection initialization |
| `src/program-selection.h` | **New** | Program selection API header |
| `src/program-selection.c` | **New** | Program selection implementation |

### Key Architectural Changes

| Feature | Original Behavior | Modified Behavior |
|---------|------------------|-------------------|
| **NACK Routing** | Send to peer with lowest RTT | Prioritize weight-1000 peers, then lowest RTT |
| **Data Sending** | Weight-based load balancing | Weight-1000 only sends when FSR requested |
| **Recovery Agents** | N/A | Weight-1000 peers designated as recovery agents |

---

### 1. Full Stream Request (FSR) Protocol Definitions

**File:** `librist/src/proto/rtp.h` (Lines 114-116)

Added RTCP APP packet subtypes for FSR signaling per TR-06-4 Section 7.1:

```c
/* RIST Part 7 Full Stream Request (FSR) subtypes - TR-06-4 Section 7.1 */
#define FSR_SUBTYPE_ENABLE  5   /* Full Stream Request Enable */
#define FSR_SUBTYPE_DISABLE 6   /* Full Stream Request Disable */
```

---

### 2. FSR Packet Writing Functions

**File:** `librist/src/proto/rtp.c` (Lines 140-164)

Added functions to create FSR Enable/Disable RTCP packets:

```c
/* Creates FSR Enable packet for requesting full stream recovery */
rist_rtcp_write_fsr_enable()

/* Creates FSR Disable packet for releasing full stream recovery */
rist_rtcp_write_fsr_disable()
```

---

### 3. Global FSR List Management

**File:** `librist/src/udp.c` (Lines 32-134)

Added a global tracking system for peers that have requested Full Stream Recovery:

```c
// Global FSR (Full Stream Recovery) tracking for weight-1000 recovery agents
static uint32_t *fsr_peers = NULL;
static int fsr_peer_count = 0;
static int fsr_peers_capacity = 0;
static pthread_mutex_t fsr_lock = PTHREAD_MUTEX_INITIALIZER;
```

**Helper functions added:**
| Function | Line | Description |
|----------|------|-------------|
| `peer_id_in_fsr_list()` | 42 | Checks if peer is in FSR list |
| `add_to_fsr_list()` | 56 | Adds peer to FSR list with dynamic array management |
| `remove_from_fsr_list()` | 87 | Removes peer from FSR list with array shrinking |
| `has_fsr_requests()` | 126 | Checks if any peers have requested FSR |

---

### 4. FSR Send Functions

**File:** `librist/src/udp.c` (Lines 1431-1481)

Added functions to send FSR messages to peers:

```c
/* Sends FSR Enable message and adds peer to FSR list */
rist_send_fsr_enable(struct rist_peer *peer, uint32_t flow_id)

/* Sends FSR Disable message and removes peer from FSR list */
rist_send_fsr_disable(struct rist_peer *peer, uint32_t flow_id)
```

Both functions automatically manage the FSR list and log operations.

---

### 5. FSR Message Processing

**File:** `librist/src/rist-common.c` (Lines 2366-2420)

Added handling for incoming FSR RTCP APP packets:

```c
/* RIST Part 7 Full Stream Request (FSR) signaling */
else if (subtype == FSR_SUBTYPE_ENABLE || subtype == FSR_SUBTYPE_DISABLE) {
    // Validates RIST packet (checks for "RIST" name field)
    // Only processes on sender side
    // Validates peer authentication
    // Calls add_to_fsr_list() or remove_from_fsr_list()
}
```

---

### 6. FSR Decision Logic (Receiver Protocol Loop)

**File:** `librist/src/rist-common.c` (Lines 4661-4940)

Comprehensive FSR decision logic in the receiver protocol loop:

- **FSR Enable triggers:**
  - Satellite peer is dead (no heartbeat)
  - Peer timeout (2+ seconds since last data)
  - High RTT (>500ms indicating degraded link)

- **FSR Disable triggers:**
  - Satellite peer recovers
  - Link quality improves

- **Peer classification:**
  - Tracks satellite vs. recovery (weight-1000) peers
  - Sends FSR Enable/Disable keepalive messages
  - Monitors recovery data flow

---

### 7. Global Recovery Agent System

**File:** `librist/src/rist-common.c` (Lines 45-47, 1110-1123)

Added global tracking for the recovery agent:

```c
static struct rist_peer *g_current_recovery_agent = NULL;
static pthread_mutex_t g_recovery_agent_lock = PTHREAD_MUTEX_INITIALIZER;
```

The weight-1000 recovery agent is stored globally for FSR operations.

---

### 8. Program Selection (VSF TR-06-4 Part 6)

**New Files:**
- `librist/src/program-selection.h` - Header with API declarations
- `librist/src/program-selection.c` - Implementation

**Key Functions:**
| Function | Description |
|----------|-------------|
| `program_selection_init()` | Initialize the system |
| `program_selection_add_peer()` | Add peer with content selection |
| `program_selection_remove_peer()` | Remove peer from system |
| `program_selection_get_peer()` | Get peer configuration |
| `program_selection_peer_has_selection()` | Check if peer has selection |
| `filter_and_compress_for_peer()` | Filter/compress based on selection |
| `program_selection_cleanup()` | Cleanup system on shutdown |

**Integration in `rist-common.c` (Lines 3001-3022):**

```c
// Parse JSON payload for contentSelection (VSF TR-06-4 Part 6)
if (info.json_len > 0 && info.json) {
    cJSON *json = cJSON_ParseWithLength((const char*)info.json, info.json_len);
    if (json) {
        cJSON *content_selection = cJSON_GetObjectItem(json, "contentSelection");
        if (content_selection && cJSON_IsArray(content_selection)) {
            // Found contentSelection - store it for this peer
            program_selection_add_peer(p->adv_peer_id, content_str);
        }
    }
}
```

---

### 9. Data Filtering Based on Program Selection

**File:** `librist/src/udp.c` (Lines 139-192)

Added filtered data send function:

```c
static void send_filtered_data_to_peer(struct rist_peer *target_peer,
                                      struct rist_buffer *buffer, ...)
{
    // Check if peer has program selection
    if (program_selection_peer_has_selection(target_peer->adv_peer_id)) {
        // Apply PID filtering and NULL packet deletion
        filter_result = filter_and_compress_for_peer(...);
    }
    // Send (filtered or original) data to peer
}
```

Multiple calls throughout `udp.c` now use `send_filtered_data_to_peer()` for content-aware delivery.

---

## PID Selection: Complete Flow and Implementation Status

This section documents the full PID selection mechanism per VSF TR-06-4 Part 6, including what's implemented, what's missing, and the TODO items for completion.

### Overview

PID Selection allows RIST receivers to request specific programs/PIDs via OOB (Out-of-Band) messaging. The sender then filters the transport stream per-peer, replacing unwanted PIDs with NULL packets and using NULL Packet Deletion to save bandwidth.

### Complete PID Selection Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         PID SELECTION FLOW                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  1. RECEIVER SENDS CONTENT SELECTION (OOB via Keepalive)                   │
│     ┌─────────────────────────────────────────────────────────────────┐    │
│     │ {                                                                │    │
│     │   "contentSelection": [{                                        │    │
│     │     "UDPPort": 5000,                                            │    │
│     │     "requestedPrograms": [1, 2, 3],                             │    │
│     │     "blockedPrograms": [4, 5],                                  │    │
│     │     "requestedPIDs": ["0x100", "0x200-0x20F"],                  │    │
│     │     "blockedPIDs": ["0x300"]                                    │    │
│     │   }]                                                            │    │
│     │ }                                                                │    │
│     └─────────────────────────────────────────────────────────────────┘    │
│                                    │                                        │
│                                    ▼                                        │
│  2. SENDER RECEIVES KEEPALIVE (rist-common.c:2984-3022)                    │
│     ├─ Parse JSON payload                                                  │
│     ├─ Extract "contentSelection" array                                    │
│     └─ Call program_selection_add_peer(peer_id, json)                      │
│                                    │                                        │
│                                    ▼                                        │
│  3. STORE PER-PEER SELECTION (program-selection.c:42-153)                  │
│     ├─ Parse requestedPrograms[], blockedPrograms[]                        │
│     ├─ Parse requestedPIDs[], blockedPIDs[]                                │
│     ├─ Store in peer_program_selection linked list                         │
│     └─ Set has_selection = true                                            │
│                                    │                                        │
│                                    ▼                                        │
│  4. ON EVERY DATA SEND (udp.c:139-192, 1031-1106)                         │
│     ├─ For each peer in send loop                                          │
│     └─ Call send_filtered_data_to_peer()                                   │
│                                    │                                        │
│                                    ▼                                        │
│  5. CHECK IF PEER HAS SELECTION (program-selection.c:202-206)             │
│     └─ program_selection_peer_has_selection(peer_id)                       │
│            │                                                               │
│            ├─ NO  → Send original unfiltered data                          │
│            └─ YES → Continue to filtering                                  │
│                                    │                                        │
│                                    ▼                                        │
│  6. FILTER AND COMPRESS (program-selection.c:462-517)                      │
│     └─ filter_and_compress_for_peer()                                      │
│            │                                                               │
│            ├─ 6a. filter_transport_stream_pids()                           │
│            │      ├─ For each TS packet (188 bytes)                        │
│            │      ├─ Check should_include_pid()                            │
│            │      ├─ If NO → Convert to NULL packet (PID 0x1FFF)          │
│            │      └─ If YES → Keep packet unchanged                        │
│            │                                                               │
│            └─ 6b. apply_null_packet_deletion()                             │
│                   ├─ Scan for NULL packets (PID 0x1FFF)                    │
│                   ├─ Copy only non-NULL packets to output                  │
│                   └─ Return compressed buffer (smaller size)               │
│                                    │                                        │
│                                    ▼                                        │
│  7. SEND FILTERED DATA                                                     │
│     └─ rist_send_common_rtcp(peer, filtered_data, reduced_size)           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### PID Filtering Rules (VSF TR-06-4 Part 6, Section 4.3)

Per the specification, the sender **shall always include** the following PIDs regardless of blockedPIDs:

| PID | Description | Priority |
|-----|-------------|----------|
| **0x0000** | PAT (Program Association Table) | Mandatory |
| **0x0001** | CAT (Conditional Access Table) | Mandatory |
| **All PMT PIDs** | Program Map Tables (from PAT) | Mandatory |
| **All EMM PIDs** | Entitlement Management Messages (from CAT) | Mandatory |

Additionally, for DVB compliance:
| PID Range | Description | Recommendation |
|-----------|-------------|----------------|
| **0x0010** | NIT (Network Information Table) | Should pass |
| **0x0011** | SDT/BAT (Service Description) | Should pass |
| **0x0012** | EIT (Event Information / EPG) | Should pass |
| **0x0014** | TDT/TOT (Time and Date) | Should pass |

### Current Implementation Status

| Component | File | Status | Notes |
|-----------|------|--------|-------|
| Keepalive reception | `rist-common.c:2984` | ✅ Done | Parses incoming keepalives |
| JSON extraction | `rist-common.c:3001-3022` | ✅ Done | Extracts contentSelection |
| Per-peer storage | `program-selection.c:42-153` | ✅ Done | Linked list by peer_id |
| Selection check | `program-selection.c:202-206` | ✅ Done | `peer_has_selection()` |
| PID→NULL conversion | `program-selection.c:333-378` | ✅ Done | `filter_transport_stream_pids()` |
| NULL deletion | `program-selection.c:380-459` | ✅ Done | `apply_null_packet_deletion()` |
| Filtered send | `udp.c:139-192` | ✅ Done | `send_filtered_data_to_peer()` |
| Always pass PAT (0) | `program-selection.c:295` | ✅ Done | Hardcoded check |
| Always pass CAT (1) | `program-selection.c:295` | ✅ Done | Hardcoded check |
| Always pass 0x00-0x1F | `program-selection.c` | ❌ Missing | Only 0 and 1 currently |
| Parse PAT → PMT PIDs | `program-selection.c` | ❌ Missing | Requires PAT parsing |
| Parse CAT → EMM PIDs | `program-selection.c` | ❌ Missing | Requires CAT parsing |
| Always pass PMT PIDs | `program-selection.c` | ❌ Missing | Needs PAT parsing first |
| Always pass EMM PIDs | `program-selection.c` | ❌ Missing | Needs CAT parsing first |
| Version monitoring | `program-selection.c` | ❌ Missing | For efficient re-parsing |

### Reference Transport Stream Analysis

Based on analysis of a live DTH transport (`transport_analysis.txt`):

```
Transport Summary:
├── Services: 34
├── Total PIDs: 197 (128 clear, 69 scrambled)
├── Bitrate: ~60.7 Mbps
└── CAS Systems: 4 (Beijing Compunicate, Irdeto, Verimatrix, Cryptoguard)

PIDs to Always Pass:
├── PSI/SI (0x00-0x1F): PAT, CAT, SDT, EIT
├── PMT PIDs: 34 (one per service, from PAT)
├── EMM PIDs: 4 (0x0583-0x0586, from CAT)
└── Total "always pass": ~42 PIDs

Table Sizes:
├── PAT: 34 programs × 4 bytes + 12 overhead = 148 bytes (fits in 1 packet ✅)
└── CAT: 4 EMM entries × ~8 bytes + 12 overhead = ~44 bytes (fits in 1 packet ✅)
```

---

## PID Selection TODO List

### TODO 1: Expand PSI/SI Bypass Range

**File:** `librist/src/program-selection.c`
**Function:** `should_include_pid()` (lines 288-331)
**Priority:** High

**Current code:**
```c
if (pid == 0 || pid == 1) {  // PAT, CAT
    return true;
}
```

**Required change:**
```c
// Always pass all PSI/SI PIDs (0x00-0x1F)
if (pid <= 0x1F) {
    return true;
}
```

**Reason:** DVB requires PAT, CAT, NIT, SDT, EIT, TDT etc. to pass through for receiver functionality.

---

### TODO 2: Add PAT Parsing for PMT PID Extraction

**File:** `librist/src/program-selection.c`
**New function:** `parse_pat_for_pmt_pids()`
**Priority:** High

**Required functionality:**
1. Detect PID 0 packets in the stream
2. Check version byte (byte 5 of section: `version = (byte >> 1) & 0x1F`)
3. If version changed from cached value, parse the PAT:
   - Skip 8-byte header
   - Read program entries (4 bytes each): `program_number` (16 bits) + `PMT_PID` (13 bits)
   - `program_number = 0` means NIT PID, skip it
   - Store all PMT PIDs in a cache array
4. Cache the version number to avoid re-parsing unchanged PATs

**PAT structure:**
```
Byte 0:     table_id (0x00)
Bytes 1-2:  section_syntax_indicator(1) + section_length(12)
Bytes 3-4:  transport_stream_id
Byte 5:     reserved(2) + version_number(5) + current_next_indicator(1)
Bytes 6-7:  section_number, last_section_number
Bytes 8+:   [program_number(16) + reserved(3) + PMT_PID(13)] × N
Last 4:     CRC32
```

**Reason:** Spec requires all PMT PIDs to pass through, including for non-selected programs.

---

### TODO 3: Add CAT Parsing for EMM PID Extraction

**File:** `librist/src/program-selection.c`
**New function:** `parse_cat_for_emm_pids()`
**Priority:** High

**Required functionality:**
1. Detect PID 1 packets in the stream
2. Check version byte (same as PAT)
3. If version changed, parse the CAT:
   - Skip 8-byte header
   - Iterate through CA_descriptors (descriptor_tag = 0x09)
   - Extract EMM_PID from each descriptor (bytes 4-5, 13 bits)
   - Store all EMM PIDs in a cache array
4. Cache the version number

**CA_descriptor structure:**
```
Byte 0:     descriptor_tag (0x09)
Byte 1:     descriptor_length
Bytes 2-3:  CA_system_ID
Bytes 4-5:  reserved(3) + EMM_PID(13)
Bytes 6+:   private_data (optional)
```

**Reason:** EMM PIDs carry conditional access entitlement data required for all CAS systems.

---

### TODO 4: Integrate PSI Parsing into Filter Loop

**File:** `librist/src/program-selection.c`
**Function:** `filter_transport_stream_pids()` (lines 333-378)
**Priority:** High

**Required changes:**
1. Before filtering, scan for PID 0 and PID 1 packets
2. Call PAT/CAT parsers if version changed
3. Update `should_include_pid()` to check against cached PMT/EMM PID lists

**Suggested approach:**
```c
int filter_transport_stream_pids(uint8_t *ts_data, size_t ts_len,
                                const struct peer_program_selection *selection)
{
    // First pass: Check for PAT/CAT updates
    for (each packet) {
        if (pid == 0) check_and_parse_pat(packet);
        if (pid == 1) check_and_parse_cat(packet);
    }

    // Second pass: Filter PIDs
    for (each packet) {
        if (!should_include_pid(pid, selection)) {
            convert_to_null(packet);
        }
    }
}
```

**Alternative (more efficient):**
- Single pass: check PAT/CAT inline while filtering
- Use version cache to skip parsing on most packets

---

### TODO 5: Add Global PID Cache Structure

**File:** `librist/src/program-selection.c`
**New data structures:**
**Priority:** Medium

```c
// Global cache for PSI-derived PIDs
static struct {
    // PAT parsing state
    uint8_t pat_version;
    bool pat_version_valid;
    uint16_t pmt_pids[64];      // Max 64 programs
    int pmt_pid_count;

    // CAT parsing state
    uint8_t cat_version;
    bool cat_version_valid;
    uint16_t emm_pids[16];      // Max 16 EMM PIDs
    int emm_pid_count;

    pthread_mutex_t lock;
} psi_cache = {0};
```

**Reason:** Centralized cache for efficient lookup during filtering.

---

### TODO 6: Update should_include_pid() for PMT/EMM Bypass

**File:** `librist/src/program-selection.c`
**Function:** `should_include_pid()` (lines 288-331)
**Priority:** High

**Required changes:**
```c
static bool should_include_pid(uint16_t pid, const struct peer_program_selection *selection)
{
    // Always include PSI/SI PIDs (0x00-0x1F)
    if (pid <= 0x1F) {
        return true;
    }

    // Always include PMT PIDs (from PAT)
    if (is_pmt_pid(pid)) {
        return true;
    }

    // Always include EMM PIDs (from CAT)
    if (is_emm_pid(pid)) {
        return true;
    }

    // ... rest of existing logic for requestedPIDs/blockedPIDs
}
```

---

### TODO 7: Add NULL Packet Reinsertion (Receiver Side)

**File:** `librist/src/program-selection.c` or new file
**New function:** `apply_null_packet_reinsertion()`
**Priority:** Low (future enhancement)

**Purpose:** On the receiver side, reinsert NULL packets to restore original PCR timing.

**Note:** The current `apply_null_packet_deletion()` has a TODO at line 456:
```c
// TODO: Create RTP extension header with null_pattern for receiver reconstruction
```

This requires:
1. Sender to include null_pattern in RTP extension header
2. Receiver to read pattern and reinsert NULLs at correct positions

---

### Implementation Order

| Order | TODO | Complexity | Impact |
|-------|------|------------|--------|
| 1 | TODO 1: Expand 0x00-0x1F bypass | Low | High - immediate DVB compliance |
| 2 | TODO 5: Add PID cache structure | Low | Required for TODO 2-4 |
| 3 | TODO 2: PAT parsing | Medium | High - PMT PIDs |
| 4 | TODO 3: CAT parsing | Medium | High - EMM PIDs |
| 5 | TODO 6: Update should_include_pid() | Low | High - uses cached PIDs |
| 6 | TODO 4: Integrate into filter loop | Medium | High - connects everything |
| 7 | TODO 7: NULL reinsertion | High | Low - future enhancement |

### Section Assembly Note

For the reference transport (34 services):
- **PAT:** 148 bytes - fits in single TS packet ✅
- **CAT:** ~44 bytes - fits in single TS packet ✅

Section assembly (handling tables spanning multiple packets) is **not required** for this transport. However, for robustness with larger multiplexes (45+ programs), section assembly should be considered as a future enhancement.

---

### 10. FSR Cleanup

**Peer Timeout Handler** (`rist-common.c` Lines 3707-3716):
```c
/* FSR CLEANUP: Remove timed-out peer from FSR list */
if (!peer->receiver_mode) {
    remove_from_fsr_list(peer->adv_peer_id);
}
```

**Peer Removal** (`rist-common.c` Lines 4079-4093):
```c
// Clean up program selection for this peer
program_selection_remove_peer(peer->adv_peer_id);

/* FSR CLEANUP: Remove peer from FSR list when peer is being removed */
remove_from_fsr_list(peer->adv_peer_id);
```

---

### 11. Initialization and Cleanup

**Program Selection Init** (`rist.c` Lines 440-444):
```c
// Initialize program selection system
if (program_selection_init() != 0) {
    rist_log_priv2(logging_settings, RIST_LOG_WARN,
                   "Failed to initialize program selection system\n");
}
```

**Program Selection Cleanup** (`rist-common.c` Lines 5074-5075):
```c
rist_log_priv(&ctx->common, RIST_LOG_INFO, "Cleaning up program selection system\n");
program_selection_cleanup();
```

---

## Weight-1000 Recovery Agent System

A key architectural feature of this implementation is the use of **weight-1000 peers** to designate recovery agents. This enables intelligent routing of both data and NACKs for multi-path redundancy.

### Understanding Peer Weights

| Weight | Role | Behavior |
|--------|------|----------|
| **0** | Duplicate path | Always sends data (mirroring) |
| **1-999** | Load balanced | Standard weighted round-robin distribution |
| **1000** | Recovery agent | Special handling - only active when FSR is requested |

### 12. Modified NACK Peer Selection

**File:** `librist/src/rist-common.c` (Lines 1049-1163)

The original `send_nack_group` function selected the peer with lowest RTT for sending NACKs. The modified version prioritizes weight-1000 recovery agents.

**Original function preserved with:**
```c
#ifdef default_send_nack_group_function
// Original function (lines 1049-1089)
#endif
```

**New behavior (Lines 1091-1163):**

```c
static void send_nack_group(struct rist_receiver *ctx, struct rist_flow *f)
{
    struct rist_peer *recovery_agent = NULL;
    uint64_t recovery_agent_rtt = UINT64_MAX;

    // First pass: Look for weight-1000 recovery agents
    for (size_t i = 0; i < f->peer_lst_len; i++) {
        struct rist_peer *check = f->peer_lst[i];
        if (check->is_rtcp && !check->dead && check->config.weight == 1000) {
            // If multiple recovery agents exist, select the one with lowest RTT
            if (check->last_rtt < recovery_agent_rtt) {
                recovery_agent = check;
                recovery_agent_rtt = check->last_rtt;
            }
        }
    }

    // Store recovery agent globally for FSR to use
    g_current_recovery_agent = recovery_agent;

    // If we found a recovery agent, use it; otherwise fall back to standard logic
    if (recovery_agent != NULL) {
        peer = recovery_agent;
    } else {
        // Standard logic: select peer with lowest RTT
        ...
    }
}
```

**Key changes:**
1. **First pass** - Scans for weight-1000 peers specifically
2. **Lowest RTT among weight-1000** - If multiple recovery agents exist, picks the one with lowest latency
3. **Stores globally** - Saves the selected recovery agent for FSR operations
4. **Fallback** - If no weight-1000 peer found, uses standard lowest-RTT selection

---

### 13. Modified Data Send Balancing

**File:** `librist/src/udp.c` (Lines 845-1125)

The `rist_sender_send_data_balanced` function controls how data is distributed to peers. The modified version adds special handling for weight-1000 recovery agents.

**Original function preserved with:**
```c
#ifdef old_rist_sender_send_data_balanced_function
// Original function (lines 845-982)
#endif
```

**New function structure (Lines 988-1120):**

```c
// ========== REPLACE rist_sender_send_data_balanced FUNCTION ==========

void rist_sender_send_data_balanced(struct rist_sender *ctx, struct rist_buffer *buffer)
{
    for (peer = ctx->common.PEERS; peer; peer = peer->next) {

        if (peer->config.weight == 0 && !looped) {
            // Weight-0: Always send (duplicate/mirror mode)
            send_filtered_data_to_peer(peer, buffer, ...);
        }
        else if (peer->config.weight == 1000 && !looped) {
            // Weight-1000: Recovery agent - only send if FSR requested
            if (has_fsr_requests()) {
                // Only send to peers that have requested FSR
                if (peer_id_in_fsr_list(peer->adv_peer_id)) {
                    send_filtered_data_to_peer(peer, buffer, ...);
                }
            }
        }
        else {
            // Standard weighted load balancing
            // ... election logic ...
        }
    }
}

// ========== END OF FUNCTION REPLACEMENT ==========
```

**Weight behavior summary:**

| Weight | Data Sending Behavior |
|--------|----------------------|
| **0** | Always sends to peer (for path duplication) |
| **1000** | Only sends when `has_fsr_requests()` is true AND peer is in FSR list |
| **Other** | Standard weighted round-robin load balancing |

**Key changes:**
1. **Weight-0 unchanged** - Still always sends (duplicate mode)
2. **Weight-1000 conditional** - Only sends when:
   - `has_fsr_requests()` returns true (at least one FSR Enable received)
   - `peer_id_in_fsr_list(peer->adv_peer_id)` is true (this specific peer requested FSR)
3. **Uses filtered sending** - All paths now use `send_filtered_data_to_peer()` for program selection support

---

### Recovery Agent Architecture

```
                    ┌─────────────────┐
                    │   RIST Sender   │
                    │   (Headend)     │
                    └────────┬────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
              ▼              ▼              ▼
     ┌────────────┐  ┌────────────┐  ┌────────────┐
     │ Satellite  │  │ Satellite  │  │ Terrestrial│
     │  Peer #1   │  │  Peer #2   │  │   Peer     │
     │ weight=0   │  │ weight=0   │  │ weight=1000│
     │ (always tx)│  │ (always tx)│  │ (on-demand)│
     └─────┬──────┘  └─────┬──────┘  └─────┬──────┘
           │               │               │
           │   Always On   │   Recovery    │
           │   Full Stream │   Agent Path  │
           ▼               ▼               ▼
     ┌─────────────────────────────────────────┐
     │              RIST Receiver              │
     │  - Monitors satellite peer health       │
     │  - Sends NACKs to weight-1000 peer      │
     │  - Sends FSR Enable when satellite down │
     └─────────────────────────────────────────┘
```

**Workflow:**
1. Normal operation: Data always flows via satellite peers (weight=0), NACKs go to weight-1000 peer
2. Satellite degradation detected: Receiver sends FSR Enable to sender
3. Sender adds receiver to FSR list, starts sending full stream via weight-1000 path
4. Satellite recovers: Receiver sends FSR Disable, sender stops weight-1000 transmission

---

### 14. FSR Message Handler (Sender Side)

**File:** `librist/src/rist-common.c` (Lines 2366-2420)

When the sender receives an FSR Enable or Disable message from a receiver, it updates the FSR peer list:

```c
else if (subtype == FSR_SUBTYPE_ENABLE || subtype == FSR_SUBTYPE_DISABLE) {
    /* RIST Part 7 Full Stream Request (FSR) signaling */

    // Validate: packet size >= 12 bytes
    // Validate: name field == "RIST"
    // Validate: sender side only (!peer->receiver_mode)
    // Validate: peer is authenticated

    /* Handle FSR Enable/Disable */
    if (subtype == FSR_SUBTYPE_ENABLE) {
        if (add_to_fsr_list(peer->adv_peer_id) == 0) {
            rist_log_priv(ctx, RIST_LOG_INFO,
                "FSR ENABLE: Added peer %u to full stream list\n",
                peer->adv_peer_id);
        }
    }
    else { /* FSR_SUBTYPE_DISABLE */
        if (remove_from_fsr_list(peer->adv_peer_id) == 0) {
            rist_log_priv(ctx, RIST_LOG_INFO,
                "FSR DISABLE: Removed peer %u from full stream list\n",
                peer->adv_peer_id);
        }
    }
}
```

**Validation steps before processing:**
1. Packet size minimum 12 bytes
2. Name field must be "RIST" (4 bytes)
3. Must be on sender side (not `receiver_mode`)
4. Peer must be authenticated

**Actions:**
- FSR Enable → `add_to_fsr_list(peer->adv_peer_id)` - adds peer to receive full stream from weight-1000 path
- FSR Disable → `remove_from_fsr_list(peer->adv_peer_id)` - removes peer from full stream list

---

## Custom Tools

### ristreceiver_with_markers

RIST receiver that converts RIST RTP metadata into VSF TR-06-4 Part 7 markers for multi-path redundancy validation.

**Purpose:**
This program sits at the output of a regular RIST sender. The sender is configured with listening peers including one with **weight=0** (always sending full stream). Our program:
1. Connects to the RIST sender as a receiver
2. Receives data blocks containing RTP metadata (sequence numbers, SSRC, flow_id)
3. Converts RIST RTP metadata into custom Part 7 markers
4. Strips RIST headers, outputting raw MPEG-TS
5. Muxes Part 7 markers into the TS stream for downstream validation

**Architecture:**
```
┌─────────────────┐          ┌──────────────────────────┐          ┌────────────────┐
│   RIST Sender   │  weight=0│  ristreceiver_with_      │          │   Downstream   │
│  (Headend)      │─────────▶│      markers             │─────────▶│   Upload/      │
│                 │  Full    │                          │  Raw TS  │   Processing   │
│ Listening peer  │  Stream  │ - Extracts RTP metadata  │  + Part 7│                │
│ @:5554          │          │ - Creates Part 7 markers │  Markers │                │
└─────────────────┘          │ - Strips RIST headers    │          └────────────────┘
                             │ - Muxes markers into TS  │
                             └──────────────────────────┘
```

**Usage:**
```bash
./ristreceiver_with_markers -i rist://192.168.110.107:5554 -o udp://239.6.6.6:6000
```

**How It Works:**

1. **RIST Connection** - Creates a RIST receiver context and connects to the sender:
   - Uses Simple Profile for reliability
   - Configures RIST logging
   - Establishes peer connection to sender's listening port

2. **Data Block Processing** - For each received RIST data block:
   - Extracts RTP metadata: `seq`, `flow_id`, `ts_ntp`
   - Uses `flow_id` as the SSRC (source identifier)
   - Data arrives already stripped of RTP headers (raw TS payload)

3. **Packet Counting** - Analyzes each 188-byte TS packet in the block:
   - Counts **null packets** (PID = 0x1FFF) - stuffing/padding
   - Counts **non-null packets** (PID ≠ 0x1FFF) - actual content
   - Accumulates counts over 5 RTP payloads

4. **Part 7 Marker Generation** - Every 5 RTP payloads, creates a marker containing:
   - Summary of the preceding 5 RTP blocks
   - Sequence tracking for loss detection
   - Packet statistics for validation

**Constants:**
| Constant | Value | Description |
|----------|-------|-------------|
| `RTP_PAYLOADS_PER_MARKER` | 5 | Generate marker after every 5 RTP payloads |
| `MARKER_PID` | 0x1FF0 | PID for Part 7 marker TS packets |
| `MARKER_TABLE_ID` | 0xBF | Private section table_id per MPEG-TS |
| `TS_PACKET_SIZE` | 188 | Standard TS packet size |
| `UDP_PAYLOAD_SIZE` | 1316 | 7 TS packets per UDP (7 × 188) |
| `UDP_PACKETS_PER_BLOCK` | 7 | Typical TS packets per RTP payload |

**Part 7 Marker Format (PID 0x1FF0, 188 bytes):**
```
Offset  Field                      Size    Description
──────────────────────────────────────────────────────────────
0       sync_byte                  1       0x47 (TS sync)
1-2     PID (0x1FF0)              13 bits Marker PID
        transport_priority         1 bit
        payload_unit_start         1 bit   Set to 1
        transport_error            1 bit
3       continuity_counter         4 bits  Incremented each marker
        adaptation_field_control   2 bits
        scrambling_control         2 bits
4       pointer_field              1       0x00
5       table_id                   1       0xBF (private section)
6-7     section_length             12 bits Length of remaining data
8-11    marker_sequence_number     4       32-bit marker sequence
12-13   non_null_count             2       16-bit count of non-null packets
14-15   null_count                 2       16-bit count of null packets
16-17   rtp_sequence_start_msb     2       Upper 16 bits of start seq
18-19   rtp_sequence_start_lsb     2       Lower 16 bits of start seq
20-21   rtp_sequence_next_msb      2       Upper 16 bits of next seq
22-23   rtp_sequence_next_lsb      2       Lower 16 bits of next seq
24-27   source_ssrc                4       32-bit SSRC (flow_id)
28-31   CRC-32                     4       ISO/IEC 13818-1 CRC
32-187  padding                    156     0xFF stuffing bytes
```

**UDP Alignment:**
- Output maintains strict 1316-byte UDP packet alignment (7 TS packets)
- Uses ring buffer to accumulate TS packets
- Flushes to output when buffer reaches 7 packets
- Markers are inserted into the buffer like regular TS packets
- Ensures downstream receivers see standard TS-over-UDP format

**Processing Flow:**
```
┌─────────────────────────────────────────────────────────────┐
│ For each RIST data block received:                          │
│                                                             │
│  1. Extract RTP metadata (seq, flow_id/SSRC)               │
│  2. Count null/non-null TS packets in payload              │
│  3. Accumulate counts for marker                           │
│  4. Send raw TS packets to output buffer                   │
│  5. Increment rtp_payload_count                            │
│                                                             │
│  If rtp_payload_count >= 5:                                │
│    - Create Part 7 marker packet                           │
│    - Insert marker into output buffer                      │
│    - Reset counters                                        │
│    - Increment marker_sequence                             │
│                                                             │
│  When buffer has 7 packets:                                │
│    - Send 1316-byte UDP to output destination              │
└─────────────────────────────────────────────────────────────┘
```

**Key Functions:**
| Function | Description |
|----------|-------------|
| `compute_crc32()` | Calculates ISO/IEC 13818-1 CRC-32 for marker validation |
| `create_part7_marker()` | Builds 188-byte marker packet with accumulated stats |
| `send_ts_packet()` | Adds packet to ring buffer, flushes when full |
| `cb_recv()` | RIST receive callback - processes each data block |
| `init_udp_socket()` | Creates output UDP socket for raw TS |

**Output Format:**
- Raw MPEG-TS stream (no RTP/RIST headers)
- Part 7 markers muxed in every 5 original RTP payloads
- Standard 7-packet UDP alignment (compatible with standard receivers)
- Markers on PID 0x1FF0 can be extracted by downstream validators

---

### ristsender_marker

VSF TR-06-4 Part 7 marker-aware sender with circuit breaker protection. This is the critical validation and re-encapsulation stage that bridges the satellite path to the RIST recovery system.

**Purpose:**
This program receives the raw TS stream from satellite (which now contains Part 7 markers from `ristreceiver_with_markers`). It validates the integrity of the stream and re-encapsulates it into RIST for downstream delivery.

**How It Works:**

1. **Marker Detection & CRC32 Validation**
   - Receives raw TS from satellite (PID 0x1FF0 markers muxed in)
   - Parses each marker and validates CRC-32 (ISO/IEC 13818-1)
   - If CRC fails, marker is untrusted and block is dropped

2. **Packet Count Validation**
   - Counts null packets (PID 0x1FFF) and non-null packets between markers
   - Compares counted values against marker's `non_null_count` and `null_count`
   - If counts don't match → packet loss detected → block dropped

3. **RIST Re-encapsulation with Original Sequence Numbers**
   - If validation passes, forwards the TS data via RIST
   - Uses the **RTP sequence number from the marker** (`rtp_sequence_start`)
   - This preserves end-to-end sequence continuity

4. **Gap Creation for NACK-based Recovery**
   - When blocks are dropped (CRC fail or count mismatch), a **sequence gap** is created
   - The downstream RIST receiver detects this gap
   - Receiver sends NACKs via the **weight-1000 peer** (IP path) to request missing packets
   - This enables satellite loss to be recovered via terrestrial backup

**Architecture in System:**
```
┌───────────────┐     ┌──────────────────┐     ┌──────────────────┐     ┌──────────────┐
│  Satellite    │     │ ristsender_      │     │  Regular RIST    │     │   Output     │
│  Receiver     │────▶│    marker        │────▶│    Receiver      │────▶│   (STB)      │
│               │ Raw │                  │RIST │                  │ Raw │              │
│ TS + Part 7   │ TS  │ - CRC32 validate │     │ - Detects gaps   │ TS  │ Mirror of    │
│   Markers     │     │ - Count validate │     │ - NACKs via IP   │     │  headend     │
└───────────────┘     │ - Drop bad blocks│     │ - timing-mode=1  │     │   stream     │
                      │ - Re-encapsulate │     └──────────────────┘     └──────────────┘
                      │   with marker seq│              │
                      └──────────────────┘              │ NACKs
                                                       ▼
                                              ┌──────────────────┐
                                              │  Weight-1000     │
                                              │  Recovery Peer   │
                                              │  (IP Path)       │
                                              └──────────────────┘
```

**Circuit Breaker Protection:**

The circuit breaker handles catastrophic failures (complete signal loss, excessive errors):

| State | Description |
|-------|-------------|
| **NORMAL** | Normal operation - validate and send blocks |
| **SHUTDOWN** | Catastrophic failure detected - discard all packets, wait for good marker |
| **RECOVERY** | Validating recovery - counting good blocks, printing restart messages |

**Circuit Breaker Triggers:**
- 10 consecutive marker losses
- 10 validation failures within 10 seconds

**Circuit Breaker Recovery:**
- Requires 10 consecutive good blocks over 10 seconds
- Prints "RESTART SENDER" messages during recovery (triggers watchdog)

**Validation Flow:**
```
For each TS packet received:
  │
  ├─ Is marker packet (PID 0x1FF0)?
  │   │
  │   ├─ YES: Parse marker
  │   │        │
  │   │        ├─ CRC32 valid?
  │   │        │   ├─ NO: Ignore marker, continue
  │   │        │   └─ YES: Validate buffered block
  │   │        │           │
  │   │        │           ├─ Counts match marker?
  │   │        │           │   ├─ NO: DROP BLOCK (creates gap)
  │   │        │           │   └─ YES: SEND via RIST (with marker's seq)
  │   │        │
  │   │        └─ Reset counters for next block
  │   │
  │   └─ NO: Buffer packet, increment null/non-null count
```

**Usage:**
```bash
./ristsender_marker -i udp://239.6.6.6:6000 -u "rist://@192.168.110.43:5554?buffer=8000"
```

---

### rist_watchdog

Process watchdog that monitors ristsender_marker and handles FSR (Full Stream Recovery) return issues.

**Purpose:**
When complete satellite failure occurs, the system triggers FSR via the weight-1000 IP path. The full stream is delivered via IP while satellite is monitored for recovery. However, **ristsender_marker cannot cleanly return from FSR** - it crashes or hangs when satellite recovers. The watchdog provides a workaround by automatically restarting the process.

**FSR Recovery Sequence:**
```
1. Satellite fails completely
   │
2. Excessive packet loss triggers circuit breaker
   │
3. Regular RIST receiver sends FSR Enable to sender (via weight-1000 peer)
   │
4. Sender starts full stream via IP path (weight-1000)
   │
5. ristsender_marker enters SHUTDOWN state, discarding satellite packets
   │
6. Satellite signal recovers
   │
7. ristsender_marker sees good markers, enters RECOVERY state
   │
8. After 10 good blocks over 10 seconds, prints "RESTART SENDER"
   │
9. Watchdog detects message, kills and restarts ristsender_marker
   │
10. Fresh ristsender_marker instance starts cleanly on recovered satellite
   │
11. Regular receiver sends FSR Disable, IP path stops
   │
12. System returns to normal satellite operation (hitless!)
```

**Key Behavior:**
- Monitors both stdout and stderr for "RESTART SENDER" string
- When detected, terminates child process and restarts after 2-second delay
- This provides a **hitless** recovery - no output disruption during FSR or return

**Usage:**
```bash
./rist_watchdog ./ristsender_marker -i udp://239.6.6.6:6000 -u "rist://@192.168.110.43:5554?buffer=8000"
```

**Features:**
- Passes through all command line arguments to monitored program
- Graceful shutdown on SIGINT/SIGTERM
- 2-second restart delay to avoid rapid cycling
- Statistics tracking (restart count, uptime)

---

### Downstream RIST Receiver Configuration

The regular RIST receiver that receives from ristsender_marker requires specific configuration:

**Required: timing-mode=1**
```bash
ristreceiver -i "rist://@192.168.110.43:5554?timing-mode=1" -o udp://output:port
```

| Timing Mode | Value | Description |
|-------------|-------|-------------|
| Source (default) | 0 | Uses RTP timestamps from source |
| **Arrival** | **1** | Uses local arrival time (REQUIRED) |
| RTC | 2 | Uses RTP/RTCP + NTP |

**Why timing-mode=1 is required:**
- ristsender_marker synthesizes timestamps when re-encapsulating
- Original satellite timestamps may be discontinuous or invalid after recovery
- Arrival time mode ensures proper playout timing regardless of source timestamps

---

## Building

### Prerequisites
- GCC or Clang
- Meson build system
- Ninja

### Build librist
```bash
cd librist
meson setup build
ninja -C build
```

### Build custom tools
```bash
gcc -o ristreceiver_with_markers ristreceiver_with_markers.c -lrist -lpthread
gcc -o ristsender_marker ristsender_marker.c -lrist -lpthread
gcc -o rist_watchdog rist_watchdog.c
```

---

## License

This project is based on librist which is licensed under BSD-2-Clause.
Custom tools are also licensed under BSD-2-Clause.

---

## References

- [VSF TR-06-4](https://www.videoservicesforum.org/) - RIST Technical Recommendations
- [librist](https://code.videolan.org/rist/librist) - Original librist project
