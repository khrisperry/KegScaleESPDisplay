#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include "controller_link.h"
#include "cJSON.h"
using esp_websocket_client_handle_t = void *;
#include "connection_types.inc"
const char *TAG="test";
const char *esp_err_to_name(esp_err_t) { return "fake"; }
struct esp_app_desc_t { char version[32]; };
const esp_app_desc_t *esp_app_get_description() {
 static const esp_app_desc_t app = {"VTEST"};
 return &app;
}
template<typename... Args> void log_fake(Args...) {}
#define ESP_LOGI(...) log_fake(__VA_ARGS__)
#define ESP_LOGW(...) log_fake(__VA_ARGS__)
#define ESP_LOGE(...) log_fake(__VA_ARGS__)
#define ESP_LOGD(...) log_fake(__VA_ARGS__)
#define pdMS_TO_TICKS(x) (x)
constexpr int pdTRUE=1, MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2;
constexpr int WIFI_EVENT=1, IP_EVENT=2, WIFI_EVENT_STA_START=1,
 WIFI_EVENT_STA_CONNECTED=2, WIFI_EVENT_STA_DISCONNECTED=3, IP_EVENT_STA_GOT_IP=4;
constexpr int WEBSOCKET_EVENT_CONNECTED=1, WEBSOCKET_EVENT_DISCONNECTED=2,
 WEBSOCKET_EVENT_ERROR=3, WEBSOCKET_EVENT_DATA=4, WEBSOCKET_EVENT_ANY=5;
using esp_event_base_t=int;
struct wifi_event_sta_disconnected_t { unsigned reason; };
struct esp_websocket_event_data_t { int op_code, payload_offset, payload_len, data_len; const char *data_ptr; };
struct esp_websocket_client_config_t {
 const char *uri; int buffer_size, task_stack; bool disable_auto_reconnect;
 int reconnect_timeout_ms, network_timeout_ms;
 size_t ping_interval_sec; int pingpong_timeout_sec;
};
Settings settings{};
StoredScaleProfile secondary_scale{};
ScaleConnection connections[2];
uint8_t active_scale_index;
std::atomic<bool> wifi_ready{true};
constexpr int64_t kReconnectRetryUs=10000000;
int frames;
int64_t clock_us=1000000;
unsigned stops, destroys, retirements, probes, starts, clears, results, ended, paired_ui, state_ui, saves, wifi_connects;
bool probe_ok=true, init_ok=true, socket_connected=true;
esp_err_t start_error=ESP_OK, save_error=ESP_OK, open_error=ESP_OK;
std::string message, result_error, last_hello;
std::vector<Frame> queued;
void *registered_token;
ScaleConnection &connection_for(uint8_t slot) { return connections[slot]; }
const ScaleConnection &connection_for_const(uint8_t slot) { return connections[slot]; }
const char *scale_host_const(uint8_t slot) { return slot?secondary_scale.host:settings.host; }
bool &scale_paired(uint8_t slot) { return slot?secondary_scale.paired:settings.paired; }
uint8_t *scale_master(uint8_t slot) { return slot?secondary_scale.master:settings.master; }
int64_t now() { return clock_us; }
void ui_state(const State &) { ++state_ui; }
void ui_message(const char *s) { message=s; }
void ui_result(bool, const char *, const char *s) { ++results; result_error=s; }
void ui_pair_code(const char *) {}
void ui_paired() { ++paired_ui; }
void touchscreen_pairing_window(uint32_t) {}
void touchscreen_pairing_ended() { ++ended; }
void publish_scale_profiles() {}
esp_err_t persist() { ++saves; return save_error; }
esp_err_t esp_wifi_connect() { ++wifi_connects; return ESP_OK; }
void esp_websocket_client_stop(void *) { ++stops; }
void esp_websocket_client_destroy(void *) { ++destroys; }
bool retire_transport_async(uint8_t slot, void *, uint32_t) {
 ++retirements;
 connections[slot].retirement_pending = true;
 return true;
}
bool esp_websocket_client_is_connected(void *) { return socket_connected; }
void *esp_websocket_client_init(const esp_websocket_client_config_t *c) {
 assert(c->disable_auto_reconnect); return init_ok?reinterpret_cast<void *>(1):nullptr;
}
void esp_websocket_register_events(void *, int, void (*)(void *,int,int,void *), void *arg) { registered_token=arg; }
esp_err_t esp_websocket_client_start(void *) { ++starts; return start_error; }
int esp_websocket_client_send_text(void *, const char *s, size_t len, int) { last_hello=s; return len; }
bool scale_accepting_connection(uint8_t) { ++probes; return probe_ok; }
bool send_secure(uint8_t, const char *) { return true; }
int xQueueSend(int, const Frame *f, int) { queued.push_back(*f); return pdTRUE; }
void esp_fill_random(void *p, size_t n) { memset(p, 1, n); }
size_t heap_caps_get_free_size(int) { return 100000; }
size_t heap_caps_get_largest_free_block(int) { return 100000; }
void cl_clear(cl_session_t *s) { ++clears; memset(s,0,sizeof(*s)); }
void cl_hex(const uint8_t *, size_t n, char *out) { memset(out,'1',n*2); out[n*2]=0; }
bool cl_unhex(const char *, uint8_t *out, size_t n) { memset(out,1,n); return true; }
esp_err_t cl_keypair(cl_session_t *, char *out) { strcpy(out,"public"); return ESP_OK; }
esp_err_t cl_agree(cl_session_t *,const char *,const char *,const char *,char *out) { strcpy(out,"ABC123"); return ESP_OK; }
esp_err_t cl_start(cl_session_t *, const uint8_t *,const uint8_t *,bool) { return ESP_OK; }
esp_err_t cl_open(cl_session_t *,const uint8_t *in,size_t n,char *out) { memcpy(out,in,n);out[n]=0;return open_error; }

/*
 * main.cpp V1.3.6 calls these wrapper-free runtime hooks explicitly.
 * Connection tests intentionally keep their original scope: UI messaging is a
 * fake, and encrypted-frame opening is the fake cl_open() above. The separate
 * main_architecture_guard_test.py verifies that production main.cpp is wired to
 * the real explicit hooks.
 */
void touchscreen_ui_message(const char *s) { ui_message(s); }
esp_err_t touchscreen_pairing_cl_open(cl_session_t *session,
                                      const uint8_t *in, size_t n,
                                      char *out) {
  return cl_open(session, in, n, out);
}

#include "connection_functions.inc"

static void reset() {
 for(auto &c: connections) { c.~ScaleConnection(); new (&c) ScaleConnection(); }
 settings={}; secondary_scale={}; strcpy(settings.host,"scale-one"); strcpy(secondary_scale.host,"scale-two");
 active_scale_index=0; wifi_ready=true; clock_us=1000000;
 stops=destroys=retirements=probes=starts=clears=results=ended=paired_ui=state_ui=saves=wifi_connects=0;
 probe_ok=init_ok=socket_connected=true; start_error=save_error=open_error=ESP_OK;
 message.clear();result_error.clear();last_hello.clear();queued.clear();
}
static Frame frame(uint8_t slot,int kind,const char *json="") {
 Frame f{};f.slot=slot;f.generation=connections[slot].generation;f.kind=kind;
 f.length=strlen(json);memcpy(f.bytes,json,f.length);return f;
}
static void test_reconnect() {
 reset(); auto &c=connections[0]; connect_scale(0); assert(probes==1&&starts==1);
 on_frame(frame(0,1));assert(last_hello.find("public")!=std::string::npos);
 assert(last_hello.find("\"firmware\":\"VTEST\"")!=std::string::npos);
 c.traffic_ready=true; c.link.master[0]=42;
 on_frame(frame(0,2));assert(!c.traffic_ready&&!c.authenticated&&ended==1&&c.retry_connection);
 assert(c.next_connection_attempt==clock_us+kReconnectRetryUs&&c.link.master[0]==0);
 connect_scale(0); assert(retirements==1&&stops==0&&destroys==0&&starts==1&&c.retirement_pending);
 c.retirement_pending=false; connect_scale(0); assert(starts==2);
 c.link.master[0]=42;on_frame(frame(0,4,"{\"type\":\"authorized\"}"));
 assert(settings.paired&&settings.master[0]==42&&c.authenticated&&saves==1);
 on_frame(frame(0,2));connect_scale(0);assert(probes==2); // saved pairing skips HTTP
 on_frame(frame(0,1));assert(last_hello.find("public")==std::string::npos&&c.link.master[0]==42);
 assert(last_hello.find("\"firmware\":\"VTEST\"")!=std::string::npos);
 reset();connect_scale(1);assert(probes==0&&starts==0&&!connections[1].retry_connection);
 secondary_scale.paired=true;connect_scale(1);assert(starts==1&&probes==0);
 active_scale_index=1;settings.paired=true;connect_scale(0);assert(starts==2&&probes==0);
 reset();probe_ok=false;connect_scale(0);assert(starts==0&&connections[0].retry_connection);
 reset();wifi_ready=false;connect_scale(0);assert(starts==0&&probes==0);
 reset();init_ok=false;connect_scale(0);assert(!connections[0].ws&&connections[0].retry_connection);
 reset();start_error=ESP_FAIL;connect_scale(0);assert(connections[0].retry_connection);
 puts("PASS: interrupted pairing, saved reconnect, independent scale slots and connection failures");
}
static void test_stale_and_cancel() {
 reset();connect_scale(0);Frame old=frame(0,4,"{\"type\":\"authorized\"}");void *old_token=registered_token;
 stop_scale_transport(0,false);assert(connections[0].retirement_pending);
 on_frame(old);assert(!settings.paired&&saves==0);
 socket_event(old_token,0,WEBSOCKET_EVENT_CONNECTED,nullptr);assert(queued.empty());
 connections[0].retirement_pending=false;connect_scale(0);
 socket_event(registered_token,0,WEBSOCKET_EVENT_CONNECTED,nullptr);assert(queued.size()==1);
 on_frame(frame(0,4,"{\"type\":\"pairing_canceled\"}"));
 assert(!connections[0].ws&&!connections[0].retry_connection&&ended==1);
 reset();connect_scale(0);connections[0].cancel_pairing_deadline_us=clock_us+1000000;
 on_frame(frame(0,2));assert(!connections[0].retry_connection&&ended==1);
 reset();settings.paired=true;connect_scale(0);
 Frame removed=frame(0,4,"{\"type\":\"authorized\"}");
 stop_scale_transport(0,false);settings.paired=false;memset(settings.master,0,32);
 connections[0].retirement_pending=false;connect_scale(0);on_frame(removed);assert(!settings.paired&&saves==0);
 on_frame(frame(0,1));assert(last_hello.find("public")!=std::string::npos);
 reset();connect_scale(0);save_error=ESP_FAIL;
 on_frame(frame(0,4,"{\"type\":\"authorized\"}"));assert(!settings.paired&&!connections[0].authenticated&&paired_ui==0);
 puts("PASS: stale callback/queued frame rejection, cancellation and failed pairing persistence");
}
static void test_lost_ack_and_ota() {
 reset();settings.paired=true;auto &c=connections[0];c.ws=reinterpret_cast<void *>(1);
 c.authenticated=c.traffic_ready=c.state.online=true;c.pending_id=7;strcpy(c.pending_op,"save");
 assert(ota_scale_transport_busy());
 on_frame(frame(0,4,"{\"type\":\"result\",\"id\":6,\"ok\":true}"));assert(c.pending_id==7&&results==0);
 on_frame(frame(0,2));assert(c.pending_id==0&&results==1&&result_error.find("Outcome unknown")!=std::string::npos);
 assert(ota_scale_transport_busy());
 c.authenticated=true;on_frame(frame(0,4,"{\"type\":\"result\",\"id\":7,\"ok\":true}"));assert(results==1);
 c.state.online=true;assert(!ota_scale_transport_busy());
 c.pending_id=8;on_frame(frame(0,4,"{\"type\":\"result\",\"id\":8,\"ok\":true}"));assert(results==2&&!c.pending_id);
 wifi_ready=false;assert(ota_scale_transport_busy());
 reset();assert(!ota_scale_transport_busy());connections[0].ws=reinterpret_cast<void *>(1);assert(ota_scale_transport_busy());
 reset();settings.paired=true;wifi_event(nullptr,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,nullptr);
 assert(!wifi_ready&&wifi_connects==1&&connections[0].retry_connection&&!connections[1].retry_connection);
 wifi_event(nullptr,IP_EVENT,IP_EVENT_STA_GOT_IP,nullptr);assert(wifi_ready&&connections[0].next_connection_attempt==clock_us);
 puts("PASS: lost/mismatched/late acknowledgements, OTA reconnect gate and Wi-Fi retry scheduling");
}

static void test_keepalive_capability() {
 assert(!scale_supports_protocol_keepalive(""));
 assert(!scale_supports_protocol_keepalive("V1.3.10"));
 assert(scale_supports_protocol_keepalive("V1.3.11"));
 assert(scale_supports_protocol_keepalive("V1.4.0"));
 assert(scale_supports_protocol_keepalive("V2.0.0"));
 puts("PASS: protocol keepalive capability preserves older Scale fallback");
}

static void test_error_without_disconnect_retries() {
 reset();settings.paired=true;connect_scale(0);
 assert(starts==1&&!connections[0].retry_connection);
 queued.clear();
 socket_event(registered_token,0,WEBSOCKET_EVENT_ERROR,nullptr);
 assert(queued.size()==1&&queued[0].kind==2);
 socket_event(registered_token,0,WEBSOCKET_EVENT_DISCONNECTED,nullptr);
 assert(queued.size()==1);
 on_frame(queued[0]);
 assert(connections[0].retry_connection);
 assert(connections[0].next_connection_attempt==clock_us+kReconnectRetryUs);
 puts("PASS: WebSocket ERROR without DISCONNECTED still schedules Scale retry");
}

static void test_retirement_is_slot_local() {
 reset();
 settings.paired=true;
 secondary_scale.paired=true;
 connections[1].retirement_pending=true;
 connect_scale(0);
 assert(starts==1);
 assert(!connections[0].retirement_pending);
 assert(connections[1].retirement_pending);
 puts("PASS: retiring Scale transport does not block the other Scale slot");
}

static void test_fragments_and_slot_isolation() {
 reset();connect_scale(0);void *token=registered_token;
 esp_websocket_event_data_t d{1,0,4,2,"ab"};
 socket_event(token,0,WEBSOCKET_EVENT_DATA,&d);assert(queued.empty());
 d.payload_offset=2;d.data_ptr="cd";
 socket_event(token,0,WEBSOCKET_EVENT_DATA,&d);
 assert(queued.size()==1&&queued[0].length==4&&!memcmp(queued[0].bytes,"abcd",4));
 queued.clear();d.payload_offset=0;d.payload_len=CL_MAX_FRAME+1;
 socket_event(token,0,WEBSOCKET_EVENT_DATA,&d);assert(queued.empty());
 d.payload_len=4;d.payload_offset=3;
 socket_event(token,0,WEBSOCKET_EVENT_DATA,&d);assert(queued.empty());
 settings.paired=true;connections[0].authenticated=connections[0].state.online=true;
 active_scale_index=1;connect_scale(1);
 unsigned before=state_ui; on_frame(frame(0,2));assert(state_ui==before&&ended==0);
 assert(connections[1].ws&&connections[1].generation==1);
 reset();settings.paired=true;auto &c=connections[0];
 c.authenticated=c.state.online=true;c.pending_id=19;strcpy(c.pending_op,"calibrate");
 wifi_event(nullptr,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,nullptr);
 on_frame(frame(0,2));assert(results==1&&!c.pending_id&&!c.authenticated);
 wifi_event(nullptr,IP_EVENT,IP_EVENT_STA_GOT_IP,nullptr);
 connect_scale(0);assert(starts==1&&results==1&&!c.pending_id); // no command replay
 c.authenticated=true;c.pending_id=20;
 on_frame(frame(0,3,"{\"type\":\"result\",\"id\":20,\"ok\":true}"));
 assert(c.pending_id==20); // plaintext cannot acknowledge a secure command
 open_error=ESP_FAIL;
 on_frame(frame(0,4,"{\"type\":\"result\",\"id\":20,\"ok\":true}"));
 assert(c.pending_id==20);
 puts("PASS: WebSocket fragments, slot isolation, Wi-Fi loss without command replay and authenticated results");
}
int main() { test_reconnect();test_stale_and_cancel();test_lost_ack_and_ota();test_keepalive_capability();test_error_without_disconnect_retries();test_retirement_is_slot_local();test_fragments_and_slot_isolation(); }
