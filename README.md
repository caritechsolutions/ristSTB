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
| `src/udp.c` | Modified | Added FSR list management and data filtering |
| `src/rist-common.c` | Modified | Added FSR message processing and recovery logic |
| `src/rist.c` | Modified | Added program selection initialization |
| `src/program-selection.h` | **New** | Program selection API header |
| `src/program-selection.c` | **New** | Program selection implementation |

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

## Custom Tools

### ristreceiver_with_markers

RIST receiver with VSF TR-06-4 Part 7 marker insertion.

**Features:**
- Extracts RTP metadata from RIST data blocks
- Inserts metadata markers every 5 RTP payloads
- Maintains 1316-byte (7 TS packets) UDP packet alignment
- CRC-32 validation per ISO/IEC 13818-1

**Usage:**
```bash
./ristreceiver_with_markers -i rist://192.168.110.107:5554 -o udp://239.6.6.6:6000
```

**Marker Format (PID 0x1FF0):**
- table_id: 0xBF
- marker_sequence_number (32-bit)
- non_null_count (16-bit)
- null_count (16-bit)
- rtp_sequence_start (32-bit, MSB+LSB)
- rtp_sequence_next (32-bit, MSB+LSB)
- source_ssrc (32-bit)
- CRC-32

---

### ristsender_marker

VSF TR-06-4 Part 7 marker-aware sender with circuit breaker protection.

**Features:**
- Validates markers at BEGINNING of block per spec
- Detects lost markers via sequence tracking
- Circuit breaker protection:
  - Trigger: 10 consecutive marker losses
  - Trigger: 10 validation failures within 10 seconds
  - Recovery: 10 consecutive good blocks over 10 seconds
- Memory efficient (no buffering during recovery state)

**Circuit Breaker States:**
| State | Description |
|-------|-------------|
| NORMAL | Normal operation, sending blocks |
| SHUTDOWN | Catastrophic failure, discarding packets, waiting for good marker |
| RECOVERY | Validating recovery with good blocks |

**Usage:**
```bash
./ristsender_marker -i udp://239.6.6.6:6000 -u "rist://@192.168.110.43:5554?buffer=8000"
```

---

### rist_watchdog

Process watchdog that monitors ristsender_marker and automatically restarts it.

**Features:**
- Monitors stdout/stderr for "RESTART SENDER" trigger
- Graceful shutdown on SIGINT/SIGTERM
- Configurable restart delay (default: 2 seconds)
- Statistics tracking (restarts, uptime)

**Usage:**
```bash
./rist_watchdog ./ristsender_marker -i udp://239.6.6.6:6000 -u "rist://@192.168.110.43:5554?buffer=8000"
```

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
