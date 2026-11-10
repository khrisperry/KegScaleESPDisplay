#include "wifi_link.h"
#include "wifi_policy.h"
#include "setup_access.h"
#include "battery.h"
#include "mqtt_feed.h"
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
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include <time.h>

static const char *TAG="tc001_wifi";
extern const char setup_start[] asm("_binary_setup_html_start");
extern const char setup_end[] asm("_binary_setup_html_end");
extern const char battery_start[] asm("_binary_battery_html_start");
extern const char battery_end[] asm("_binary_battery_html_end");
typedef struct {
    uint32_t version;
    char ssid[33],password[65],broker[128],username[65],mqtt_password[129],base_topic[96];
    uint16_t port;
    bool tls;
} settings_t;
// Read Wi-Fi from the previous controller build without erasing other NVS data.
typedef struct {
    uint32_t version;
    char ssid[33],password[65],host[128];
    bool paired;
    uint8_t master[32];
} legacy_settings_t;
static SemaphoreHandle_t guard;
static settings_t settings;
static tc001_view_t view;
static tc001_feed_t feed;
static bool setup_requested, restart_pending, mqtt_connected;
static int64_t restart_at;
static _Atomic bool wifi_ready, wifi_connecting;
static char setup_token[33],hostname[33],state_topic[128],availability_topic[128];
static esp_mqtt_client_handle_t mqtt;
static tc001_mqtt_frame_t assembly;
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
    e=nvs_set_blob(n,"mqtt_settings",s,sizeof(*s));
    if(e==ESP_OK) e=nvs_commit(n);
    nvs_close(n);return e;
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
static bool request_token_valid(httpd_req_t *r) {
    char supplied[33];
    return httpd_req_get_hdr_value_str(r,"X-Keg-Setup",supplied,sizeof(supplied))==ESP_OK && !strcmp(supplied,setup_token);
}
static bool setup_allowed(httpd_req_t *r,bool token) {
    xSemaphoreTake(guard,portMAX_DELAY);bool active=view.setup_active;xSemaphoreGive(guard);
    struct sockaddr_storage local={0};socklen_t size=sizeof(local);
    if(getsockname(httpd_req_to_sockfd(r),(struct sockaddr *)&local,&size)!=0 ||
        !tc001_setup_address_allowed(active,(const struct sockaddr *)&local,size)) return false;
    if(token && !request_token_valid(r)) return false;
    return true;
}
// Battery access is available on the actual station interface or active setup AP.
// Match socket addresses, not spoofable Host/Origin headers; handle dual-stack sockets.
static bool battery_allowed(httpd_req_t *r,bool token) {
    if(setup_allowed(r,false)) return !token || request_token_valid(r);
    if(!wifi_ready) return false;
    struct sockaddr_storage local={0};socklen_t size=sizeof(local);
    if(getsockname(httpd_req_to_sockfd(r),(struct sockaddr *)&local,&size)!=0)return false;
    esp_netif_t *sta=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(!sta)return false;
    esp_netif_ip_info_t info={0};bool allowed=false;
    if(esp_netif_get_ip_info(sta,&info)==ESP_OK)
        allowed=tc001_address_matches_ipv4((const struct sockaddr *)&local,size,(const unsigned char *)&info.ip.addr);
#if CONFIG_LWIP_IPV6
    esp_ip6_addr_t ip6;
    if(!allowed && esp_netif_get_ip6_linklocal(sta,&ip6)==ESP_OK)
        allowed=tc001_address_matches_ipv6((const struct sockaddr *)&local,size,(const unsigned char *)ip6.addr);
    if(!allowed && esp_netif_get_ip6_global(sta,&ip6)==ESP_OK)
        allowed=tc001_address_matches_ipv6((const struct sockaddr *)&local,size,(const unsigned char *)ip6.addr);
#endif
    return allowed && (!token || request_token_valid(r));
}
static esp_err_t send_page(httpd_req_t *r,const char *start,const char *end) {
    size_t len=(size_t)(end-start);
    char *page=malloc(len+64);if(!page) return ESP_ERR_NO_MEM;
    const char *mark=strstr(start,"__SETUP_TOKEN__");
    if(!mark) { free(page);return ESP_FAIL; }
    size_t prefix=(size_t)(mark-start);
    memcpy(page,start,prefix);memcpy(page+prefix,setup_token,32);
    size_t suffix=(size_t)(end-(mark+15));
    memcpy(page+prefix+32,mark+15,suffix);page[prefix+32+suffix]=0;
    httpd_resp_set_type(r,"text/html");httpd_resp_set_hdr(r,"Cache-Control","no-store");
    httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(r,"X-Frame-Options","DENY");
    esp_err_t e=httpd_resp_sendstr(r,page);free(page);return e;
}
static esp_err_t battery_page(httpd_req_t *r) {
    if(!battery_allowed(r,false))return error_response(r,"403 Forbidden","Connect to the clock's local Wi-Fi network or its setup network");
    return send_page(r,battery_start,battery_end);
}
static esp_err_t setup_page(httpd_req_t *r) {
    if(!setup_allowed(r,false))return battery_page(r);
    return send_page(r,setup_start,setup_end);
}
static void add_battery_status(cJSON *o) {
    tc001_battery_t battery;tc001_battery_get(&battery);
    cJSON_AddBoolToObject(o,"battery_valid",battery.valid);
    cJSON_AddNumberToObject(o,"battery_raw",battery.raw);
    if(battery.valid)cJSON_AddNumberToObject(o,"battery_percent",battery.percent);
    cJSON_AddNumberToObject(o,"battery_empty",battery.empty);
    cJSON_AddNumberToObject(o,"battery_full",battery.full);
}
static esp_err_t battery_status_handler(httpd_req_t *r) {
    if(!battery_allowed(r,true))return error_response(r,"403 Forbidden","Battery page access required; reload the page");
    cJSON *o=cJSON_CreateObject();if(!o)return ESP_ERR_NO_MEM;
    add_battery_status(o);
    cJSON_AddStringToObject(o,"hostname",hostname);
    cJSON_AddStringToObject(o,"firmware",esp_app_get_description()->version);
    return json_response(r,o);
}
static esp_err_t status_handler(httpd_req_t *r) {
    if(!setup_allowed(r,true)) return error_response(r,"403 Forbidden","Setup access required");
    cJSON *o=cJSON_CreateObject();if(!o) return ESP_ERR_NO_MEM;
    xSemaphoreTake(guard,portMAX_DELAY);
    cJSON_AddStringToObject(o,"ssid",settings.ssid);
    cJSON_AddStringToObject(o,"broker",settings.broker);
    cJSON_AddStringToObject(o,"username",settings.username);
    cJSON_AddStringToObject(o,"base_topic",settings.base_topic);
    cJSON_AddNumberToObject(o,"port",settings.port);
    cJSON_AddBoolToObject(o,"tls",settings.tls);
    cJSON_AddBoolToObject(o,"mqtt_password_saved",settings.mqtt_password[0]!=0);
    cJSON_AddBoolToObject(o,"connected",view.online);
    cJSON_AddBoolToObject(o,"mqtt",mqtt_connected);
    xSemaphoreGive(guard);
    cJSON_AddBoolToObject(o,"wifi",wifi_ready);cJSON_AddStringToObject(o,"firmware",esp_app_get_description()->version);
    add_battery_status(o);
    return json_response(r,o);
}
static esp_err_t battery_handler(httpd_req_t *r) {
    if(!battery_allowed(r,true))return error_response(r,"403 Forbidden","Battery page access required; reload the page");
    if(r->content_len<=0 || r->content_len>=128)return error_response(r,"400 Bad Request","Calibration too large");
    char body[128]={0};int used=0;
    while(used<r->content_len) { int n=httpd_req_recv(r,body+used,r->content_len-used);if(n<=0)return ESP_FAIL;used+=n; }
    cJSON *o=cJSON_Parse(body);if(!o)return error_response(r,"400 Bad Request","Invalid JSON");
    const cJSON *empty=cJSON_GetObjectItemCaseSensitive(o,"empty"),*full=cJSON_GetObjectItemCaseSensitive(o,"full");
    bool valid=cJSON_IsNumber(empty) && cJSON_IsNumber(full) && isfinite(empty->valuedouble) && isfinite(full->valuedouble) &&
        empty->valuedouble>=100 && full->valuedouble<=1000 && full->valuedouble-empty->valuedouble>=20 &&
        floor(empty->valuedouble)==empty->valuedouble && floor(full->valuedouble)==full->valuedouble;
    esp_err_t err=valid?tc001_battery_calibrate((int)empty->valuedouble,(int)full->valuedouble):ESP_ERR_INVALID_ARG;
    cJSON_Delete(o);
    if(err==ESP_ERR_INVALID_ARG)return error_response(r,"400 Bad Request","Use raw values from 100 to 1000; full must be at least 20 above empty");
    if(err!=ESP_OK)return error_response(r,"500 Internal Server Error","Could not save battery calibration");
    cJSON *reply=cJSON_CreateObject();if(reply)cJSON_AddStringToObject(reply,"message","Battery calibration saved. No restart needed.");
    return json_response(r,reply);
}
static bool wifi_password_valid(const char *p) {
    size_t n=strlen(p);
    if(!n || (n>=8 && n<=63)) return true;
    if(n!=64) return false;
    for(size_t i=0;i<n;i++) if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')||(p[i]>='A'&&p[i]<='F'))) return false;
    return true;
}
static esp_err_t config_handler(httpd_req_t *r) {
    if(!setup_allowed(r,true)) return error_response(r,"403 Forbidden","Setup access required");
    if(r->content_len<=0 || r->content_len>=2048) return error_response(r,"400 Bad Request","Configuration too large");
    char body[2048]={0};int used=0;
    while(used<r->content_len) { int n=httpd_req_recv(r,body+used,r->content_len-used);if(n<=0)return ESP_FAIL;used+=n; }
    cJSON *o=cJSON_Parse(body);if(!o) return error_response(r,"400 Bad Request","Invalid JSON");
    const char *ssid=str(o,"ssid"),*password=str(o,"password"),*broker=str(o,"broker");
    const char *user=str(o,"username"),*pass=str(o,"mqtt_password"),*topic=str(o,"base_topic");
    const cJSON *port=cJSON_GetObjectItemCaseSensitive(o,"port");
    bool open=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"open"));
    bool clear=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"clear_mqtt_password"));
    if(!ssid[0] || strlen(ssid)>32 || !wifi_password_valid(password) || !tc001_host_valid(broker) ||
        strlen(user)>64 || strlen(pass)>128 || !tc001_topic_base_valid(topic) || !cJSON_IsNumber(port) ||
        !isfinite(port->valuedouble) || port->valuedouble<1 || port->valuedouble>65535 || floor(port->valuedouble)!=port->valuedouble) {
        cJSON_Delete(o);return error_response(r,"400 Bad Request","Check Wi-Fi, broker hostname (no URL/port), port, and exact MQTT base topic (no wildcards)");
    }
    xSemaphoreTake(guard,portMAX_DELAY);
    if(restart_pending) {
        xSemaphoreGive(guard);cJSON_Delete(o);return error_response(r,"409 Conflict","Restart pending");
    }
    settings_t updated=settings;
    if(open) updated.password[0]=0;
    else if(password[0]) snprintf(updated.password,sizeof(updated.password),"%s",password);
    else if(strcmp(ssid,settings.ssid)) updated.password[0]=0;
    if(!open && !updated.password[0]) {
        xSemaphoreGive(guard);cJSON_Delete(o);return error_response(r,"400 Bad Request","Enter the Wi-Fi password or select Open network");
    }
    if(clear || strcmp(broker,settings.broker) || strcmp(user,settings.username)) updated.mqtt_password[0]=0;
    if(pass[0] && !clear) snprintf(updated.mqtt_password,sizeof(updated.mqtt_password),"%s",pass);
    snprintf(updated.ssid,sizeof(updated.ssid),"%s",ssid);
    snprintf(updated.broker,sizeof(updated.broker),"%s",broker);
    snprintf(updated.username,sizeof(updated.username),"%s",user);
    snprintf(updated.base_topic,sizeof(updated.base_topic),"%s",topic);
    updated.port=(uint16_t)port->valuedouble;
    updated.tls=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"tls"));
    esp_err_t e=persist(&updated);
    if(e==ESP_OK) { restart_pending=true;restart_at=ms()+2000; }
    memset(&updated,0,sizeof(updated));xSemaphoreGive(guard);
    cJSON_Delete(o);memset(body,0,sizeof(body));
    if(e!=ESP_OK) return error_response(r,"500 Internal Server Error","Could not save settings. Retry setup");
    cJSON *reply=cJSON_CreateObject();if(reply)cJSON_AddStringToObject(reply,"message","Saved. Clock is restarting and will wait for a fresh MQTT reading. No scale pairing is needed.");
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
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        wifi_ready=false;wifi_connecting=false;
        xSemaphoreTake(guard,portMAX_DELAY);tc001_feed_connect(&feed,false);view.online=false;xSemaphoreGive(guard);
    } else if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        wifi_ready=true;wifi_connecting=false;
        // A brief radio outage can leave the TCP/MQTT session alive. Re-enable
        // its feed after IP recovery, but still require a new live snapshot.
        xSemaphoreTake(guard,portMAX_DELAY);tc001_feed_connect(&feed,mqtt_connected);xSemaphoreGive(guard);
        const ip_event_got_ip_t *event=data;
        ESP_LOGI(TAG,"Battery page: http://%s.local/ or http://" IPSTR "/",hostname,IP2STR(&event->ip_info.ip));
    }
}
static void update_view_locked(void) {
    view.online=wifi_ready && tc001_feed_fresh(&feed,ms());
    view.state=feed.state;view.last_state_ms=feed.have_live?feed.last_live_ms:0;
    const char *text=!wifi_ready?"WIFI WAIT":!mqtt_connected?(settings.tls && time(NULL)<1700000000?"TIME WAIT":"MQTT WAIT"):!view.online?"NO DATA":"";
    if(strcmp(view.message,text)) {
        snprintf(view.message,sizeof(view.message),"%s",text);view.message_start_ms=ms();
    }
}
static void mqtt_event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;(void)base;
    esp_mqtt_event_handle_t e=data;
    if(id==MQTT_EVENT_CONNECTED) {
        memset(&assembly,0,sizeof(assembly));
        xSemaphoreTake(guard,portMAX_DELAY);
        mqtt_connected=true;tc001_feed_connect(&feed,true);update_view_locked();
        xSemaphoreGive(guard);
        // Read-only subscriptions. Never subscribe to or publish scale commands.
        int s=esp_mqtt_client_subscribe(e->client,state_topic,1);
        int a=esp_mqtt_client_subscribe(e->client,availability_topic,1);
        ESP_LOGI(TAG,"MQTT connected; subscribing to %s",state_topic);
        if(s<0 || a<0) {
            ESP_LOGW(TAG,"MQTT subscription failed; reconnecting");
            esp_mqtt_client_disconnect(e->client);
        }
    } else if(id==MQTT_EVENT_DISCONNECTED) {
        memset(&assembly,0,sizeof(assembly));
        xSemaphoreTake(guard,portMAX_DELAY);
        mqtt_connected=false;tc001_feed_connect(&feed,false);update_view_locked();
        xSemaphoreGive(guard);
        ESP_LOGW(TAG,"MQTT disconnected; automatic retry enabled");
    } else if(id==MQTT_EVENT_DATA) {
        int result=tc001_mqtt_append(&assembly,e->topic,e->topic_len,e->data,e->data_len,
            e->total_data_len,e->current_data_offset,e->retain);
        if(result!=1) return;
        xSemaphoreTake(guard,portMAX_DELAY);
        if(!strcmp(assembly.topic,state_topic))
            tc001_feed_state(&feed,assembly.payload,assembly.length,assembly.retained,ms());
        else if(!strcmp(assembly.topic,availability_topic))
            tc001_feed_availability(&feed,assembly.payload,assembly.length);
        update_view_locked();xSemaphoreGive(guard);
    } else if(id==MQTT_EVENT_ERROR) {
        // Log codes only: never broker credentials or payloads.
        ESP_LOGW(TAG,"MQTT connection error (type %d, broker return %d). Check address, port, credentials and broker ACL.",
            e->error_handle?(int)e->error_handle->error_type:-1,
            e->error_handle?(int)e->error_handle->connect_return_code:-1);
    }
}
static esp_err_t start_mqtt(void) {
    snprintf(state_topic,sizeof(state_topic),"%s/state",settings.base_topic);
    snprintf(availability_topic,sizeof(availability_topic),"%s/availability",settings.base_topic);
    esp_mqtt_client_config_t config={
        .broker.address={.hostname=settings.broker,.port=settings.port,
            .transport=settings.tls?MQTT_TRANSPORT_OVER_SSL:MQTT_TRANSPORT_OVER_TCP},
        .credentials={.client_id=hostname,.username=settings.username[0]?settings.username:NULL,
            .authentication.password=settings.mqtt_password[0]?settings.mqtt_password:NULL},
        .session={.keepalive=30,.disable_clean_session=false,.protocol_ver=MQTT_PROTOCOL_V_3_1_1},
        .network={.reconnect_timeout_ms=10000,.timeout_ms=5000},
        .buffer={.size=1024},.task={.stack_size=8192}
    };
    if(settings.tls) config.broker.verification.crt_bundle_attach=esp_crt_bundle_attach;
    mqtt=esp_mqtt_client_init(&config);if(!mqtt)return ESP_ERR_NO_MEM;
    esp_err_t err=esp_mqtt_client_register_event(mqtt,ESP_EVENT_ANY_ID,mqtt_event,NULL);
    if(err==ESP_OK) err=esp_mqtt_client_start(mqtt);
    if(err!=ESP_OK) { esp_mqtt_client_destroy(mqtt);mqtt=NULL; }
    return err;
}
static void setup_mode(bool enabled) {
    wifi_config_t ap={0};
    xSemaphoreTake(guard,portMAX_DELAY);
    if(enabled) {
        memcpy(ap.ap.ssid,view.setup_ssid,strlen(view.setup_ssid));ap.ap.ssid_len=strlen(view.setup_ssid);
        ap.ap.authmode=WIFI_AUTH_OPEN;ap.ap.max_connection=2;
        view.setup_active=true;view.setup_start_ms=ms();
    } else view.setup_active=false;
    xSemaphoreGive(guard);
    if(enabled) { ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap)); }
    else ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}
static void worker(void *unused) {
    (void)unused;int64_t next_wifi=0,next_mqtt=0;
    for(;;) {
        int64_t now=ms();
        xSemaphoreTake(guard,portMAX_DELAY);
        bool updating=restart_pending;
        bool reboot=updating && now>=restart_at;
        bool setup=setup_requested;setup_requested=false;
        bool complete=settings.ssid[0] && settings.broker[0] && settings.base_topic[0];
        bool expire=view.setup_active && complete && now-view.setup_start_ms>=600000;
        update_view_locked();xSemaphoreGive(guard);
        if(reboot) esp_restart();
        if(updating) { vTaskDelay(pdMS_TO_TICKS(100));continue; }
        if(setup) setup_mode(true);else if(expire) setup_mode(false);
        if(!wifi_ready && !wifi_connecting && settings.ssid[0] && now>=next_wifi) {
            wifi_connecting=esp_wifi_connect()==ESP_OK;next_wifi=now+10000;
        }
        if(wifi_ready && complete && !mqtt && now>=next_mqtt) {
            // Certificate validity requires a real clock. Plain local MQTT does not.
            if(settings.tls && time(NULL)<1700000000) message("TIME WAIT");
            else if(start_mqtt()!=ESP_OK) { next_mqtt=now+10000;message("MQTT WAIT"); }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
esp_err_t tc001_wifi_start(void) {
    guard=xSemaphoreCreateMutex();if(!guard) return ESP_ERR_NO_MEM;
    settings.version=2;settings.port=1883;
    bootloader_random_enable();
    nvs_handle_t n;esp_err_t e=nvs_open("tc001_wifi",NVS_READWRITE,&n);if(e!=ESP_OK)return e;
    size_t size=sizeof(settings);
    e=nvs_get_blob(n,"mqtt_settings",&settings,&size);
    if(e==ESP_ERR_NVS_NOT_FOUND) {
        legacy_settings_t legacy={0};size_t old_size=sizeof(legacy);
        esp_err_t old=nvs_get_blob(n,"settings",&legacy,&old_size);
        if(old==ESP_OK && old_size==sizeof(legacy) && legacy.version==1) {
            memcpy(settings.ssid,legacy.ssid,sizeof(settings.ssid));
            memcpy(settings.password,legacy.password,sizeof(settings.password));
        }
        memset(&legacy,0,sizeof(legacy));
    }
    if(e!=ESP_OK && e!=ESP_ERR_NVS_NOT_FOUND) { nvs_close(n);return e; }
    if(e==ESP_OK && (size!=sizeof(settings)||settings.version!=2)) { nvs_close(n);return ESP_ERR_INVALID_VERSION; }
    settings.ssid[32]=0;settings.password[64]=0;settings.broker[127]=0;
    settings.username[64]=0;settings.mqtt_password[128]=0;settings.base_topic[95]=0;
    if(!settings.port) settings.port=1883;
    nvs_close(n);
    uint8_t random_token[16],mac[6];esp_fill_random(random_token,16);
    for(unsigned i=0;i<16;i++) snprintf(setup_token+i*2,3,"%02x",random_token[i]);
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
    // Install the named, open setup AP before radio startup and recovery.
    setup_mode(true);
    if(settings.ssid[0] && settings.broker[0] && settings.base_topic[0]) setup_mode(false);
    ESP_ERROR_CHECK(esp_wifi_start());ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(mdns_init());ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    httpd_handle_t http;httpd_config_t hc=HTTPD_DEFAULT_CONFIG();hc.stack_size=8192;hc.max_uri_handlers=8;
    ESP_ERROR_CHECK(httpd_start(&http,&hc));
    const httpd_uri_t endpoints[]={
        {.uri="/",.method=HTTP_GET,.handler=setup_page},
        {.uri="/api/status",.method=HTTP_GET,.handler=status_handler},
        {.uri="/api/config",.method=HTTP_POST,.handler=config_handler},
        {.uri="/api/wifi",.method=HTTP_GET,.handler=scan_wifi_handler},
        {.uri="/api/battery",.method=HTTP_POST,.handler=battery_handler},
        {.uri="/api/battery",.method=HTTP_GET,.handler=battery_status_handler},
        {.uri="/battery",.method=HTTP_GET,.handler=battery_page}
    };
    for(unsigned i=0;i<sizeof(endpoints)/sizeof(endpoints[0]);i++) ESP_ERROR_CHECK(httpd_register_uri_handler(http,&endpoints[i]));
    if(settings.tls) {
        esp_sntp_config_t ntp=ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        ESP_ERROR_CHECK(esp_netif_sntp_init(&ntp));
    }
    message("WIFI WAIT");
    return xTaskCreate(worker,"tc001_wifi",4096,NULL,4,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
