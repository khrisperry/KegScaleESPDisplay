#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define FR_STACK_DEPTH 12U
#define FR_INCIDENT_CAPACITY 12U
/* Static bounded recorder. Never pass secrets, URLs or request bodies as labels. */
typedef struct {
    uint32_t sequence, uptime_ms, free_internal, minimum_internal, largest_internal;
    uint32_t argument, duration_us;
    char label[32];
} fr_event_t;
typedef struct {
    uint32_t sequence, uptime_ms, size, caps, pointer, task, freed, pc[FR_STACK_DEPTH];
} fr_allocation_t;
/* Best-effort atomic snapshots; candidates can be overwritten until frozen. */
int fr_allocation_at(unsigned index, fr_allocation_t *out);
int fr_failed_allocation(fr_allocation_t *out);
unsigned fr_failure_count(void);
int fr_is_frozen(void);
esp_err_t fr_init(void);
void fr_event(const char *label, uint32_t argument, uint32_t duration_us);
/* Incidents have a separate ring: routine HTTP/health events cannot evict them. */
void fr_incident(const char *label, uint32_t argument, uint32_t duration_us);
uint32_t fr_latest_incident_sequence(void);
int fr_get_incident(uint32_t sequence, fr_event_t *out);
void fr_checkpoint(const char *label);
void fr_dump(void);
void fr_request_dump(void);
void fr_rearm(void);
uint32_t fr_latest_sequence(void);
int fr_get_event(uint32_t sequence, fr_event_t *out);
size_t fr_events(fr_event_t *out, size_t capacity, uint32_t *overwritten);
#ifdef __cplusplus
}
#endif
