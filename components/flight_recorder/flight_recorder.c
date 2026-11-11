#include "flight_recorder.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define EVENTS 24U
#define ALLOCS 24U
#define DEPTH 6U
#define CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
/* Hook fields are atomic: simultaneous allocations, frees and snapshots are
 * permitted. Hooks never log, allocate, write flash, or take a mutex/spinlock.
 * Only allocations >= 1 KiB are retained; this is a bounded candidate trace,
 * not a complete leak trace. Backtraces are decoded using the matching ELF. */
typedef struct {
    atomic_uint sequence, ms, size, caps, pointer, task, freed;
    atomic_uint pc[DEPTH];
} allocation_t;
static DRAM_ATTR allocation_t allocations[ALLOCS];
static DRAM_ATTR atomic_uint allocation_sequence;
static DRAM_ATTR atomic_bool armed, frozen, pending_dump;
static DRAM_ATTR atomic_uint failures;
static DRAM_ATTR atomic_bool failure_captured;
static DRAM_ATTR allocation_t failed;
static fr_event_t events[EVENTS];
static uint32_t event_sequence, event_count;
static portMUX_TYPE event_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t timer;
static uint32_t observed_min, last_health_ms, last_dump_ms;
static atomic_bool rearm_requested;
static const char *TAG = "flight_recorder";

static void capture(allocation_t *a, uint32_t seq, void *ptr, size_t size, uint32_t caps) {
    atomic_store(&a->sequence, 0);
    atomic_store(&a->ms, (uint32_t)(esp_timer_get_time() / 1000));
    atomic_store(&a->size, size);
    atomic_store(&a->caps, caps);
    atomic_store(&a->pointer, (uintptr_t)ptr);
    atomic_store(&a->task, (uintptr_t)xTaskGetCurrentTaskHandle());
    atomic_store(&a->freed, 0);
    esp_backtrace_frame_t frame = {0};
    esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);
    for (unsigned i = 0; i < DEPTH; ++i) {
        atomic_store(&a->pc[i], frame.pc);
        if (!esp_backtrace_get_next_frame(&frame)) {
            for (++i; i < DEPTH; ++i) atomic_store(&a->pc[i], 0);
            break;
        }
    }
    atomic_store(&a->sequence, seq);
}

void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps) {
    if (size < 1024 || !atomic_load(&armed) || atomic_load(&frozen) || xPortInIsrContext()) return;
    unsigned seq = atomic_fetch_add(&allocation_sequence, 1) + 1;
    capture(&allocations[(seq - 1) % ALLOCS], seq, ptr, size, caps);
}
void esp_heap_trace_free_hook(void *ptr) {
    if (!atomic_load(&armed) || atomic_load(&frozen) || xPortInIsrContext()) return;
    for (unsigned i = 0; i < ALLOCS; ++i)
        if (atomic_load(&allocations[i].pointer) == (uintptr_t)ptr)
            atomic_store(&allocations[i].freed, 1);
}
static void allocation_failed(size_t size, uint32_t caps, const char *function_name) {
    (void)function_name;
    atomic_fetch_add(&failures, 1);
    /* One frozen failure is retained; the count includes subsequent failures. */
    atomic_store(&frozen, true);
    if (!atomic_exchange(&failure_captured, true)) {
        if (!xPortInIsrContext()) capture(&failed, 1, NULL, size, caps);
        else {
            atomic_store(&failed.size, size);
            atomic_store(&failed.caps, caps);
            atomic_store(&failed.sequence, 1);
        }
        atomic_store(&pending_dump, true);
    }
}

void fr_event(const char *label, uint32_t argument, uint32_t duration_us) {
    fr_event_t e = {
        .uptime_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .free_internal = heap_caps_get_free_size(CAPS),
        .minimum_internal = heap_caps_get_minimum_free_size(CAPS),
        .largest_internal = heap_caps_get_largest_free_block(CAPS),
        .argument = argument, .duration_us = duration_us,
    };
    snprintf(e.label, sizeof(e.label), "%s", label);
    portENTER_CRITICAL(&event_lock);
    e.sequence = ++event_sequence;
    events[(e.sequence - 1) % EVENTS] = e;
    if (event_count < EVENTS) ++event_count;
    portEXIT_CRITICAL(&event_lock);
}
void fr_checkpoint(const char *label) { fr_event(label, 0, 0); }
size_t fr_events(fr_event_t *out, size_t capacity, uint32_t *overwritten) {
    portENTER_CRITICAL(&event_lock);
    size_t n = event_count < capacity ? event_count : capacity;
    if (overwritten) *overwritten = event_sequence - event_count;
    for (size_t i = 0; i < n; ++i) out[i] = events[(event_sequence - n + i) % EVENTS];
    portEXIT_CRITICAL(&event_lock);
    return n;
}
static int copy_allocation(allocation_t *a, fr_allocation_t *out) {
    unsigned seq = atomic_load(&a->sequence);
    if (!seq || seq == UINT32_MAX || !out) return 0;
    out->sequence=seq; out->uptime_ms=atomic_load(&a->ms);
    out->size=atomic_load(&a->size); out->caps=atomic_load(&a->caps);
    out->pointer=atomic_load(&a->pointer); out->task=atomic_load(&a->task);
    out->freed=atomic_load(&a->freed);
    for (unsigned i=0;i<DEPTH;++i) out->pc[i]=atomic_load(&a->pc[i]);
    return seq == atomic_load(&a->sequence);
}
int fr_allocation_at(unsigned index, fr_allocation_t *out) {
    return index < ALLOCS && copy_allocation(&allocations[index], out);
}
int fr_failed_allocation(fr_allocation_t *out) { return copy_allocation(&failed, out); }
unsigned fr_failure_count(void) { return atomic_load(&failures); }
int fr_is_frozen(void) { return atomic_load(&frozen); }
static void print_allocation(const char *kind, allocation_t *a) {
    unsigned seq = atomic_load(&a->sequence);
    if (!seq || seq == UINT32_MAX) return;
    uint32_t ms = atomic_load(&a->ms), size = atomic_load(&a->size), caps = atomic_load(&a->caps);
    uint32_t ptr = atomic_load(&a->pointer), task = atomic_load(&a->task), freed = atomic_load(&a->freed);
    uint32_t pc[DEPTH];
    for (unsigned i = 0; i < DEPTH; ++i) pc[i] = atomic_load(&a->pc[i]);
    if (seq != atomic_load(&a->sequence)) return;
    ESP_LOGW(TAG, "%s seq=%u ms=%lu size=%lu caps=0x%lx ptr=0x%lx task=0x%lx freed=%lu pc=0x%lx:0x%lx:0x%lx:0x%lx:0x%lx:0x%lx",
        kind, seq, (unsigned long)ms, (unsigned long)size, (unsigned long)caps,
        (unsigned long)ptr, (unsigned long)task, (unsigned long)freed,
        (unsigned long)pc[0], (unsigned long)pc[1], (unsigned long)pc[2],
        (unsigned long)pc[3], (unsigned long)pc[4], (unsigned long)pc[5]);
}
void fr_dump(void) {
    /* Copy one event at a time to avoid a multi-KiB timer-task stack frame. */
    uint32_t end, count;
    portENTER_CRITICAL(&event_lock);
    end = event_sequence; count = event_count;
    portEXIT_CRITICAL(&event_lock);
    ESP_LOGW(TAG, "DUMP events=%lu overwritten=%lu alloc_total=%u frozen=%u allocation_failures=%u",
        (unsigned long)count, (unsigned long)(end-count), atomic_load(&allocation_sequence),
        atomic_load(&frozen), atomic_load(&failures));
    for (uint32_t seq = end-count+1; seq <= end && count; ++seq) {
        fr_event_t e;
        portENTER_CRITICAL(&event_lock);
        e = events[(seq-1)%EVENTS];
        portEXIT_CRITICAL(&event_lock);
        if (e.sequence != seq) continue;
        ESP_LOGW(TAG, "EVENT seq=%lu ms=%lu label=%s arg=%lu duration_us=%lu free=%lu min=%lu largest=%lu",
            (unsigned long)e.sequence, (unsigned long)e.uptime_ms, e.label,
            (unsigned long)e.argument, (unsigned long)e.duration_us,
            (unsigned long)e.free_internal, (unsigned long)e.minimum_internal,
            (unsigned long)e.largest_internal);
    }
    print_allocation("FAILED_ALLOC", &failed);
    for (unsigned i=0; i<ALLOCS; ++i) print_allocation("ALLOC", &allocations[i]);
}
void fr_request_dump(void) { atomic_store(&pending_dump, true); }
void fr_rearm(void) { atomic_store(&rearm_requested, true); }
uint32_t fr_latest_sequence(void) {
    portENTER_CRITICAL(&event_lock);
    uint32_t n = event_sequence;
    portEXIT_CRITICAL(&event_lock);
    return n;
}
int fr_get_event(uint32_t seq, fr_event_t *out) {
    if (!seq || !out) return 0;
    portENTER_CRITICAL(&event_lock);
    *out = events[(seq-1)%EVENTS];
    portEXIT_CRITICAL(&event_lock);
    return out->sequence == seq;
}
static void tick(void *arg) {
    (void)arg;
    uint32_t ms = (uint32_t)(esp_timer_get_time()/1000);
    uint32_t minimum = heap_caps_get_minimum_free_size(CAPS);
    if (atomic_exchange(&rearm_requested, false)) {
        observed_min = minimum;
        atomic_store(&frozen, false);
        fr_checkpoint("trace_rearmed");
    }
    if (minimum < observed_min) {
        fr_event("new_heap_min", observed_min-minimum, 0);
        if (ms > 60000) {
            ESP_LOGW(TAG, "NEW_MIN noticed_ms=%lu previous=%lu min=%lu current=%u largest=%u; observation time is not allocation time",
                (unsigned long)ms, (unsigned long)observed_min, (unsigned long)minimum,
                (unsigned)heap_caps_get_free_size(CAPS), (unsigned)heap_caps_get_largest_free_block(CAPS));
            if (observed_min-minimum >= 1024) atomic_store(&pending_dump, true);
        }
        /* Freeze only a NEW post-startup severe dip, not an old boot watermark. */
        if (ms > 60000 && minimum < 8192) {
            atomic_store(&frozen, true);
            atomic_store(&pending_dump, true);
        }
        observed_min = minimum;
    }
    if (ms-last_health_ms >= 30000) {
        last_health_ms = ms;
        wifi_ap_record_t ap = {0};
        bool wifi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        ESP_LOGI(TAG, "HEALTH ms=%lu free=%u min=%u largest=%u alloc_failures=%u frozen=%u wifi=%u rssi=%d channel=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x",
            (unsigned long)ms, (unsigned)heap_caps_get_free_size(CAPS), (unsigned)minimum,
            (unsigned)heap_caps_get_largest_free_block(CAPS), atomic_load(&failures),
            atomic_load(&frozen), wifi, wifi ? ap.rssi : 0, wifi ? ap.primary : 0,
            ap.bssid[0],ap.bssid[1],ap.bssid[2],ap.bssid[3],ap.bssid[4],ap.bssid[5]);
    }
    if (atomic_load(&pending_dump) && ms-last_dump_ms >= 10000) {
        atomic_store(&pending_dump, false);
        last_dump_ms = ms;
        fr_dump();
    }
}
esp_err_t fr_init(void) {
    if (timer) return ESP_OK;
    observed_min = heap_caps_get_minimum_free_size(CAPS);
    esp_err_t e = heap_caps_register_failed_alloc_callback(allocation_failed);
    if (e != ESP_OK) return e;
    const esp_timer_create_args_t args = {.callback=tick, .name="flight_recorder", .skip_unhandled_events=true};
    e = esp_timer_create(&args, &timer);
    if (e != ESP_OK) return e;
    e = esp_timer_start_periodic(timer, 100000);
    if (e != ESP_OK) return e;
    atomic_store(&armed, true);
    fr_checkpoint("recorder_start");
    ESP_LOGI(TAG, "Static recorder storage=%u bytes", (unsigned)(sizeof(events)+sizeof(allocations)+sizeof(failed)));
    ESP_LOGI(TAG, "Diagnostic recorder: 100ms watermark observation; 30s health; 24 allocation candidates >=1024B; 24 events; RAM-only; matching ELF required");
    return ESP_OK;
}
