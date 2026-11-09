#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqtt_feed.h"
#include "wifi_policy.h"

// Matches the scale's combined /state schema, including ON/OFF flags and fractions.
static const char *snapshot="{\"calibrated\":\"ON\",\"stable\":\"ON\",\"tare_set\":\"ON\",\"servings_remaining\":42.9,\"remaining_percent\":68,\"remaining_gallons\":3.4,\"keg_name\":\"Cottage IPA\",\"total_weight\":32.2}";
static bool state(tc001_feed_t *f,const char *s,bool retained,int64_t now) {
    return tc001_feed_state(f,s,strlen(s),retained,now);
}
int main(void) {
    assert(tc001_host_valid("192.168.90.10"));assert(tc001_host_valid("mqtt.cottage.local"));
    assert(!tc001_host_valid("mqtt://192.168.90.10"));assert(!tc001_host_valid("mqtt:1883"));
    assert(!tc001_host_valid(""));assert(!tc001_host_valid(".mqtt"));
    char long_host[129];memset(long_host,'a',128);long_host[128]=0;assert(!tc001_host_valid(long_host));
    assert(tc001_topic_base_valid("kegscale/ab8070"));assert(tc001_topic_base_valid("site/scale-one"));
    assert(!tc001_topic_base_valid(""));assert(!tc001_topic_base_valid("kegscale/+"));
    assert(!tc001_topic_base_valid("kegscale/#"));assert(!tc001_topic_base_valid("/kegscale/a"));
    assert(!tc001_topic_base_valid("kegscale/a/"));assert(!tc001_topic_base_valid("keg scale/a"));
    char long_topic[97];memset(long_topic,'a',96);long_topic[96]=0;assert(!tc001_topic_base_valid(long_topic));
    tc001_feed_t f={0};
    assert(!state(&f,snapshot,false,1000));
    tc001_feed_connect(&f,true);
    assert(!state(&f,snapshot,true,1000));assert(!tc001_feed_fresh(&f,1000));
    tc001_feed_availability(&f,"online",6);assert(!tc001_feed_fresh(&f,1000));
    assert(state(&f,snapshot,false,1000));assert(tc001_feed_fresh(&f,1000));
    assert(f.state.servings==42 && f.state.ready && f.state.stable && !strcmp(f.state.name,"Cottage IPA"));
    assert(f.state.percent==68 && f.state.gallons>3.39f && f.state.gallons<3.41f);
    assert(tc001_feed_fresh(&f,36000));assert(!tc001_feed_fresh(&f,36001));assert(!tc001_feed_fresh(&f,999));
    assert(!tc001_state_stale(36000,1000));assert(tc001_state_stale(36001,1000));
    const char *invalid[]={"{}","[]","not json","{\"stable\":\"ON\",\"calibrated\":\"ON\",\"servings_remaining\":-1,\"remaining_percent\":10,\"remaining_gallons\":2}",
        "{\"stable\":\"ON\",\"calibrated\":\"ON\",\"servings_remaining\":1,\"remaining_percent\":101,\"remaining_gallons\":2}",
        "{\"stable\":\"ON\",\"calibrated\":\"ON\",\"servings_remaining\":1,\"remaining_percent\":10,\"remaining_gallons\":1e999}",
        "{\"stable\":\"ON\",\"calibrated\":\"ON\",\"servings_remaining\":\"1\",\"remaining_percent\":10,\"remaining_gallons\":2}",
        "{\"stable\":\"ON\",\"calibrated\":\"ON\",\"servings_remaining\":1}",
        "{\"stable\":\"maybe\",\"calibrated\":\"ON\"}","{}junk"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) assert(!state(&f,invalid[i],false,40000));
    assert(f.last_live_ms==1000);assert(!tc001_feed_fresh(&f,40000));
    assert(state(&f,snapshot,false,41000));
    tc001_feed_availability(&f,"offline",7);assert(!tc001_feed_fresh(&f,41000));
    tc001_feed_availability(&f,"online",6);assert(!tc001_feed_fresh(&f,41000));
    assert(!state(&f,snapshot,true,42000));assert(!tc001_feed_fresh(&f,42000));
    assert(state(&f,snapshot,false,43000));assert(tc001_feed_fresh(&f,43000));
    // IP recovery resets freshness even if a brief outage preserved the MQTT socket.
    tc001_feed_connect(&f,true);assert(!tc001_feed_fresh(&f,43500));
    assert(state(&f,snapshot,false,43600));
    tc001_feed_connect(&f,false);assert(!tc001_feed_fresh(&f,44000));
    tc001_feed_connect(&f,true);assert(!state(&f,snapshot,true,45000));
    assert(!tc001_feed_fresh(&f,45000));assert(state(&f,snapshot,false,46000));
    // Missing readings must replace a former live count with SETUP, never zero beers.
    assert(state(&f,"{\"calibrated\":\"OFF\",\"stable\":\"OFF\"}",false,47000));
    assert(!f.state.ready && tc001_feed_fresh(&f,47000));
    assert(state(&f,"{\"calibrated\":true,\"stable\":false,\"servings_remaining\":0,\"remaining_percent\":0,\"remaining_gallons\":0,\"keg_name\":\"Empty keg\"}",false,48000));
    assert(f.state.ready && !f.state.stable && f.state.servings==0);
    assert(!tc001_feed_state(&f,"{}\0junk",7,false,49000));
    char nested[129];memset(nested,'[',64);memset(nested+64,']',64);nested[128]=0;
    assert(!state(&f,nested,false,50000));
    assert(state(&f,snapshot,false,50000));
    tc001_mqtt_frame_t frame={0};
    const char *topic="kegscale/ab8070/state";int tn=(int)strlen(topic),n=(int)strlen(snapshot);
    for(int split=1;split<n;split++) {
        assert(tc001_mqtt_append(&frame,topic,tn,snapshot,split,n,0,false)==0);
        assert(tc001_mqtt_append(&frame,NULL,0,snapshot+split,n-split,n,split,false)==1);
        assert(!strcmp(frame.topic,topic));assert(!strcmp(frame.payload,snapshot));
        assert(tc001_feed_state(&f,frame.payload,frame.length,frame.retained,51000));
    }
    // Simulate a snapshot larger than the ESP-MQTT 1024-byte receive buffer.
    char large[2048];snprintf(large,sizeof(large),"%s",snapshot);size_t base=strlen(large);
    memset(large+base,' ',sizeof(large)-base);
    assert(tc001_mqtt_append(&frame,topic,tn,large,1024,sizeof(large),0,false)==0);
    assert(tc001_mqtt_append(&frame,NULL,0,large+1024,1024,sizeof(large),1024,false)==1);
    assert(tc001_feed_state(&f,frame.payload,frame.length,frame.retained,52000));
    assert(tc001_mqtt_append(&frame,topic,tn,"abc",3,6,0,true)==0);
    assert(tc001_mqtt_append(&frame,NULL,0,"def",3,6,3,true)==1);assert(frame.retained);
    assert(tc001_mqtt_append(&frame,topic,tn,"abc",3,6,0,false)==0);
    assert(tc001_mqtt_append(&frame,NULL,0,"de",2,6,4,false)==-1);
    assert(tc001_mqtt_append(&frame,NULL,0,"def",3,6,3,false)==-1);
    assert(tc001_mqtt_append(&frame,topic,tn,"abc",3,6,0,false)==0);
    assert(tc001_mqtt_append(&frame,NULL,0,"defg",4,6,3,false)==-1);
    assert(tc001_mqtt_append(&frame,topic,tn,"abc",3,6,0,false)==0);
    assert(tc001_mqtt_append(&frame,"other",5,"def",3,6,3,false)==-1);
    assert(tc001_mqtt_append(&frame,topic,tn,"abc",3,6,0,false)==0);
    assert(tc001_mqtt_append(&frame,NULL,0,"def",3,6,3,true)==-1);
    assert(tc001_mqtt_append(&frame,topic,tn,"a",1,TC001_MQTT_PAYLOAD_MAX+1,0,false)==-1);
    assert(tc001_mqtt_append(&frame,topic,tn,"a",-1,1,0,false)==-1);
    assert(tc001_mqtt_append(&frame,topic,tn,NULL,1,1,0,false)==-1);
    assert(tc001_mqtt_append(&frame,topic,tn,NULL,0,0,0,false)==1);
    assert(!tc001_feed_state(&f,frame.payload,frame.length,false,53000));
    puts("PASS: actual scale JSON, retained-data rejection, reconnect/offline/staleness, invalid/unconfigured readings, MQTT fragmentation and bounds");
}
