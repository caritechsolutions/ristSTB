# PID Selection in the FSR Retransmission Path — End-to-End Trace

Read-only analysis of the vendored librist fork under `librist/`. All
`file:line` references are to that tree.

## TL;DR — the most important correction

The task hypothesis was that **the PID selection is encoded in the NACK / FSR
RTCP message**. That is **not** how this code works.

- The **FSR (Full Stream Request) RTCP message carries no PID information at
  all** — it only toggles full-stream-recovery membership for a peer and
  carries a `flow_id`.
- The **actual PID selection travels out-of-band in the GRE _keepalive_** as a
  `contentSelection` JSON blob (VSF TR-06-4 Part 6), keyed per peer-id.
- Retransmissions are filtered on the **source side** using that
  per-peer selection that was learned from the keepalive, *not* from anything
  in the NACK.

So PID selection and retransmission requests are two independent control
channels that happen to converge at the sender's per-peer send function.

---

## 1. NACK / FSR construction — does the NACK encode PID selection?

**Correction: No.** The FSR RTCP packet has a fixed 12-byte body with no PID
fields. The PID selection is carried separately in the keepalive JSON.

The FSR packet struct (`librist/src/proto/rtp.h:265`):

```c
RIST_PACKED_STRUCT(rist_rtcp_fsr_pkt, {
    struct rist_rtcp_hdr rtcp;    // Standard RTCP header (8 bytes)
    uint8_t name[4];              // "RIST" (0x52495354) (4 bytes)
    // Total: 12 bytes
})
```

Writers only set a subtype flag + `flow_id` (ssrc), no PIDs
(`librist/src/proto/rtp.c:140`):

```c
void rist_rtcp_write_fsr_enable(uint8_t *buf, int *offset, const uint32_t flow_id) {
    fsr->rtcp.flags = (2 << 6) | FSR_SUBTYPE_ENABLE;   // subtype 5
    fsr->rtcp.ptype = PTYPE_NACK_CUSTOM;               // PT=204
    fsr->rtcp.ssrc  = htobe32(flow_id);
    memcpy(fsr->name, "RIST", 4);
```

Subtypes are just enable/disable (`librist/src/proto/rtp.h:115`):

```c
#define FSR_SUBTYPE_ENABLE  5   /* Full Stream Request Enable */
#define FSR_SUBTYPE_DISABLE 6   /* Full Stream Request Disable */
```

The `fsr_peers` / `add_to_fsr_list` machinery in `udp.c` is a global list of
**peer IDs** (not PIDs) that have requested full-stream recovery
(`librist/src/udp.c:36`):

```c
static uint32_t *fsr_peers = NULL;       // list of peer_ids, not PIDs
static int fsr_peer_count = 0;
RIST_PRIV int add_to_fsr_list(uint32_t peer_id) { ... }
```

**Where the PID selection actually rides:** the GRE keepalive appends the
`contentSelection` JSON after the fixed keepalive struct
(`librist/src/proto/gre.c:220`):

```c
/* VSF TR-06-4 Part 6: Include contentSelection JSON if set */
if (p->content_selection_json && p->content_selection_json_len > 0) {
    memcpy(buf, &ka, sizeof(ka));
    memcpy(buf + sizeof(ka), p->content_selection_json, p->content_selection_json_len);
    _librist_proto_gre_send_data(p, 0, RIST_GRE_PROTOCOL_TYPE_KEEPALIVE, buf, total_len, ...);
```

The selected PIDs/programs are stored per peer in
`struct peer_program_selection` (`librist/src/program-selection.h:18`):

```c
struct peer_program_selection {
    uint32_t peer_id;
    bool has_selection;
    uint16_t *requested_programs;   /* programs requested */
    uint16_t *requested_pids;       /* PIDs requested */
    uint16_t *blocked_pids;         /* PIDs blocked */
    ...
};
```

---

## 2. Source-side filtering of the resend ("compress" non-selected PIDs)

When the sender resends a buffered packet, the retransmit path checks whether
the **target peer** has a program selection and, if so, routes the resend
through the same filtering function used for live data
(`librist/src/udp.c:1329`):

```c
// Apply PID filtering for NACK retransmissions if peer has program selection
struct rist_peer *target_peer = retry->peer->peer_data;
if (!skip_filtering && program_selection_peer_has_selection(target_peer->adv_peer_id)) {
    send_filtered_data_to_peer(target_peer, buffer, buffer->type, ...);  // filtered resend
} else {
    ret = rist_send_seq_rtcp(target_peer, buffer->seq_rtp, ...);          // unfiltered resend
}
```

`send_filtered_data_to_peer` filters then compresses in two stages
(`librist/src/udp.c:208` and `:226`):

```c
/* Apply PID filtering (replaces unwanted PIDs with NULL packets) */
filter_result = filter_and_compress_for_peer(data_start, buffer->size,
                    temp_payload, &output_len, buffer->size, target_peer->adv_peer_id);
...
/* Apply NPD to remove NULL packets we created during filtering */
int suppressed = suppress_null_packets(filtered_data, (uint8_t*)(hdr_ext + 1), &npd_len, hdr_ext);
```

Stage 1 — **filter**: unwanted PIDs are overwritten in place with standard
NULL packets (PID `0x1FFF`) (`librist/src/program-selection.c:911`):

```c
if (!should_include_pid(pid, selection)) {
    packet[1] = 0x1F;   /* PID high 5 bits = 0x1F */
    packet[2] = 0xFF;   /* PID low 8 bits  -> PID = 0x1FFF */
    packet[3] = 0x10;   /* AFC=payload only, CC=0 */
    memset(&packet[4], 0xFF, 184);
}
```

Stage 2 — **compress + signal positions**: `suppress_null_packets` removes the
`0x1FFF` packets and records *which* of the (≤7) TS packets in the payload were
elided in the `npd_bits` bitmask of the RTP extension header
(`librist/src/mpegts.c:31`):

```c
for (int i = 0; i <= (int)count-1; i++) {
    if (be16toh(hdr->flags1) == 0x1FFF) {
        *payload_len -= packet_size;
        SET_BIT(header_ext->npd_bits, (6 - i));   // mark elided slot i
        suppressed++;
    }
}
```

`npd_bits` bit 7 distinguishes 188- vs 204-byte packets; bits 6..0 are the
per-slot "this slot was a NULL that I removed" flags. This is the only
information the receiver gets about elided positions — the resend is then sent
as `RIST_PAYLOAD_TYPE_DATA_RAW_RTP_EXT`.

> Note: this is standard librist NPD reused for PID elision — the unwanted PIDs
> are turned into NULLs specifically so the existing NPD machinery compresses
> them away.

---

## 3. Receiver-side reinsertion (null the elided slots, pass selected PIDs)

The receiver detects the `"RI"` RTP extension header and, if the NPD flag is
set, expands the payload back to full size, reinserting NULL packets in the
slots flagged in `npd_bits` (`librist/src/rist-common.c:3097`):

```c
if (memcmp(&hdr_ext->identifier, "RI", 2) == 0 && be16toh(hdr_ext->length) == 1) {
    if (CHECK_BIT(hdr_ext->flags, 7)) {
        if (expand_null_packets(data_payload, data_payload_out, &payload.size, hdr_ext->npd_bits))
            payload.data = (void *)data_payload_out;
    }
}
```

`expand_null_packets` writes a fresh `0x1FFF` NULL packet into every flagged
slot and copies the surviving (selected-PID) packets into the remaining slots
(`librist/src/mpegts.c:75`):

```c
for (int i = 0; i <= (int)ts_count-1; i++) {
    if (CHECK_BIT(npd_bits, (6 - i)) == 0) {
        memcpy(&payload_out[offset], &payload_in[input_offset], packet_size);  // selected PID passes through
    } else {
        hdr->syncbyte = 0x47;
        hdr->flags1   = htobe16(0x1FFF);                                        // reinserted NULL
        memset(&payload_out[offset + sizeof(*hdr)], 0xff, packet_size - sizeof(*hdr));
    }
}
```

**PCR caveat at this layer:** reinsertion restores *packet count and positions*
(so PCR-interval spacing is preserved), but the elided packets come back as
**plain NULLs** — there is no per-packet PCR-rewrite/restamping here. PCR
integrity therefore depends on the *selection* keeping the real PCR PID (see §5),
not on reinsertion.

---

## 4. Dynamic update at runtime (channel change)

There are two ways the per-peer selection changes at runtime; neither touches
the NACK/FSR path.

1. **Control API** on the receiver — `rist_receiver_set_content_selection`
   updates the JSON for a specific peer (or all peers) under lock
   (`librist/src/rist.c:256`):

   ```c
   int rist_receiver_set_content_selection(struct rist_ctx *rist_ctx,
                                           struct rist_peer *peer,
                                           const char *json_str) {
       ...
       peer->content_selection_json = strdup(json_str);
       peer->content_selection_json_len = strlen(json_str);
   ```

   That JSON is then advertised upstream on the **next keepalive** (§1,
   `gre.c:220`) — so a channel change is just "set new JSON, keepalive carries
   it."

2. **Sender ingest of the keepalive** — every keepalive is re-parsed and the
   selection is refreshed live via `program_selection_add_peer`, deliberately
   outside the capability-change guard so it updates on *every* keepalive
   (`librist/src/rist-common.c:3005`):

   ```c
   /* ...must be outside the capability-change check so JSON is processed
    * on every keepalive, not just when capabilities change */
   if (info.json_len > 0 && info.json) {
       cJSON *content_selection = cJSON_GetObjectItem(json, "contentSelection");
       if (content_selection && cJSON_IsArray(content_selection)) {
           program_selection_add_peer(p->adv_peer_id, content_str);   // live update
   ```

   Because both live sends and retransmits look up `adv_peer_id` at send time
   (§2), a channel change takes effect on the next keepalive with no session
   teardown.

---

## 5. PCR integrity — is the PCR PID guaranteed to stay selected?

**It depends on selection style; there is no unconditional PCR guarantee.**

- **Program-based selection** (`requestedPrograms`): yes, the PCR PID is kept.
  `should_include_pid` → `pid_belongs_to_program` treats the PCR PID as a member
  of the program (`librist/src/program-selection.c:375`):

  ```c
  /* Check PCR PID */
  if (prog->pcr_pid == pid && prog->pcr_pid != 0x1FFF) {
      return true;
  }
  ```

  The PCR PID is learned from the PMT during parsing
  (`librist/src/program-selection.c:820`):

  ```c
  /* Get PCR PID */
  prog->pcr_pid = ((section[8] & 0x1F) << 8) | section[9];
  ```

- **Explicit PID-list selection** (`requestedPids`): **no automatic PCR
  preservation.** `should_include_pid` keeps a PID only if it is literally in
  the requested list and otherwise defaults to exclude
  (`librist/src/program-selection.c:466`, `:517`):

  ```c
  if (selection->requested_pids) {
      for (int i = 0; selection->requested_pids[i] != 0; i++)
          if (selection->requested_pids[i] == pid) return true;
  }
  ...
  if (selection->requested_pids) {
      /* If specific PIDs requested, default to exclude others */
      return false;
  }
  ```

  So when selecting by raw PID, the caller must include the PCR PID explicitly;
  the filter will null it otherwise. PSI/SI (`pid <= 0x1F`), PMT PIDs, and EMM
  PIDs are always kept (`program-selection.c:447–457`), but the PCR PID is not
  on that always-keep list.

- At the receiver, reinsertion (§3) restores packet timing slots but not PCR
  values, reinforcing that PCR correctness is a *selection-time* property.

---

## 6. NACK routing / topology implied by the code

Retransmission NACKs go to the **immediate RIST sender** (the peer that the
receiver is connected to) — standard librist NACK handling. What is special
here is the **full-stream-recovery overlay** the FSR signaling builds:

- A node can be a **weight-1000 "recovery agent"** peer. When such a peer is
  seen, the sender enables "satellite mode" (`librist/src/udp.c:979`):

  ```c
  } else if (peer->config.weight == 1000 && !looped) {
      if (!is_satellite_mode()) enable_satellite_mode();
      if (has_fsr_requests()) { ... }   // only stream to FSR-listed peers
  ```

- In satellite mode, **weight-0 (satellite) peers receive the full,
  unfiltered stream** while **weight-1000 (recovery) peers obey the PID
  selection** (`librist/src/udp.c:173`):

  ```c
  //   - Weight 0 (satellite) peers: Send full stream - IGNORE PID selection
  //   - Weight 1000 (recovery) peers: Apply PID filtering - OBEY PID selection
  bool skip_filtering = (is_satellite_mode() && target_peer->config.weight == 0);
  ```

- FSR enable/disable is driven by a control loop on the sender that watches for
  a recovery agent + satellite peer and toggles membership
  (`librist/src/rist-common.c:4864`, `:4886`), sending the FSR RTCP to the
  recovery agent (`rist_send_fsr_enable`, `librist/src/udp.c:1523`). The
  receiver side of FSR only adds/removes the peer id — again, **no PIDs**
  (`librist/src/rist-common.c:2397`):

  ```c
  if (subtype == FSR_SUBTYPE_ENABLE) {
      if (add_to_fsr_list(peer->adv_peer_id) == 0) { ... }
  ```

**Implied topology:** a source feeds (a) a satellite/broadcast leg that gets the
**full** stream and (b) one or more **recovery agents** that get a
**PID-filtered** stream and act as the buffered retransmission source for
downstream receivers. The FSR signaling is how a recovery agent asks the source
to start/stop sending it the full stream so it can serve as the recovery
buffer; the per-PID selection that downstream receivers want is conveyed
independently via the keepalive `contentSelection` JSON. Retransmissions are
served from whichever sender holds the buffer for that receiver, and are
filtered to that receiver's selection at resend time (§2).

---

## Summary table

| # | Claim in task | Finding |
|---|---------------|---------|
| 1 | PID selection encoded in NACK/FSR | **Corrected** — FSR carries only subtype + `flow_id`; PID selection rides the GRE keepalive `contentSelection` JSON. |
| 2 | Source filters resend to selected PIDs | **Confirmed** — `udp.c:1333` → `filter_and_compress_for_peer` (null unwanted PIDs) → `suppress_null_packets` signals elided slots via `npd_bits`. |
| 3 | Receiver nulls `0x1FFF` and passes selected | **Confirmed** — `rist-common.c:3101` → `expand_null_packets` reinserts NULLs in flagged slots; restores positions, not PCR values. |
| 4 | Runtime selection update | **Confirmed** — `rist_receiver_set_content_selection` (`rist.c:256`) → keepalive → `program_selection_add_peer` on every keepalive (`rist-common.c:3020`). |
| 5 | PCR stays in selection | **Partly** — guaranteed for program-based selection (`program-selection.c:376`); **not** automatic for explicit `requestedPids`. |
| 6 | NACK routing / topology | NACKs go to the immediate sender; FSR overlay distinguishes full-stream satellite peers (weight 0) from PID-filtered recovery agents (weight 1000) that hold the recovery buffer. |
