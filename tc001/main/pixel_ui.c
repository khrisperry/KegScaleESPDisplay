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
    return 0x7282; // unknown/non-ASCII byte: visible question mark
}
// Five-by-seven type uses the full height above the single-row level gauge.
typedef struct { char c; uint8_t rows[7]; } large_glyph_t;
static const large_glyph_t large_font[] = {
    {'0', {14,17,19,21,25,17,14}},
    {'1', {4,12,4,4,4,4,14}},
    {'2', {14,17,1,2,4,8,31}},
    {'3', {30,1,1,14,1,1,30}},
    {'4', {2,6,10,18,31,2,2}},
    {'5', {31,16,16,30,1,1,30}},
    {'6', {14,16,16,30,17,17,14}},
    {'7', {31,1,2,4,8,8,8}},
    {'8', {14,17,17,14,17,17,14}},
    {'9', {14,17,17,15,1,1,14}},
    {'A', {14,17,17,31,17,17,17}},
    {'B', {30,17,17,30,17,17,30}},
    {'C', {14,17,16,16,16,17,14}},
    {'D', {30,17,17,17,17,17,30}},
    {'E', {31,16,16,30,16,16,31}},
    {'F', {31,16,16,30,16,16,16}},
    {'G', {14,17,16,23,17,17,15}},
    {'H', {17,17,17,31,17,17,17}},
    {'I', {14,4,4,4,4,4,14}},
    {'J', {7,2,2,2,2,18,12}},
    {'K', {17,18,20,24,20,18,17}},
    {'L', {16,16,16,16,16,16,31}},
    {'M', {17,27,21,21,17,17,17}},
    {'N', {17,25,21,19,17,17,17}},
    {'O', {14,17,17,17,17,17,14}},
    {'P', {30,17,17,30,16,16,16}},
    {'Q', {14,17,17,17,21,18,13}},
    {'R', {30,17,17,30,20,18,17}},
    {'S', {15,16,16,14,1,1,30}},
    {'T', {31,4,4,4,4,4,4}},
    {'U', {17,17,17,17,17,17,14}},
    {'V', {17,17,17,17,17,10,4}},
    {'W', {17,17,17,21,21,27,17}},
    {'X', {17,17,10,4,10,17,17}},
    {'Y', {17,17,10,4,4,4,4}},
    {'Z', {31,1,2,4,8,16,31}},
    {'%', {24,25,2,4,8,19,3}},
    {'.', {0,0,0,0,0,0,1}},
    {'-', {0,0,0,7,0,0,0}},
    {'?', {14,17,1,2,4,0,4}},
    {' ', {0,0,0,0,0,0,0}},
};
#define SCROLL_HOLD_MS 1200u
#define SCROLL_STEP_MS 100u
static unsigned large_width(char c) { return c=='.'?1u:(c==' '||c=='-'?3u:5u); }
static unsigned large_length(const char *s) {
    unsigned n=0;
    for(;*s;s++) n+=large_width(*s)+1;
    return n?n-1:0;
}
static uint32_t scroll_cycle(unsigned length, unsigned width) {
    return length>width?SCROLL_HOLD_MS*2+(length-width)*SCROLL_STEP_MS:8000;
}
static int text_left(unsigned length,unsigned width,uint32_t ms,int left) {
    if(length<=width) return left+(int)(width-length)/2;
    unsigned travel=length-width;
    uint32_t t=ms%scroll_cycle(length,width);
    unsigned pos=t<SCROLL_HOLD_MS?0:(t-SCROLL_HOLD_MS)/SCROLL_STEP_MS;
    if(pos>travel) pos=travel;
    return left-(int)pos;
}
static void large_text(pixel_t *f,const char *s,int left,unsigned width,uint32_t ms,uint32_t color) {
    int x=text_left(large_length(s),width,ms,left);
    for(;*s;s++) {
        char c=(char)toupper((unsigned char)*s);
        const large_glyph_t *g=NULL;
        for(unsigned i=0;i<sizeof(large_font)/sizeof(large_font[0]);i++) if(large_font[i].c==c) { g=&large_font[i];break; }
        if(!g) { c='?';g=&large_font[sizeof(large_font)/sizeof(large_font[0])-2]; }
        unsigned w=large_width(c);
        for(int y=0;y<7;y++) for(unsigned col=0;col<w;col++) {
            int px=x+(int)col;
            if(px>=left && px<left+(int)width && ((g->rows[y]>>(w-1-col))&1)) dot(f,px,y,color);
        }
        x+=(int)w+1;
    }
}
uint32_t pixel_page_duration_ms(const pixel_state_t *s,unsigned page) {
    if(page%PIXEL_PAGES!=3) return 8000;
    uint32_t duration=scroll_cycle(large_length(s->name),32);
    return duration<8000?8000:duration;
}
static void text(pixel_t *f, const char *s, int left, int width, uint32_t ms, uint32_t color) {
    int n=(int)strlen(s), length=n*4-1;
    if(n==0) return;
    int x=text_left((unsigned)length,(unsigned)width,ms,left);
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
        // A compact pint: broad foam head and a tapered, solid amber glass.
        // No handle or shaded patch that can resemble an extra letter.
        for(int x=1;x<=5;x++) dot(f,x,0,0xffecca);
        for(int x=0;x<=6;x++) dot(f,x,1,0xffecca);
        dot(f,0,2,0xffecca);dot(f,6,2,0xffecca);
        for(int x=1;x<=5;x++) dot(f,x,2,0xffbf00);
        for(int y=3;y<=5;y++) {
            dot(f,1,y,0xffecca);dot(f,5,y,0xffecca);
            for(int x=2;x<=4;x++) dot(f,x,y,0xffbf00);
        }
        for(int x=2;x<=4;x++) dot(f,x,6,0xffecca);
        snprintf(value,sizeof(value),"%u",s->servings);
        large_text(f,value,8,24,ms,0xffecca);break;
    case 1:
        for(int y=1;y<=5;y++) { dot(f,1,y,0x7f91a8);dot(f,6,y,0x7f91a8); }
        for(int x=2;x<=5;x++) { dot(f,x,0,0x7f91a8);dot(f,x,6,0x7f91a8); }
        for(int row=0;row<5;row++) if(pct>row*20)
            for(int x=2;x<=5;x++) dot(f,x,5-row,color);
        snprintf(value,sizeof(value),"%.0f%%",(double)pct);
        large_text(f,value,8,24,ms,color);break;
    case 2:
        snprintf(value,sizeof(value),"%.2fG",(double)(isfinite(s->gallons)&&s->gallons>0?s->gallons:0));
        large_text(f,value,0,32,ms,0x48baff);break;
    default: large_text(f,s->name[0]?s->name:"KEG",0,32,ms,0xffecca);break;
    }
    int fill=(int)lroundf(pct*32/100);
    // Keep the bar quieter than the readout and make its empty section nearly off.
    uint32_t bar=((((color>>16)&255)/3)<<16)|((((color>>8)&255)/3)<<8)|((color&255)/3);
    for(int x=0;x<32;x++) dot(f,x,7,x<fill?bar:0x010101);
}
