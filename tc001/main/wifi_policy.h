#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// Plaintext is permitted only during the public-material handshake.
static inline bool tc001_allow_handshake(bool traffic_ready, bool authenticated) {
    return !traffic_ready && !authenticated;
}
static inline bool tc001_state_stale(int64_t now_ms, int64_t last_ms) {
    return last_ms <= 0 || now_ms - last_ms > 12000;
}
static inline bool tc001_host_valid(const char *s) {
    if(!s || !s[0]) return false;
    size_t n=0;
    for(;s[n];n++) {
        char c=s[n];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='.')) return false;
        if(n>=126) return false;
    }
    return s[0]!='.' && s[n-1]!='.';
}
