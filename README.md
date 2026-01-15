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

---

## Weight-Based Peer Routing

The system uses peer weights to control data flow and PID selection behavior:

| Weight | Role | Data Behavior | PID Selection |
|--------|------|---------------|---------------|
| **0** | Satellite path | Always sends full stream | **IGNORED** - full stream always |
| **1-999** | Load balanced | Standard weighted round-robin | Applied if peer has contentSelection |
| **1000** | Recovery agent | Only sends when FSR is requested | **APPLIED** - filters per contentSelection |

### Key Behaviors

**Weight 0 (Satellite):**
- Always sends data (mirroring mode)
- **Ignores PID selection** - always sends full transport stream
- Used for primary satellite distribution path

**Weight 1000 (Recovery Agent):**
- Data only sent when receiver has requested FSR Enable
- **Obeys PID selection** - applies filtering per receiver's contentSelection
- NACKs are prioritized to weight-1000 peers
- Used for terrestrial backup/recovery path

---

## Recovery Agent Architecture

```
                    ┌─────────────────┐
                    │   RIST Sender   │
                    │   (Headend)     │
                    └────────┬────────┘
                             │
              ┌──────────────┴──────────────┐
              │                             │
              ▼                             ▼
     ┌────────────────┐            ┌────────────────┐
     │   Satellite    │            │  Terrestrial   │
     │     Peer       │            │     Peer       │
     │   weight=0     │            │  weight=1000   │
     │  (always tx)   │            │  (on-demand)   │
     └───────┬────────┘            └───────┬────────┘
             │                             │
             │  Always On                  │  Recovery
             │  Full Stream                │  Agent Path
             │  (no filtering)             │  (PID filtered)
             ▼                             ▼
     ┌─────────────────────────────────────────┐
     │              RIST Receiver              │
     │  - Monitors satellite peer health       │
     │  - Sends NACKs to weight-1000 peer      │
     │  - Sends FSR Enable when satellite down │
     │  - Sends contentSelection for filtering │
     └─────────────────────────────────────────┘
```

### Workflow

1. **Normal operation**: Data flows via satellite (weight=0) with full stream
2. **Satellite degradation**: Receiver sends FSR Enable to sender
3. **Recovery active**: Sender sends PID-filtered stream via weight-1000 path
4. **Satellite recovers**: Receiver sends FSR Disable, weight-1000 transmission stops

---

## Program Selection (VSF TR-06-4 Part 6)

Receivers can request specific programs via contentSelection JSON in keepalive messages:

```json
{
  "contentSelection": [{
    "requestedPrograms": [1840, 2010],
    "blockedPrograms": [4, 5],
    "requestedPIDs": ["0x100", "0x200"],
    "blockedPIDs": ["0x300"]
  }]
}
```

### Always-Passed PIDs (per spec)

The following PIDs are always included regardless of contentSelection:
- **PAT** (0x0000) - Program Association Table
- **CAT** (0x0001) - Conditional Access Table
- **All PMT PIDs** - Program Map Tables (extracted from PAT)
- **All EMM PIDs** - Entitlement Management Messages (extracted from CAT)
- **PSI/SI range** (0x00-0x1F) - DVB mandatory tables

### How PID Filtering Works

1. Receiver sends contentSelection JSON via keepalive
2. Sender stores selection per-peer
3. For weight-1000 peers only: unwanted PIDs → NULL packets (0x1FFF)
4. NULL Packet Deletion (NPD) compresses stream by removing NULLs
5. Receiver reinstates NULLs to preserve PCR timing

**Important**: Do NOT use the `-n` flag when using program selection. NPD is applied automatically after filtering.

### CLI Usage

```bash
# Direct JSON string
ristreceiver -i rist://sender:5000 -o udp://127.0.0.1:6000 \
  -C '{"contentSelection":[{"requestedPrograms":[1840,2010]}]}'

# Load JSON from file
ristreceiver -i rist://sender:5000 -o udp://127.0.0.1:6000 \
  -C @/path/to/selection.json
```

---

## Full Stream Request (FSR) Protocol

### FSR Message Types

| Subtype | Name | Description |
|---------|------|-------------|
| 5 | FSR_ENABLE | Receiver requests full stream from recovery path |
| 6 | FSR_DISABLE | Receiver releases full stream request |

### FSR Triggers (Receiver Side)

**Enable triggers:**
- Satellite peer is dead (no heartbeat)
- Peer timeout (2+ seconds since last data)
- High RTT (>500ms indicating degraded link)

**Disable triggers:**
- Satellite peer recovers
- Link quality improves

---

## Custom Tools

### ristreceiver_with_markers

Receives RIST stream and inserts Part 7 markers for downstream validation.

```bash
./ristreceiver_with_markers -i rist://192.168.110.107:5554 -o udp://239.6.6.6:6000
```

**Purpose**: Creates metadata markers containing RTP sequence information every 5 payloads, enabling downstream validation of stream integrity through the satellite path.

### ristsender_marker

Validates Part 7 markers and re-encapsulates into RIST with circuit breaker protection.

```bash
./ristsender_marker -i udp://239.6.6.6:6000 -u "rist://@192.168.110.43:5554?buffer=8000"
```

**Validation steps:**
1. CRC-32 validation of markers
2. Packet count validation (null vs non-null)
3. Drops invalid blocks (creates sequence gaps for NACK recovery)

**Circuit breaker triggers:**
- 10 consecutive marker losses
- 10 validation failures within 10 seconds

### rist_watchdog

Process monitor for ristsender_marker with automatic restart.

```bash
./rist_watchdog ./ristsender_marker -i udp://239.6.6.6:6000 -u "rist://@192.168.110.43:5554?buffer=8000"
```

Monitors for "RESTART SENDER" output and automatically restarts the process for clean FSR recovery.

---

## Downstream Receiver Configuration

The RIST receiver connected to ristsender_marker requires:

```bash
ristreceiver -i "rist://@192.168.110.43:5554?timing-mode=1" -o udp://output:port
```

**timing-mode=1** (arrival time) is required because ristsender_marker synthesizes timestamps during re-encapsulation.

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

## Modifications to librist

### Summary of Changes

| File | Description |
|------|-------------|
| `src/proto/rtp.h` | FSR subtype definitions |
| `src/proto/rtp.c` | FSR packet writing functions |
| `src/udp.c` | FSR list management, weight-based data filtering |
| `src/rist-common.c` | FSR message processing, recovery logic |
| `src/rist.c` | Program selection initialization |
| `src/program-selection.h` | Program selection API header |
| `src/program-selection.c` | PID filtering, PAT/CAT parsing, NPD integration |

### Key Architectural Changes

| Feature | Original | Modified |
|---------|----------|----------|
| NACK Routing | Lowest RTT | Prioritize weight-1000 peers |
| Data Sending | Weight-based load balancing | Weight-1000 only when FSR requested |
| PID Selection | N/A | Weight-0 ignores, weight-1000 applies |

---

## License

This project is based on librist which is licensed under BSD-2-Clause.
Custom tools are also licensed under BSD-2-Clause.

---

## References

- [VSF TR-06-4](https://www.videoservicesforum.org/) - RIST Technical Recommendations
- [librist](https://code.videolan.org/rist/librist) - Original librist project
