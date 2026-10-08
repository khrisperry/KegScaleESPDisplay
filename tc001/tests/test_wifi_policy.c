#include <assert.h>
#include <stdio.h>
#include "wifi_policy.h"
#include "wifi_frame.h"
int main(void) {
    assert(tc001_host_valid("192.168.1.50"));assert(tc001_host_valid("KegScale-1234.local"));
    assert(!tc001_host_valid("http://192.168.1.50/ws"));assert(!tc001_host_valid("scale:80"));
    assert(!tc001_host_valid("scale\".local"));assert(!tc001_host_valid(""));assert(!tc001_host_valid(".scale"));
    char large[130];memset(large,'a',129);large[129]=0;assert(!tc001_host_valid(large));
    assert(tc001_allow_handshake(false,false));
    assert(!tc001_allow_handshake(true,false));assert(!tc001_allow_handshake(false,true));assert(!tc001_allow_handshake(true,true));
    assert(tc001_state_stale(500,0));assert(!tc001_state_stale(13000,1000));assert(tc001_state_stale(13001,1000));
    tc001_frame_t f={0};
    assert(tc001_frame_append(&f,1,2,6,0,3,"abc")==0);
    assert(tc001_frame_append(&f,1,2,6,3,3,"def")==1);assert(f.kind==4 && f.len==6 && !memcmp(f.bytes,"abcdef",6));
    assert(tc001_frame_append(&f,1,2,6,0,3,"abc")==0);
    assert(tc001_frame_append(&f,1,2,6,4,2,"de")==-1);
    assert(tc001_frame_append(&f,1,2,6,3,4,"defg")==-1);
    assert(tc001_frame_append(&f,2,2,6,3,3,"def")==-1);
    assert(tc001_frame_append(&f,1,1,6,3,3,"def")==-1);
    assert(tc001_frame_append(&f,1,2,7,3,3,"def")==-1);
    assert(tc001_frame_append(&f,1,2,2073,0,1,"a")==-1);
    assert(tc001_frame_append(&f,1,2,6,-1,1,"a")==-1);
    assert(tc001_frame_append(&f,1,0,6,0,1,"a")==-1);
    puts("PASS: scale host validation, handshake gate, stale timeout, frame reassembly/bounds/generation");
}
