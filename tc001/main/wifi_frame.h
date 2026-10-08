#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define TC001_MAX_FRAME 2072
typedef struct {
    uint32_t generation;
    int kind;
    size_t len, expected;
    uint8_t bytes[TC001_MAX_FRAME+1];
} tc001_frame_t;
// -1 invalid, 0 incomplete, 1 complete. Never copy out of bounds.
static inline int tc001_frame_append(tc001_frame_t *f,uint32_t generation,int opcode,
        int total,int offset,int length,const void *data) {
    if((opcode!=1 && opcode!=2) || total<=0 || total>TC001_MAX_FRAME ||
        offset<0 || length<=0 || !data) return -1;
    if(offset==0) { memset(f,0,sizeof(*f));f->generation=generation;f->kind=opcode==1?3:4;f->expected=(size_t)total; }
    if(f->generation!=generation || f->kind!=(opcode==1?3:4) || f->expected!=(size_t)total ||
        f->len!=(size_t)offset || f->len>f->expected || (size_t)length>f->expected-f->len) return -1;
    memcpy(f->bytes+f->len,data,(size_t)length);f->len+=(size_t)length;
    return f->len==f->expected?1:0;
}
