#ifndef PROGRAM_SELECTION_H
#define PROGRAM_SELECTION_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Program selection configuration for a specific peer
 * 
 * Stores the PID and program filtering requirements for a single peer
 * based on VSF TR-06-4 Part 6 contentSelection specification.
 */
struct peer_program_selection {
    uint32_t peer_id;                          /* RIST peer ID */
    bool has_selection;                        /* True if peer has active program selection */
    
    /* Program selection arrays - NULL terminated */
    uint16_t *requested_programs;              /* Programs explicitly requested */
    uint16_t *blocked_programs;                /* Programs explicitly blocked */
    uint16_t *requested_pids;                  /* PIDs explicitly requested */
    uint16_t *blocked_pids;                    /* PIDs explicitly blocked */
    
    /* Flow identification for Main Profile */
    uint16_t udp_port;                         /* UDP port for flow identification */
    char *destination_ip;                      /* Destination IP (optional) */
    char *source_ip;                           /* Source IP (optional) */
    
    /* The raw JSON this selection was built from.
     *
     * Kept so an unchanged selection can be recognised without rebuilding it.
     * The selection arrives on EVERY RTCP keepalive -- once per second, per peer
     * -- and was previously re-parsed, re-printed and re-registered each time,
     * logging a state change on every receipt. */
    char *raw_json;

    /* Linked list management */
    struct peer_program_selection *next;       /* Next entry in list */
};

/**
 * @brief Initialize the program selection system
 * 
 * Sets up the global program selection storage and mutex.
 * Must be called before any other program selection functions.
 * 
 * @return 0 on success, -1 on failure
 */
int program_selection_init(void);

/**
 * @brief Add or update program selection for a peer
 * 
 * Parses the JSON contentSelection and stores the program/PID requirements
 * for the specified peer. If peer already exists, updates the selection.
 * 
 * @param peer_id RIST peer identifier
 * @param json_content JSON string containing contentSelection data
 * @return 0 on success, -1 on failure
 */
int program_selection_add_peer(uint32_t peer_id, const char *json_content);

/**
 * @brief Remove program selection for a peer
 * 
 * Removes the peer from program selection tracking and frees associated memory.
 * Safe to call even if peer doesn't exist.
 * 
 * @param peer_id RIST peer identifier
 * @return 0 on success, -1 if peer not found
 */
int program_selection_remove_peer(uint32_t peer_id);

/**
 * @brief Get program selection configuration for a peer
 * 
 * Returns pointer to the program selection structure for the specified peer.
 * Returns NULL if peer has no program selection configured.
 * 
 * @param peer_id RIST peer identifier
 * @return Pointer to peer_program_selection or NULL if not found
 */
struct peer_program_selection* program_selection_get_peer(uint32_t peer_id);

/**
 * @brief Check if a peer has active program selection
 * 
 * Quick check to determine if a peer has any program selection requirements.
 * 
 * @param peer_id RIST peer identifier
 * @return true if peer has program selection, false otherwise
 */
bool program_selection_peer_has_selection(uint32_t peer_id);

/**
 * @brief Get count of peers with active program selection
 * 
 * Returns the number of peers currently configured with program selection.
 * Useful for monitoring and debugging.
 * 
 * @return Number of peers with program selection
 */
int program_selection_get_peer_count(void);

/**
 * @brief Cleanup the program selection system
 * 
 * Frees all memory and destroys the mutex. Should be called during
 * sender context destruction.
 */
void program_selection_cleanup(void);

/**
 * @brief Dump program selection state for debugging
 * 
 * Logs the current program selection state for all peers.
 * Useful for debugging and monitoring.
 */
void program_selection_debug_dump(void);

/**
 * @brief Filter transport stream packets based on program selection
 * 
 * Processes a buffer of transport stream packets and filters out unwanted PIDs
 * by converting them to NULL packets (PID 0x1FFF).
 * 
 * @param ts_data Transport stream data buffer (must be multiple of 188 bytes)
 * @param ts_len Length of transport stream data
 * @param selection Program selection configuration
 * @return 0 on success, -1 on error
 */
int filter_transport_stream_pids(uint8_t *ts_data, size_t ts_len, 
                               const struct peer_program_selection *selection);

/**
 * @brief Apply NULL packet deletion to transport stream
 * 
 * @param input_data Input transport stream data
 * @param input_len Length of input data
 * @param output_data Output buffer for compressed data
 * @param output_len Pointer to output length
 * @param max_output_len Maximum output buffer size
 * @return 0 on success, -1 on error, >0 number of NULL packets removed
 */
int apply_null_packet_deletion(const uint8_t *input_data, size_t input_len,
                             uint8_t *output_data, size_t *output_len,
                             size_t max_output_len);

/**
 * @brief Complete PID filtering and NPD for a peer
 * 
 * @param input_data Input transport stream data
 * @param input_len Length of input data  
 * @param output_data Output buffer for filtered and compressed data
 * @param output_len Pointer to output length
 * @param max_output_len Maximum output buffer size
 * @param peer_id Peer ID for program selection lookup
 * @return 0 on success, -1 on error, >0 number of NULL packets removed
 */
/**
 * @brief Is the stored selection for this peer byte-identical to json_content?
 *
 * Returns false when the peer has no stored selection at all, so an unchanged
 * verdict always implies the selection is still registered -- a peer whose entry
 * was freed will be re-registered rather than skipped.
 */
bool program_selection_selection_unchanged(uint32_t peer_id, const char *json_content);

int filter_and_compress_for_peer(const uint8_t *input_data, size_t input_len,
                                uint8_t *output_data, size_t *output_len,
                                size_t max_output_len, uint32_t peer_id);

#ifdef __cplusplus
}
#endif

#endif /* PROGRAM_SELECTION_H */