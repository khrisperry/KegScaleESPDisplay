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
    fr_event_t old;
    assert(!fr_get_event(1,&old));
    assert(fr_get_event(fr_latest_sequence(),&old));
    esp_heap_trace_alloc_hook((void *)0x1000,2048,3);
    assert(atomic_load(&allocation_sequence)==1);
    esp_heap_trace_free_hook((void *)0x1000);
    assert(atomic_load(&allocations[0].freed));
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
    fr_rearm(); tick(NULL);
    assert(!atomic_load(&frozen));
    esp_heap_trace_alloc_hook((void *)0x4000,2048,3);
    assert(atomic_load(&allocation_sequence)==before+1);
    puts("PASS: recorder wrap, overwrite accounting, large allocation/free, ISR exclusion, startup exclusion, recovered transient dip, freeze, failure retention and rearm");
}
