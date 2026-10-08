#include "pixel_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
typedef struct { char c; uint16_t bits; } glyph_t;
static const glyph_t font[] = {
    {'0', 0x7b6f},
    {'1', 0x2c97},
    {'2', 0x73e7},
    {'3', 0x73cf},
    {'4', 0x5bc9},
    {'5', 0x79cf},
    {'6', 0x79ef},
    {'7', 0x7292},
    {'8', 0x7bef},
    {'9', 0x7bcf},
    {'A', 0x2bed},
    {'B', 0x6bae},
    {'C', 0x7927},
    {'D', 0x6b6e},
    {'E', 0x79a7},
    {'F', 0x79a4},
    {'G', 0x796f},
    {'H', 0x5bed},
    {'I', 0x7497},
    {'J', 0x126f},
    {'K', 0x5bad},
    {'L', 0x4927},
    {'M', 0x5fed},
    {'N', 0x5ffd},
    {'O', 0x7b6f},
    {'P', 0x7be4},
    {'Q', 0x7b79},
    {'R', 0x6bad},
    {'S', 0x79cf},
    {'T', 0x7492},
    {'U', 0x5b6f},
    {'V', 0x5b6a},
    {'W', 0x5bfd},
    {'X', 0x5aad},
    {'Y', 0x5a92},
    {'Z', 0x72a7},
    {'%', 0x52a5},
    {'.', 0x0002},
    {'-', 0x01c0},
    {':', 0x0410},
    {'/', 0x12a4},
    {'?', 0x7282},
    {' ', 0x0000},
};
static void dot(pixel_t *f, int x, int y, uint32_t c) {
    if(x<0 || x>=32 || y<0 || y>=8) return;
    f[y*32+x]=(pixel_t){(c>>16)&255,(c>>8)&255,c&255};
}
static uint16_t glyph(char c) {
    c=(char)toupper((unsigned char)c);
    for(unsigned i=0;i<sizeof(font)/sizeof(font[0]);i++) if(font[i].c==c) return font[i].bits;
    return 0x7492; // unknown/non-ASCII byte: visible question mark
}
static void text(pixel_t *f, const char *s, int left, int width, uint32_t ms, uint32_t color) {
    int n=(int)strlen(s), length=n*4-1;
    if(n==0) return;
    int x=left+(width-length)/2;
    if(length>width) {
        // Pause at the left edge, then scroll; include a blank gap before wrap.
        unsigned step=ms/130;
        unsigned period=(unsigned)(length+width+8);
        unsigned pos=step%period;
        x=left-(int)(pos>8?pos-8:0);
    }
    for(int i=0;i<n;i++) {
        uint16_t g=glyph(s[i]);
        for(int y=0;y<5;y++) for(int col=0;col<3;col++)
            if((g>>(14-y*3-col))&1) {
                int px=x+i*4+col;
                if(px>=left && px<left+width) dot(f,px,y+1,color);
            }
    }
}
void pixel_message(pixel_t f[PIXEL_COUNT],const char *s,uint32_t ms,uint32_t color) {
    memset(f,0,sizeof(pixel_t)*PIXEL_COUNT); text(f,s,0,32,ms,color);
}
unsigned pixel_wire_index(unsigned x,unsigned y,unsigned layout) {
    if(layout==1) return (x/8)*64+y*8+x%8;
    if(layout==2) return x*8+((x&1)?7-y:y);
    return y*32+((y&1)?31-x:x);
}
void pixel_render(pixel_t f[PIXEL_COUNT],const pixel_state_t *s,unsigned page,uint32_t ms) {
    if(s->demo && ms<1500) { pixel_message(f,"DEMO",ms,0x00cfff); return; }
    if(!s->ready) { pixel_message(f,"SETUP",ms,0xff9f26); return; }
    if(!s->stable) { pixel_message(f,"SETTLE",ms,0xff9f26); return; }
    memset(f,0,sizeof(pixel_t)*PIXEL_COUNT);
    float pct=isfinite(s->percent)?s->percent:0;
    if(pct<0) pct=0;
    if(pct>100) pct=100;
    uint32_t color=pct<=10?0xff3838:(pct<=25?0xffa020:0x32d680);
    char value[48];
    switch(page%PIXEL_PAGES) {
    case 0:
        // Mug: white foam, amber body, white handle.
        for(int x=1;x<=5;x++) dot(f,x,1,0xffecca);
        for(int y=2;y<=5;y++) for(int x=1;x<=4;x++) dot(f,x,y,0xffb52e);
        dot(f,5,2,0xffecca);dot(f,6,2,0xffecca);dot(f,6,3,0xffecca);
        dot(f,6,4,0xffecca);dot(f,5,4,0xffecca);
        snprintf(value,sizeof(value),"%u",s->servings);
        text(f,value,8,24,ms,0xffecca); break;
    case 1:
        for(int y=1;y<=5;y++) { dot(f,1,y,0x7f91a8);dot(f,5,y,0x7f91a8); }
        for(int x=2;x<5;x++) { dot(f,x,1,0x7f91a8);dot(f,x,5,0x7f91a8); }
        for(int y=5;y>=2;y--) if(pct>=(5-y)*25+1)
            for(int x=2;x<5;x++) dot(f,x,y,color);
        snprintf(value,sizeof(value),"%.0f%%",(double)pct);
        text(f,value,8,24,ms,color); break;
    case 2:
        snprintf(value,sizeof(value),"%.2fG",(double)(isfinite(s->gallons)&&s->gallons>0?s->gallons:0));
        text(f,value,0,32,ms,0x48baff);break;
    default: text(f,s->name[0]?s->name:"KEG",0,32,ms,0xffecca);break;
    }
    int fill=(int)lroundf(pct*32/100);
    for(int x=0;x<32;x++) dot(f,x,7,x<fill?color:0x0c1520);
}
