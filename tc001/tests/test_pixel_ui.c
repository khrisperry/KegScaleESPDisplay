#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "pixel_ui.h"
static int same(pixel_t *a,pixel_t *b) { return !memcmp(a,b,sizeof(pixel_t)*PIXEL_COUNT); }
int main(int argc,char **argv) {
    pixel_t frame[256],other[256];
    for(unsigned layout=0;layout<3;layout++) {
        bool seen[256]={false};
        for(unsigned y=0;y<8;y++) for(unsigned x=0;x<32;x++) {
            unsigned i=pixel_wire_index(x,y,layout);assert(i<256 && !seen[i]);seen[i]=true;
        }
    }
    assert(pixel_wire_index(0,1,0)==63);
    assert(pixel_wire_index(8,0,1)==64);
    assert(pixel_wire_index(1,0,2)==15);
    pixel_state_t s={.servings=42,.percent=68,.gallons=3.4,.ready=true,.stable=true,.name="COTTAGE IPA"};
    pixel_render(frame,&s,0,2000);
    int filled=0;for(unsigned x=0;x<32;x++) if(frame[224+x].g==71) filled++;
    assert(filled==22);
    s.percent=10;pixel_render(frame,&s,0,2000);assert(frame[224].r==85 && frame[224].g==18);
    s.percent=150;pixel_render(frame,&s,0,2000);assert(frame[255].g==71);
    s.percent=NAN;s.gallons=INFINITY;pixel_render(frame,&s,2,2000);assert(frame[224].r==1);
    s.ready=false;pixel_render(frame,&s,0,0);pixel_message(other,"SETUP",0,0xff9f26);assert(same(frame,other));
    s.ready=true;s.stable=false;pixel_render(frame,&s,0,0);pixel_message(other,"SETTLE",0,0xff9f26);assert(same(frame,other));
    s.stable=true;s.demo=true;pixel_render(frame,&s,0,0);pixel_message(other,"DEMO",0,0x00cfff);assert(same(frame,other));
    pixel_message(frame,"COTTAGE IPA",0,0xffffff);pixel_message(other,"COTTAGE IPA",2000,0xffffff);assert(!same(frame,other));
    s=(pixel_state_t){.servings=29,.percent=30,.gallons=4.66,.ready=true,.stable=true,.name="ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEF"};
    assert(pixel_page_duration_ms(&s,0)==8000);
    assert(pixel_page_duration_ms(&s,3)==18300); // 191 px, 159 px travel, two end pauses
    pixel_render(frame,&s,3,0);pixel_render(other,&s,3,18000);assert(!same(frame,other));
    // At the end pause, the rightmost five glyphs plus the edge of the sixth
    // must match the same tail rendered at its own end pause.
    pixel_state_t tail=s;snprintf(tail.name,sizeof(tail.name),"ABCDEF");
    pixel_t end[PIXEL_COUNT];pixel_render(end,&tail,3,1500);
    assert(same(other,end));
    pixel_render(frame,&s,0,0);
    bool top=false,bottom=false;
    for(unsigned x=8;x<32;x++) { top|=frame[x].r!=0;bottom|=frame[6*32+x].r!=0; }
    assert(top && bottom); // numeric font occupies all seven rows
    if(argc>1) {
        FILE *f=fopen(argv[1],"w");assert(f);
        s=(pixel_state_t){.servings=42,.percent=68,.gallons=3.4,.ready=true,.stable=true,.name="COTTAGE IPA"};
        fprintf(f,"const frames = [");
        const char *messages[]={"MQTT WAIT","WIFI WAIT","NO DATA","SETTLE","SETUP","DEMO"};
        for(unsigned page=0;page<10;page++) {
            if(page) fprintf(f,",");
            fprintf(f,"[");
            unsigned ticks=page<4?pixel_page_duration_ms(&s,page)/100:80;
            for(unsigned t=0;t<ticks;t++) {
                if(t) fprintf(f,",");
                fprintf(f,"[");
                if(page<4) pixel_render(frame,&s,page,t*100);else pixel_message(frame,messages[page-4],t*100,page==6?0xff3838:0xffb52e);
                for(unsigned i=0;i<256;i++) { if(i) fprintf(f,",");fprintf(f,"%u",(frame[i].r<<16)|(frame[i].g<<8)|frame[i].b); }
                fprintf(f,"]");
            }
            fprintf(f,"]");
        }
        fprintf(f,"];\n");fclose(f);
    }
    puts("PASS: matrix mappings, gauge limits/colors, invalid numbers, setup, settling, demo, seven-row numbers, complete long-name scrolling");
}
