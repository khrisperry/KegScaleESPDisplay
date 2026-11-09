#include "mqtt_feed.h"
#include "cJSON.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool on(const cJSON *root, const char *key) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(root,key);
    return cJSON_IsTrue(v) || (cJSON_IsString(v) && !strcmp(v->valuestring,"ON"));
}
static bool number(const cJSON *root, const char *key, double max, double *out) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(root,key);
    if(!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<0 || v->valuedouble>max) return false;
    *out=v->valuedouble;return true;
}
// Bound cJSON recursion to fit the embedded MQTT task's stack.
static bool shallow_json(const char *s, size_t n) {
    unsigned depth=0;bool quoted=false,escape=false;
    for(size_t i=0;i<n;i++) {
        char c=s[i];
        if(quoted) { if(escape) escape=false;else if(c=='\\') escape=true;else if(c=='"') quoted=false; }
        else if(c=='"') quoted=true;
        else if(c=='{' || c=='[') { if(++depth>16) return false; }
        else if(c=='}' || c==']') { if(!depth) return false;depth--; }
    }
    return !quoted && depth==0;
}
void tc001_feed_connect(tc001_feed_t *f, bool connected) {
    memset(f,0,sizeof(*f));f->connected=connected;
}
bool tc001_feed_state(tc001_feed_t *f, const char *json, size_t len, bool retained, int64_t now) {
    if(!f->connected || retained || !json || !len || len>TC001_MQTT_PAYLOAD_MAX || !shallow_json(json,len)) return false;
    const char *end=NULL;
    cJSON *root=cJSON_ParseWithLengthOpts(json,len,&end,false);
    if(!root) return false;
    // Reject embedded NULs, trailing junk and non-object messages.
    size_t consumed=(size_t)(end-json);
    while(consumed<len && (json[consumed]==' ' || json[consumed]=='\n' || json[consumed]=='\r' || json[consumed]=='\t')) consumed++;
    bool valid=cJSON_IsObject(root) && consumed==len;
    const cJSON *calibrated=cJSON_GetObjectItemCaseSensitive(root,"calibrated");
    const cJSON *stable=cJSON_GetObjectItemCaseSensitive(root,"stable");
    bool flags=(cJSON_IsBool(calibrated) || (cJSON_IsString(calibrated) &&
        (!strcmp(calibrated->valuestring,"ON") || !strcmp(calibrated->valuestring,"OFF")))) &&
        (cJSON_IsBool(stable) || (cJSON_IsString(stable) &&
        (!strcmp(stable->valuestring,"ON") || !strcmp(stable->valuestring,"OFF"))));
    double servings=0,percent=0,gallons=0;
    bool values=number(root,"servings_remaining",65535,&servings) &&
        number(root,"remaining_percent",100,&percent) && number(root,"remaining_gallons",FLT_MAX,&gallons);
    // An unconfigured scale omits calculated values. Clear the previous reading
    // and show SETUP, while still recognizing its valid live telemetry.
    bool missing=!cJSON_HasObjectItem(root,"servings_remaining") &&
        !cJSON_HasObjectItem(root,"remaining_percent") && !cJSON_HasObjectItem(root,"remaining_gallons");
    valid=valid && flags && (values || missing);
    if(valid) {
        pixel_state_t state={.servings=(unsigned)floor(servings),.percent=(float)percent,.gallons=(float)gallons,
            .ready=values && on(root,"calibrated"),.stable=on(root,"stable")};
        const cJSON *name=cJSON_GetObjectItemCaseSensitive(root,"keg_name");
        if(cJSON_IsString(name)) snprintf(state.name,sizeof(state.name),"%s",name->valuestring);
        if(state.ready && !state.name[0]) state.ready=false;
        f->state=state;f->have_live=true;f->source_offline=false;f->last_live_ms=now;
    }
    cJSON_Delete(root);return valid;
}
void tc001_feed_availability(tc001_feed_t *f, const char *value, size_t len) {
    if(value && len==7 && !memcmp(value,"offline",7)) {
        f->source_offline=true;f->have_live=false;
    }
    // An 'online' announcement (including retained) never revives an old reading.
}
bool tc001_feed_fresh(const tc001_feed_t *f, int64_t now) {
    return f->connected && f->have_live && !f->source_offline &&
        now>=f->last_live_ms && now-f->last_live_ms<=TC001_MQTT_STALE_MS;
}
bool tc001_topic_base_valid(const char *base) {
    if(!base || !base[0] || base[0]=='/') return false;
    size_t n=0;
    for(;base[n];n++) {
        unsigned char c=(unsigned char)base[n];
        if(n>=95 || c<=32 || c>=127 || c=='+' || c=='#') return false;
    }
    return base[n-1]!='/';
}
int tc001_mqtt_append(tc001_mqtt_frame_t *f, const char *topic, int topic_len,
    const char *data, int len, int total, int offset, bool retained) {
    if(!f) return -1;
    if(offset==0) {
        memset(f,0,sizeof(*f));
        if(!topic || topic_len<=0 || topic_len>=TC001_MQTT_TOPIC_MAX || total<0 || total>TC001_MQTT_PAYLOAD_MAX) return -1;
        memcpy(f->topic,topic,(size_t)topic_len);f->expected=(size_t)total;f->retained=retained;f->active=true;
    }
    if(!f->active || len<0 || offset<0 || total<0 || (size_t)total!=f->expected ||
        (size_t)offset!=f->length || f->length>f->expected || (size_t)len>f->expected-f->length ||
        (len && !data) || retained!=f->retained || topic_len<0 ||
        (topic_len && (!topic || (size_t)topic_len!=strlen(f->topic) || memcmp(topic,f->topic,(size_t)topic_len)))) {
        f->active=false;return -1;
    }
    if(len) memcpy(f->payload+f->length,data,(size_t)len);
    f->length+=(size_t)len;f->payload[f->length]=0;
    if(f->length==f->expected) { f->active=false;return 1; }
    return 0;
}
