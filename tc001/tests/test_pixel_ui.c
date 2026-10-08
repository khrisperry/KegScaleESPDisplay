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
    int filled=0;for(unsigned x=0;x<32;x++) if(frame[224+x].g==214) filled++;
    assert(filled==22);
    s.percent=10;pixel_render(frame,&s,0,2000);assert(frame[224].r==255 && frame[224].g==56);
    s.percent=150;pixel_render(frame,&s,0,2000);assert(frame[255].g==214);
    s.percent=NAN;s.gallons=INFINITY;pixel_render(frame,&s,2,2000);assert(frame[224].r==12);
    s.ready=false;pixel_render(frame,&s,0,0);pixel_message(other,"SETUP",0,0xff9f26);assert(same(frame,other));
    s.ready=true;s.stable=false;pixel_render(frame,&s,0,0);pixel_message(other,"SETTLE",0,0xff9f26);assert(same(frame,other));
    s.stable=true;s.demo=true;pixel_render(frame,&s,0,0);pixel_message(other,"DEMO",0,0x00cfff);assert(same(frame,other));
    pixel_message(frame,"COTTAGE IPA",0,0xffffff);pixel_message(other,"COTTAGE IPA",2000,0xffffff);assert(!same(frame,other));
    if(argc>1) {
        FILE *f=fopen(argv[1],"w");assert(f);
        s=(pixel_state_t){.servings=42,.percent=68,.gallons=3.4,.ready=true,.stable=true,.name="COTTAGE IPA"};
        fprintf(f,"const frames = [");
        const char *messages[]={"A1B2C3","PAIR - OPEN SCALE WEB","NO LINK","SETTLE","SETUP","DEMO"};
        for(unsigned page=0;page<10;page++) {
            if(page) fprintf(f,",");
            fprintf(f,"[");
            for(unsigned t=0;t<80;t++) {
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
    puts("PASS: matrix mappings, gauge limits/colors, invalid numbers, setup, settling, demo, scrolling");
}
