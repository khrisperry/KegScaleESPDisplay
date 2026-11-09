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
// The name page trades the level bar for two four-pixel-high text rows.
// Narrow I/L glyphs let "YUENGLING" fit across the 32-pixel width exactly.
typedef struct { char c; unsigned width; uint8_t rows[4]; } compact_glyph_t;
static const compact_glyph_t compact_font[] = {
    {'A', 3, {2,5,7,5}},
    {'B', 3, {6,7,5,7}},
    {'C', 3, {7,4,4,7}},
    {'D', 3, {6,5,5,6}},
    {'E', 3, {7,6,4,7}},
    {'F', 3, {7,6,4,4}},
    {'G', 3, {7,4,5,7}},
    {'H', 3, {5,7,5,5}},
    {'I', 1, {1,1,1,1}},
    {'J', 3, {1,1,5,7}},
    {'K', 3, {5,6,6,5}},
    {'L', 2, {2,2,2,3}},
    {'M', 5, {17,27,21,17}},
    {'N', 3, {5,7,7,5}},
    {'O', 3, {7,5,5,7}},
    {'P', 3, {6,5,6,4}},
    {'Q', 3, {7,5,7,1}},
    {'R', 3, {6,5,6,5}},
    {'S', 3, {3,4,1,6}},
    {'T', 3, {7,2,2,2}},
    {'U', 3, {5,5,5,7}},
    {'V', 3, {5,5,5,2}},
    {'W', 5, {17,21,27,17}},
    {'X', 3, {5,2,2,5}},
    {'Y', 3, {5,2,2,2}},
    {'Z', 3, {7,1,4,7}},
    {'0', 3, {7,5,5,7}},
    {'1', 1, {1,1,1,1}},
    {'2', 3, {6,1,2,7}},
    {'3', 3, {6,3,1,6}},
    {'4', 3, {5,7,1,1}},
    {'5', 3, {7,6,1,6}},
    {'6', 3, {4,6,5,7}},
    {'7', 3, {7,1,2,2}},
    {'8', 3, {7,7,5,7}},
    {'9', 3, {7,5,3,1}},
    {' ', 2, {0,0,0,0}},
    {'-', 3, {0,0,7,0}},
    {'.', 1, {0,0,0,1}},
    {'\'', 1, {1,1,0,0}},
    {'&', 3, {2,4,3,7}},
    {'/', 3, {1,2,2,4}},
    {'?', 3, {7,1,2,2}},
};
static const compact_glyph_t *compact_glyph(char c) {
    c=(char)toupper((unsigned char)c);
    for(unsigned i=0;i<sizeof(compact_font)/sizeof(compact_font[0]);i++)
        if(compact_font[i].c==c) return &compact_font[i];
    return &compact_font[sizeof(compact_font)/sizeof(compact_font[0])-1];
}
static unsigned compact_length(const char *s) {
    unsigned length=0;
    for(;*s;s++) length+=compact_glyph(*s)->width+1;
    return length?length-1:0;
}
unsigned pixel_name_lines(const char *name,char lines[2][33]) {
    char clean[33]={0};unsigned n=0;bool space=false;
    memset(lines,0,2*33);
    if(name) for(unsigned i=0;i<32 && name[i];i++) {
        if(isspace((unsigned char)name[i])) { space=n>0;continue; }
        if(space && n<32) clean[n++]=' ';
        if(n<32) clean[n++]=name[i];
        space=false;
    }
    if(!n) { memcpy(lines[0],"KEG",4);return 1; }
    if(compact_length(clean)<=32) { memcpy(lines[0],clean,n+1);return 1; }
    // Choose the word boundary with the smallest maximum line width.
    unsigned split=0,best=~0u,best_balance=~0u;
    for(unsigned i=1;i<n;i++) if(clean[i]==' ') {
        char first[33]={0};memcpy(first,clean,i);
        unsigned left=compact_length(first),right=compact_length(clean+i+1);
        unsigned maximum=left>right?left:right;
        unsigned balance=left>right?left-right:right-left;
        if(maximum<best || (maximum==best && balance<best_balance)) {
            split=i;best=maximum;best_balance=balance;
        }
    }
    if(!split) split=n/2; // Single long word: wrap without dropping characters.
    memcpy(lines[0],clean,split);
    unsigned second=split+(clean[split]==' ');
    memcpy(lines[1],clean+second,n-second);
    return 2;
}
static void compact_text(pixel_t *f,const char *s,int top,uint32_t ms,uint32_t color) {
    int x=text_left(compact_length(s),32,ms,0);
    for(;*s;s++) {
        const compact_glyph_t *g=compact_glyph(*s);
        for(unsigned y=0;y<4;y++) for(unsigned col=0;col<g->width;col++)
            if((g->rows[y]>>(g->width-1-col))&1) dot(f,x+(int)col,top+(int)y,color);
        x+=(int)g->width+1;
    }
}
static void name_page(pixel_t *f,const char *name,uint32_t ms) {
    char lines[2][33];unsigned count=pixel_name_lines(name,lines);
    compact_text(f,lines[0],count==1?2:0,ms,0xffecca);
    if(count==2) compact_text(f,lines[1],4,ms,0xffbf00);
}
uint32_t pixel_page_duration_ms(const pixel_state_t *s,unsigned page) {
    if(page%PIXEL_PAGES!=3) return 8000;
    char lines[2][33];pixel_name_lines(s->name,lines);
    unsigned a=compact_length(lines[0]),b=compact_length(lines[1]);
    uint32_t duration=scroll_cycle(a>b?a:b,32);
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
void pixel_battery(pixel_t f[PIXEL_COUNT],int percent,bool valid,uint32_t ms) {
    memset(f,0,sizeof(pixel_t)*PIXEL_COUNT);
    if(percent<0) percent=0;
    if(percent>100) percent=100;
    uint32_t color=!valid?0x7f91a8:percent<=15?0xff3838:percent<=30?0xffa020:0x32d680;
    for(int x=0;x<=6;x++) { dot(f,x,1,0x7f91a8);dot(f,x,5,0x7f91a8); }
    for(int y=2;y<=4;y++) { dot(f,0,y,0x7f91a8);dot(f,6,y,0x7f91a8);dot(f,7,y,0x7f91a8); }
    int fill=valid?(percent*5+99)/100:0;
    for(int y=2;y<=4;y++) for(int x=1;x<=fill;x++) dot(f,x,y,color);
    char value[12];if(valid)snprintf(value,sizeof(value),"%d%%",percent);else snprintf(value,sizeof(value),"--%%");
    large_text(f,value,8,24,ms,color);
    uint32_t bar=((((color>>16)&255)/3)<<16)|((((color>>8)&255)/3)<<8)|((color&255)/3);
    int pixels=valid?(percent*32+50)/100:0;
    for(int x=0;x<32;x++) dot(f,x,7,x<pixels?bar:0x010101);
}
void pixel_render(pixel_t f[PIXEL_COUNT],const pixel_state_t *s,unsigned page,uint32_t ms) {
    if(s->demo && ms<1500) { pixel_message(f,"DEMO",ms,0x00cfff); return; }
    if(page%PIXEL_PAGES==PIXEL_BATTERY_PAGE) { pixel_battery(f,0,false,ms);return; }
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
        // Straight-sided stein with a broad foam cap and a 2x2 open handle.
        // The amber body stays rectangular all the way down to its flat base.
        for(int x=1;x<=3;x++) dot(f,x,0,0xffecca);
        for(int x=0;x<=4;x++) dot(f,x,1,0xffecca);
        for(int y=2;y<=6;y++) for(int x=0;x<=4;x++) dot(f,x,y,0xffbf00);
        for(int y=2;y<=4;y++) dot(f,2,y,0xffdc70);
        for(int x=5;x<=7;x++) { dot(f,x,2,0xffecca);dot(f,x,5,0xffecca); }
        dot(f,7,3,0xffecca);dot(f,7,4,0xffecca);
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
    default: name_page(f,s->name,ms);return; // Both rows are text; no gauge on this page.
    }
    int fill=(int)lroundf(pct*32/100);
    // Keep the bar quieter than the readout and make its empty section nearly off.
    uint32_t bar=((((color>>16)&255)/3)<<16)|((((color>>8)&255)/3)<<8)|((color&255)/3);
    for(int x=0;x<32;x++) dot(f,x,7,x<fill?bar:0x010101);
}
