#include "controller_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    cl_session_t scale={0},display={0},other={0};
    char scale_pub[131],display_pub[131],a_code[13],b_code[13];
    assert(cl_keypair(&scale,scale_pub)==ESP_OK);
    assert(cl_keypair(&display,display_pub)==ESP_OK);
    assert(cl_agree(&scale,display_pub,scale_pub,display_pub,a_code)==ESP_OK);
    assert(cl_agree(&display,scale_pub,scale_pub,display_pub,b_code)==ESP_OK);
    assert(!strcmp(a_code,b_code));assert(!memcmp(scale.master,display.master,32));
    uint8_t challenge[32]={1};
    assert(cl_start(&scale,challenge,true)==ESP_OK);
    assert(cl_start(&display,challenge,false)==ESP_OK);
    uint8_t frame[CL_MAX_FRAME],damaged[CL_MAX_FRAME];char plain[CL_MAX_PLAIN];size_t len;
    assert(cl_seal(&display,"{\"type\":\"auth\"}",frame,&len)==ESP_OK);
    memcpy(damaged,frame,len);damaged[len-1]^=1;
    assert(cl_open(&scale,damaged,len,plain)!=ESP_OK); /* tamper must not consume sequence */
    assert(cl_open(&scale,frame,len,plain)==ESP_OK);
    assert(!strcmp(plain,"{\"type\":\"auth\"}"));
    assert(cl_open(&scale,frame,len,plain)!=ESP_OK); /* replay */
    assert(cl_open(&display,frame,len,plain)!=ESP_OK); /* reflected direction */
    assert(cl_seal(&scale,"state",frame,&len)==ESP_OK);
    assert(cl_open(&display,frame,len,plain)==ESP_OK);
    memcpy(other.master,display.master,32);challenge[0]=2;
    assert(cl_start(&other,challenge,false)==ESP_OK);
    assert(cl_open(&other,frame,len,plain)!=ESP_OK); /* previous connection */
    assert(cl_open(&display,frame,3,plain)!=ESP_OK);
    char max[CL_MAX_PLAIN];memset(max,'x',sizeof(max));max[sizeof(max)-1]=0;
    assert(cl_seal(&scale,max,frame,&len)==ESP_OK);
    assert(cl_open(&display,frame,len,plain)==ESP_OK);assert(!strcmp(max,plain));
    scale.tx_seq=UINT32_MAX;assert(cl_seal(&scale,"x",frame,&len)!=ESP_OK);
    uint8_t bytes[6];assert(!cl_unhex("xyz",bytes,6));assert(!cl_unhex("xxxxxxxxxxxx",bytes,6));
    assert(cl_unhex("AABBCCDDEEFF",bytes,6));
    cl_clear(&scale);cl_clear(&display);cl_clear(&other);
    puts("PASS: ECDH agreement, bidirectional encryption, tamper rejection, replay rejection, connection isolation, frame bounds");
}
