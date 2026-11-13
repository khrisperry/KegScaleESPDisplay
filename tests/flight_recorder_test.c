#include <assert.h>
#include <stdio.h>
#include "recorder_host/platform.h"
uint32_t test_free=60000, test_min=60000, test_largest=32000;
int64_t test_time;
bool test_isr;
#include "../components/flight_recorder/flight_recorder.c"
int main(void) {
    assert(fr_init()==ESP_OK);
    for (unsigned i=0;i<30;++i) fr_event("operation",i,0);
    fr_event_t snapshot[24]; uint32_t overwritten;
    assert(fr_events(snapshot,24,&overwritten)==24 && overwritten==7);
    assert(snapshot[0].argument==6 && snapshot[23].argument==29);
    fr_incident("ws_send_failure", 11, 123);
    for (unsigned i=0;i<100;++i) fr_event("http_poll", i, 0);
    fr_event_t incident;
    assert(fr_get_incident(1,&incident));
    assert(incident.argument==11 && incident.duration_us==123 && incident.free_internal==test_free);
    for (unsigned i=0;i<FR_INCIDENT_CAPACITY;++i) fr_incident("disconnect",i,0);
    assert(!fr_get_incident(1,&incident));
    assert(fr_get_incident(fr_latest_incident_sequence(),&incident));
    fr_event_t old;
    assert(!fr_get_event(1,&old));
    assert(fr_get_event(fr_latest_sequence(),&old));
    esp_heap_trace_alloc_hook((void *)0x1000,2048,3);
    assert(atomic_load(&allocation_sequence)==1);
    esp_heap_trace_free_hook((void *)0x1000);
    assert(atomic_load(&allocations[0].freed));
    fr_allocation_t exported;
    assert(fr_allocation_at(0,&exported) && exported.size==2048 && exported.freed);
    assert(exported.pc[0]==0x42000000-3);
    assert(exported.pc[11]==0x4200002c-3);
    assert(!fr_allocation_at(24,&exported));
    esp_heap_trace_alloc_hook((void *)0x2000,512,3);
    assert(atomic_load(&allocation_sequence)==1);
    test_isr=true;
    esp_heap_trace_alloc_hook((void *)0x2000,2048,3);
    assert(atomic_load(&allocation_sequence)==1);
    test_isr=false;
    test_time=10000000;test_min=7000;tick(NULL);
    assert(!atomic_load(&frozen)); /* startup low does not freeze */
    test_time=70000000;test_min=6000;tick(NULL);
    assert(atomic_load(&frozen)); /* recovered free=60000 still detects historical dip */
    unsigned before=atomic_load(&allocation_sequence);
    esp_heap_trace_alloc_hook((void *)0x3000,2048,3);
    assert(atomic_load(&allocation_sequence)==before);
    allocation_failed(4096,3,"malloc");
    assert(atomic_load(&failed.size)==4096 && atomic_load(&failures)==1);
    allocation_failed(8192,3,"malloc");
    assert(atomic_load(&failed.size)==4096 && atomic_load(&failures)==2);
    assert(fr_failed_allocation(&exported) && exported.size==4096);
    assert(fr_failure_count()==2 && fr_is_frozen());
    fr_rearm(); tick(NULL);
    assert(!atomic_load(&frozen));
    esp_heap_trace_alloc_hook((void *)0x4000,2048,3);
    assert(atomic_load(&allocation_sequence)==before+1);
    puts("PASS: recorder wrap, overwrite accounting, large allocation/free, ISR exclusion, startup exclusion, recovered transient dip, freeze, failure retention and rearm");
}
