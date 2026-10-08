#include "wifi_link.h"
#include "wifi_policy.h"
#include "wifi_frame.h"
#include "controller_link.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "bootloader_random.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <assert.h>
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdatomic.h>

static const char *TAG="tc001_wifi";
extern const char setup_start[] asm("_binary_setup_html_start");
extern const char setup_end[] asm("_binary_setup_html_end");
typedef struct {
    uint32_t version;
    char ssid[33],password[65],host[128];
    bool paired;
    uint8_t master[32];
} settings_t;
typedef tc001_frame_t frame_t;
_Static_assert(TC001_MAX_FRAME==CL_MAX_FRAME,"Framing limit must match shared controller protocol");
static SemaphoreHandle_t guard;
static QueueHandle_t frames;
static settings_t settings;
static tc001_view_t view;
static bool setup_requested, restart_pending;
static int64_t restart_at;
static _Atomic bool wifi_ready, wifi_connecting, transport_bad;
static _Atomic uint32_t generation;
static char setup_token[33],hostname[33];
static cl_session_t session;
static bool traffic_ready,authenticated;
static uint8_t nonce[32];
static char public_key[131];
static esp_websocket_client_handle_t ws;
static frame_t assembly;
static int64_t connected_at,last_state,last_ping,pair_deadline,next_attempt;
static int64_t ms(void) { return esp_timer_get_time()/1000; }
static const char *str(const cJSON *o,const char *key) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);
    return cJSON_IsString(v)?v->valuestring:"";
}
static void message(const char *s) {
    xSemaphoreTake(guard,portMAX_DELAY);
    if(strcmp(view.message,s)) { snprintf(view.message,sizeof(view.message),"%s",s);view.message_start_ms=ms(); }
    xSemaphoreGive(guard);
}
void tc001_wifi_view(tc001_view_t *out) {
    xSemaphoreTake(guard,portMAX_DELAY);*out=view;xSemaphoreGive(guard);
}
void tc001_wifi_setup_request(void) {
    xSemaphoreTake(guard,portMAX_DELAY);setup_requested=true;xSemaphoreGive(guard);
}
static esp_err_t persist(const settings_t *s) {
    nvs_handle_t n;esp_err_t e=nvs_open("tc001_wifi",NVS_READWRITE,&n);
    if(e!=ESP_OK) return e;
    e=nvs_set_blob(n,"settings",s,sizeof(*s));
    if(e==ESP_OK) e=nvs_commit(n);
    nvs_close(n);return e;
}
static esp_err_t clear_pairing(void) {
    xSemaphoreTake(guard,portMAX_DELAY);
    if(restart_pending) { xSemaphoreGive(guard);return ESP_ERR_INVALID_STATE; }
    settings_t updated=settings;updated.paired=false;memset(updated.master,0,32);
    esp_err_t e=persist(&updated);
    if(e==ESP_OK) settings=updated;
    xSemaphoreGive(guard);return e;
}
static esp_err_t json_response(httpd_req_t *r,cJSON *o) {
    if(!o) return ESP_ERR_NO_MEM;
    char *body=cJSON_PrintUnformatted(o);cJSON_Delete(o);
    if(!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    esp_err_t e=httpd_resp_sendstr(r,body);free(body);return e;
}
static esp_err_t error_response(httpd_req_t *r,const char *status,const char *text) {
    httpd_resp_set_status(r,status);cJSON *o=cJSON_CreateObject();
    if(o) cJSON_AddStringToObject(o,"error",text);
    return json_response(r,o);
}
static bool setup_allowed(httpd_req_t *r,bool token) {
    xSemaphoreTake(guard,portMAX_DELAY);bool active=view.setup_active;xSemaphoreGive(guard);
    struct sockaddr_in local;socklen_t size=sizeof(local);
    if(!active || getsockname(httpd_req_to_sockfd(r),(struct sockaddr *)&local,&size)!=0 ||
        local.sin_family!=AF_INET || local.sin_addr.s_addr!=inet_addr("192.168.4.1")) return false;
    if(token) {
        char supplied[33];
        if(httpd_req_get_hdr_value_str(r,"X-Keg-Setup",supplied,sizeof(supplied))!=ESP_OK || strcmp(supplied,setup_token)) return false;
    }
    return true;
}
static esp_err_t setup_page(httpd_req_t *r) {
    if(!setup_allowed(r,false)) return error_response(r,"403 Forbidden","Hold the middle button for setup and join the clock Wi-Fi");
    size_t len=(size_t)(setup_end-setup_start);
    char *page=malloc(len+64);if(!page) return ESP_ERR_NO_MEM;
    const char *mark=strstr(setup_start,"__SETUP_TOKEN__");
    if(!mark) { free(page);return ESP_FAIL; }
    size_t prefix=(size_t)(mark-setup_start);
    memcpy(page,setup_start,prefix);memcpy(page+prefix,setup_token,32);
    size_t suffix=(size_t)(setup_end-(mark+15));
    memcpy(page+prefix+32,mark+15,suffix);page[prefix+32+suffix]=0;
    httpd_resp_set_type(r,"text/html");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");
    esp_err_t e=httpd_resp_sendstr(r,page);free(page);return e;
}
static esp_err_t status_handler(httpd_req_t *r) {
    if(!setup_allowed(r,true)) return error_response(r,"403 Forbidden","Setup access required");
    cJSON *o=cJSON_CreateObject();if(!o) return ESP_ERR_NO_MEM;
    xSemaphoreTake(guard,portMAX_DELAY);
    cJSON_AddStringToObject(o,"ssid",settings.ssid);cJSON_AddStringToObject(o,"host",settings.host);
    cJSON_AddBoolToObject(o,"paired",settings.paired);cJSON_AddBoolToObject(o,"connected",view.online);
    xSemaphoreGive(guard);
    cJSON_AddBoolToObject(o,"wifi",wifi_ready);cJSON_AddStringToObject(o,"firmware",esp_app_get_description()->version);
    return json_response(r,o);
}
static esp_err_t config_handler(httpd_req_t *r) {
    if(!setup_allowed(r,true)) return error_response(r,"403 Forbidden","Setup access required");
    if(r->content_len<=0 || r->content_len>=512) return error_response(r,"400 Bad Request","Configuration too large");
    char body[512]={0};int used=0;
    while(used<r->content_len) { int n=httpd_req_recv(r,body+used,r->content_len-used);if(n<=0)return ESP_FAIL;used+=n; }
    cJSON *o=cJSON_Parse(body);if(!o) return error_response(r,"400 Bad Request","Invalid JSON");
    const char *ssid=str(o,"ssid"),*password=str(o,"password"),*host=str(o,"host");
    bool open=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"open"));
    bool forget=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"forget"));
    size_t pn=strlen(password);
    uint8_t psk[32];
    bool valid_password=!pn || (pn>=8 && pn<=63) || (pn==64 && cl_unhex(password,psk,32));
    if(!ssid[0] || strlen(ssid)>32 || !valid_password || (host[0] && !tc001_host_valid(host))) {
        cJSON_Delete(o);return error_response(r,"400 Bad Request","Use a valid SSID, WPA password, and scale IP/hostname without a URL or port");
    }
    xSemaphoreTake(guard,portMAX_DELAY);
    if(restart_pending || (settings.paired && strcmp(settings.host,host) && !forget)) {
        xSemaphoreGive(guard);cJSON_Delete(o);return error_response(r,"409 Conflict","Restart pending, or clear the saved pairing before changing scales");
    }
    settings_t updated=settings;
    // Empty password preserves it only for the same SSID, unless open was explicit.
    if(open) updated.password[0]=0;
    else if(pn) snprintf(updated.password,sizeof(updated.password),"%s",password);
    else if(strcmp(ssid,settings.ssid)) updated.password[0]=0;
    if(!open && !updated.password[0]) {
        xSemaphoreGive(guard);cJSON_Delete(o);return error_response(r,"400 Bad Request","Enter the Wi-Fi password or select Open network");
    }
    snprintf(updated.ssid,sizeof(updated.ssid),"%s",ssid);snprintf(updated.host,sizeof(updated.host),"%s",host);
    if(forget) { updated.paired=false;memset(updated.master,0,32); }
    esp_err_t e=persist(&updated);
    if(e==ESP_OK) { restart_pending=true;restart_at=ms()+2000; }
    memset(&updated,0,sizeof(updated));xSemaphoreGive(guard);
    cJSON_Delete(o);memset(body,0,sizeof(body));memset(psk,0,sizeof(psk));
    if(e!=ESP_OK) return error_response(r,"500 Internal Server Error","Could not save settings. Retry setup");
    cJSON *reply=cJSON_CreateObject();if(reply)cJSON_AddStringToObject(reply,"message","Saved. Clock is restarting. If no scale address was entered, rejoin this setup network to find your scale.");
    return json_response(r,reply);
}
static esp_err_t scan_wifi_handler(httpd_req_t *r) {
    if(!setup_allowed(r,true)) return error_response(r,"403 Forbidden","Setup access required");
    if(wifi_connecting) return error_response(r,"409 Conflict","Wi-Fi is connecting. Try again shortly");
    wifi_scan_config_t scan={.show_hidden=false};
    esp_err_t e=esp_wifi_scan_start(&scan,true);
    if(e!=ESP_OK) return error_response(r,"503 Service Unavailable","Wi-Fi scan busy. Try again");
    wifi_ap_record_t records[20];uint16_t count=20;
    e=esp_wifi_scan_get_ap_records(&count,records);
    if(e!=ESP_OK) { esp_wifi_clear_ap_list();return error_response(r,"503 Service Unavailable","Scan failed"); }
    cJSON *o=cJSON_CreateObject(),*a=cJSON_CreateArray();if(!o||!a){cJSON_Delete(o);cJSON_Delete(a);return ESP_ERR_NO_MEM;}
    cJSON_AddItemToObject(o,"networks",a);
    for(unsigned i=0;i<count;i++) {
        bool duplicate=false;
        for(unsigned j=0;j<i;j++) if(!strcmp((char *)records[i].ssid,(char *)records[j].ssid)) duplicate=true;
        if(!duplicate) cJSON_AddItemToArray(a,cJSON_CreateString((char *)records[i].ssid));
    }
    return json_response(r,o);
}
static esp_err_t scan_scales_handler(httpd_req_t *r) {
    if(!setup_allowed(r,true)) return error_response(r,"403 Forbidden","Setup access required");
    if(!wifi_ready) return error_response(r,"409 Conflict","Save Wi-Fi first, then rejoin this setup network to find scales");
    mdns_result_t *found=NULL;esp_err_t e=mdns_query_ptr("_kegscale","_tcp",2000,8,&found);
    if(e!=ESP_OK) return error_response(r,"503 Service Unavailable","Discovery failed. Enter the scale IP manually");
    cJSON *o=cJSON_CreateObject(),*a=cJSON_CreateArray();if(!o||!a){cJSON_Delete(o);cJSON_Delete(a);mdns_query_results_free(found);return ESP_ERR_NO_MEM;}
    cJSON_AddItemToObject(o,"scales",a);
    for(mdns_result_t *p=found;p;p=p->next) if(p->hostname && p->port==80) {
        char host[128];snprintf(host,sizeof(host),"%s.local",p->hostname);
        if(tc001_host_valid(host)) cJSON_AddItemToArray(a,cJSON_CreateString(host));
    }
    mdns_query_results_free(found);return json_response(r,o);
}
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;(void)data;
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) { wifi_ready=false;wifi_connecting=false;transport_bad=true; }
    else if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) { wifi_ready=true;wifi_connecting=false;ESP_LOGI(TAG,"Wi-Fi connected"); }
}
static void socket_event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)base;uint32_t token=(uint32_t)(uintptr_t)arg;
    if(token!=generation) return;
    frame_t f={.generation=token};
    if(id==WEBSOCKET_EVENT_CONNECTED) f.kind=1;
    else if(id==WEBSOCKET_EVENT_DISCONNECTED || id==WEBSOCKET_EVENT_ERROR) { transport_bad=true;return; }
    else if(id==WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *d=data;
        if(d->op_code!=1 && d->op_code!=2) return;
        int result=tc001_frame_append(&assembly,token,d->op_code,d->payload_len,d->payload_offset,d->data_len,d->data_ptr);
        if(result<0) { transport_bad=true;return; }
        if(result==0) return;
        f=assembly;
    } else return;
    if(xQueueSend(frames,&f,0)!=pdTRUE) transport_bad=true;
}
static void stop_socket(void) {
    ++generation; // Ignore all events/queued data from the retired connection.
    if(ws) { esp_websocket_client_stop(ws);esp_websocket_client_destroy(ws);ws=NULL; }
    cl_clear(&session);traffic_ready=false;authenticated=false;connected_at=0;pair_deadline=0;last_state=0;
    xSemaphoreTake(guard,portMAX_DELAY);view.online=false;xSemaphoreGive(guard);
    transport_bad=false;
}
static bool secure_send(const char *plain) {
    uint8_t encrypted[CL_MAX_FRAME];size_t len=0;
    if(!traffic_ready || !ws || cl_seal(&session,plain,encrypted,&len)!=ESP_OK) return false;
    return esp_websocket_client_send_bin(ws,(char *)encrypted,len,pdMS_TO_TICKS(1000))==(int)len;
}
static bool hello(void) {
    char nonce_hex[65],body[400];esp_fill_random(nonce,32);cl_hex(nonce,32,nonce_hex);
    xSemaphoreTake(guard,portMAX_DELAY);bool paired=settings.paired;
    if(paired) memcpy(session.master,settings.master,32);
    xSemaphoreGive(guard);
    if(paired) snprintf(body,sizeof(body),"{\"type\":\"hello\",\"protocol\":1,\"firmware\":\"%s\",\"nonce\":\"%s\"}",esp_app_get_description()->version,nonce_hex);
    else {
        if(cl_keypair(&session,public_key)!=ESP_OK) return false;
        snprintf(body,sizeof(body),"{\"type\":\"hello\",\"protocol\":1,\"firmware\":\"%s\",\"public\":\"%s\",\"nonce\":\"%s\"}",esp_app_get_description()->version,public_key,nonce_hex);
    }
    return esp_websocket_client_send_text(ws,body,strlen(body),pdMS_TO_TICKS(1000))==(int)strlen(body);
}
static bool process(frame_t *f) {
    if(f->generation!=generation) return true;
    if(f->kind==1) { connected_at=ms();return hello(); }
    char plain[CL_MAX_PLAIN];
    if(f->kind==4) {
        if(!traffic_ready || cl_open(&session,f->bytes,f->len,plain)!=ESP_OK) return false;
    } else {
        if(!tc001_allow_handshake(traffic_ready,authenticated) || f->len>=sizeof(plain)) return false;
        memcpy(plain,f->bytes,f->len);plain[f->len]=0;
    }
    cJSON *o=cJSON_Parse(plain);if(!o) return false;
    const char *type=str(o,"type");bool good=false;
    xSemaphoreTake(guard,portMAX_DELAY);bool paired=settings.paired;xSemaphoreGive(guard);
    if(f->kind==3 && (!strcmp(type,"pair") || !strcmp(type,"challenge"))) {
        cJSON *protocol=cJSON_GetObjectItemCaseSensitive(o,"protocol");uint8_t challenge[32];
        bool pairing=!strcmp(type,"pair");char code[13]={0};
        good=cJSON_IsNumber(protocol) && protocol->valuedouble==1 && cl_unhex(str(o,"challenge"),challenge,32);
        if(pairing) {
            cJSON *seconds=cJSON_GetObjectItemCaseSensitive(o,"seconds");
            good=good && !paired && cJSON_IsNumber(seconds) && isfinite(seconds->valuedouble) && seconds->valuedouble>0 && seconds->valuedouble<=300;
            if(good) good=cl_agree(&session,str(o,"public"),str(o,"public"),public_key,code)==ESP_OK;
            if(good) { pair_deadline=ms()+(int64_t)(seconds->valuedouble*1000);message(code); }
        } else good=good && paired;
        if(good) good=cl_start(&session,challenge,nonce,false)==ESP_OK;
        traffic_ready=good;last_ping=ms();
        if(good && !pairing) good=secure_send("{\"type\":\"auth\"}");
    } else if(f->kind==4 && !strcmp(type,"authorized")) {
        if(!paired) {
            xSemaphoreTake(guard,portMAX_DELAY);
            settings_t updated=settings;updated.paired=true;memcpy(updated.master,session.master,32);
            good=!restart_pending && persist(&updated)==ESP_OK;
            if(good) settings=updated;
            memset(&updated,0,sizeof(updated));xSemaphoreGive(guard);
        } else good=true;
        if(good) { authenticated=true;pair_deadline=0;last_state=ms();message("LINKING"); }
    } else if(f->kind==4 && authenticated && !strcmp(type,"state")) {
        cJSON *protocol=cJSON_GetObjectItemCaseSensitive(o,"protocol"),*servings=cJSON_GetObjectItemCaseSensitive(o,"servings"),
            *percent=cJSON_GetObjectItemCaseSensitive(o,"percent"),*gallons=cJSON_GetObjectItemCaseSensitive(o,"gallons");
        good=cJSON_IsNumber(protocol) && protocol->valuedouble==1 && cJSON_IsNumber(servings) &&
            isfinite(servings->valuedouble) && servings->valuedouble>=0 && servings->valuedouble<=65535 &&
            cJSON_IsNumber(percent) && isfinite(percent->valuedouble) && percent->valuedouble>=0 && percent->valuedouble<=100 &&
            cJSON_IsNumber(gallons) && isfinite(gallons->valuedouble) && gallons->valuedouble>=0 && gallons->valuedouble<=FLT_MAX;
        if(good) {
            pixel_state_t state={.servings=(unsigned)floor(servings->valuedouble),.percent=(float)percent->valuedouble,.gallons=(float)gallons->valuedouble,
                .ready=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"ready")) && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"reading_valid")),
                .stable=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"stable"))};
            snprintf(state.name,sizeof(state.name),"%s",str(o,"name"));last_state=ms();
            xSemaphoreTake(guard,portMAX_DELAY);view.state=state;view.last_state_ms=last_state;view.online=true;view.message[0]=0;xSemaphoreGive(guard);
        }
    } else if(f->kind==4 && authenticated && !strcmp(type,"unpair")) {
        if(clear_pairing()!=ESP_OK) message("SAVE FAILED");else message("ADD ON SCALE");
        good=false;
    } else if(f->kind==4 && !paired && !strcmp(type,"pairing_canceled")) { message("ADD ON SCALE");good=false; }
    cJSON_Delete(o);memset(plain,0,sizeof(plain));return good;
}
static bool connect_socket(void) {
    char uri[180];snprintf(uri,sizeof(uri),"ws://%s/ws/controller",settings.host);
    esp_websocket_client_config_t config={.uri=uri,.buffer_size=CL_MAX_FRAME+1,.task_stack=6144,
        .disable_auto_reconnect=true,.network_timeout_ms=5000,.disable_pingpong_discon=true};
    ws=esp_websocket_client_init(&config);if(!ws) return false;
    ++generation;connected_at=ms();
    esp_err_t e=esp_websocket_register_events(ws,WEBSOCKET_EVENT_ANY,socket_event,(void *)(uintptr_t)generation);
    if(e==ESP_OK) e=esp_websocket_client_start(ws);
    return e==ESP_OK;
}
static void setup_mode(bool enabled) {
    wifi_config_t ap={0};
    xSemaphoreTake(guard,portMAX_DELAY);
    if(enabled) {
        memcpy(ap.ap.ssid,view.setup_ssid,strlen(view.setup_ssid));ap.ap.ssid_len=strlen(view.setup_ssid);
        memcpy(ap.ap.password,view.setup_password,8);ap.ap.authmode=WIFI_AUTH_WPA2_PSK;ap.ap.max_connection=2;
        view.setup_active=true;view.setup_start_ms=ms();
    } else view.setup_active=false;
    xSemaphoreGive(guard);
    if(enabled) { ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap)); }
    else ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}
static void worker(void *unused) {
    (void)unused;int64_t next_wifi=0;
    frame_t *f=malloc(sizeof(*f));assert(f);
    for(;;) {
        int64_t now=ms();
        xSemaphoreTake(guard,portMAX_DELAY);
        bool updating=restart_pending;
        bool reboot=updating && now>=restart_at;
        bool setup=setup_requested;setup_requested=false;
        bool complete=settings.ssid[0] && settings.host[0];
        bool expire=view.setup_active && complete && now-view.setup_start_ms>=600000;
        xSemaphoreGive(guard);
        if(reboot) esp_restart();
        if(updating) { if(ws)stop_socket();vTaskDelay(pdMS_TO_TICKS(100));continue; }
        if(setup) setup_mode(true);else if(expire) setup_mode(false);
        if(!wifi_ready && !wifi_connecting && settings.ssid[0] && now>=next_wifi) {
            wifi_connecting=esp_wifi_connect()==ESP_OK;next_wifi=now+10000;
            message("WIFI WAIT");
        }
        if(transport_bad || (ws && (!wifi_ready ||
            (authenticated && tc001_state_stale(now,last_state)) ||
            (!traffic_ready && now-connected_at>10000) ||
            (!authenticated && traffic_ready && pair_deadline && now>=pair_deadline) ||
            (!authenticated && traffic_ready && !pair_deadline && now-connected_at>15000)))) {
            stop_socket();next_attempt=ms()+10000;message(wifi_ready?"NO LINK":"WIFI WAIT");
        }
        if(wifi_ready && settings.host[0] && !ws && now>=next_attempt) {
            message(settings.paired?"LINKING":"ADD ON SCALE");
            if(!connect_socket()) { stop_socket();next_attempt=ms()+10000; }
        }
        if(xQueueReceive(frames,f,pdMS_TO_TICKS(100))==pdTRUE && !process(f)) transport_bad=true;
        if(traffic_ready && ms()-last_ping>=3000) {
            last_ping=ms();if(!secure_send("{\"type\":\"ping\"}")) transport_bad=true;
        }
    }
}
esp_err_t tc001_wifi_start(void) {
    guard=xSemaphoreCreateMutex();frames=xQueueCreate(6,sizeof(frame_t));if(!guard||!frames) return ESP_ERR_NO_MEM;
    settings.version=1;
    bootloader_random_enable();
    nvs_handle_t n;esp_err_t e=nvs_open("tc001_wifi",NVS_READWRITE,&n);if(e!=ESP_OK)return e;
    size_t size=sizeof(settings);
    e=nvs_get_blob(n,"settings",&settings,&size);
    if(e!=ESP_OK && e!=ESP_ERR_NVS_NOT_FOUND) { nvs_close(n);return e; }
    if(e==ESP_OK && (size!=sizeof(settings)||settings.version!=1)) { nvs_close(n);return ESP_ERR_INVALID_VERSION; }
    settings.ssid[32]=0;settings.password[64]=0;settings.host[127]=0;
    size=sizeof(view.setup_password);e=nvs_get_str(n,"ap_pass",view.setup_password,&size);
    if(e==ESP_ERR_NVS_NOT_FOUND) {
        // Eight base-32 characters, readable on the pixel display; never logged.
        const char *alphabet="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        uint8_t bytes[8];esp_fill_random(bytes,8);
        for(unsigned i=0;i<8;i++) view.setup_password[i]=alphabet[bytes[i]&31];
        view.setup_password[8]=0;e=nvs_set_str(n,"ap_pass",view.setup_password);
        if(e==ESP_OK)e=nvs_commit(n);
    }
    nvs_close(n);if(e!=ESP_OK)return e;
    uint8_t random_token[16],mac[6];esp_fill_random(random_token,16);cl_hex(random_token,16,setup_token);
    esp_read_mac(mac,ESP_MAC_WIFI_STA);snprintf(hostname,sizeof(hostname),"KegPixel-%02X%02X",mac[4],mac[5]);
    snprintf(view.setup_ssid,sizeof(view.setup_ssid),"%s",hostname);
    ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta=esp_netif_create_default_wifi_sta();esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta,hostname));
    bootloader_random_disable();
    wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t wifi={0};memcpy(wifi.sta.ssid,settings.ssid,strlen(settings.ssid));memcpy(wifi.sta.password,settings.password,strlen(settings.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&wifi));
    // Install the WPA2 AP configuration before starting the radio, including
    // on configured clocks, so recovery never enables a default/open AP.
    setup_mode(true);
    if(settings.ssid[0] && settings.host[0]) setup_mode(false);
    ESP_ERROR_CHECK(esp_wifi_start());ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(mdns_init());ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    httpd_handle_t http;httpd_config_t hc=HTTPD_DEFAULT_CONFIG();hc.stack_size=8192;hc.max_uri_handlers=8;
    ESP_ERROR_CHECK(httpd_start(&http,&hc));
    const httpd_uri_t endpoints[]={
        {.uri="/",.method=HTTP_GET,.handler=setup_page},
        {.uri="/api/status",.method=HTTP_GET,.handler=status_handler},
        {.uri="/api/config",.method=HTTP_POST,.handler=config_handler},
        {.uri="/api/wifi",.method=HTTP_GET,.handler=scan_wifi_handler},
        {.uri="/api/scales",.method=HTTP_GET,.handler=scan_scales_handler}
    };
    for(unsigned i=0;i<sizeof(endpoints)/sizeof(endpoints[0]);i++) ESP_ERROR_CHECK(httpd_register_uri_handler(http,&endpoints[i]));
    if(psa_crypto_init()!=PSA_SUCCESS) return ESP_FAIL;
    message("WIFI WAIT");
    return xTaskCreate(worker,"tc001_wifi",16384,NULL,4,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
