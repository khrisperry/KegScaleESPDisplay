#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ble_client.h"
#include "power_policy.h"
#include "wake_types.inc"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_ERROR_CHECK(e) assert((e)==ESP_OK)
#define ESP_ERR_INVALID_VERSION 0x110
#define pdMS_TO_TICKS(n) (n)
#define BIT(n) (1U<<(n))
#define CONFIG_KEG_DISPLAY_TOUCH_WAKE 1
#define CONFIG_KEG_DISPLAY_SLEEP_SECONDS 180
#define CONFIG_KEG_DISPLAY_SAFETY_WAKE_SECONDS 3600
#define ESP_SLEEP_WAKEUP_ALL 0
#define ESP_SLEEP_WAKEUP_TIMER 1
#define ESP_SLEEP_WAKEUP_TOUCHPAD 2
typedef int esp_sleep_source_t;
typedef struct { ble_client_peer_t peer; } pairing_config_t;
static retained_state_t s_retained;
static uint8_t s_touch_threshold_percent=15;
static bool s_periodic_checkin_enabled, s_lightweight_fetch;
static uint32_t s_scale_screen_magic;
static unsigned causes, disables, touch_arms, sleeps, resets, renders, notices, clears;
static unsigned fetches, saves, forgot, pairing_clears, pairing_starts, full, partial, panel_sleep, command_acks;
static uint64_t timer_us;
static bool compatible=true, forced, fetched_full;
static esp_err_t screen_error, fetch_error, save_error, command_ack_error;
static unsigned fail_fetches;
static size_t strlcpy(char *d,const char *s,size_t n) { size_t len=strlen(s);snprintf(d,n,"%s",s);return len; }
static unsigned esp_sleep_get_wakeup_causes(void) { return causes; }
static esp_err_t esp_sleep_disable_wakeup_source(int source) { assert(source==ESP_SLEEP_WAKEUP_ALL);disables++;touch_arms=0;return ESP_ERR_INVALID_STATE; }
static esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us) { assert(disables);timer_us=us;return ESP_OK; }
static esp_err_t touch_wake_prepare(uint8_t threshold) { assert(threshold==15&&timer_us);touch_arms++;return ESP_OK; }
static void esp_deep_sleep_start(void) { sleeps++; }
static void pairing_reset_power_cycle_count(void) { resets++; }
static void vTaskDelay(unsigned ms) { assert(ms==750); }
static void apply_display_settings(const ble_client_scale_state_t *s) { (void)s; }
static esp_err_t display_ui_show_message(const char *a,const char *b,const char *c) { (void)a;(void)b;(void)c;notices++;return screen_error; }
static esp_err_t display_ui_show_scale(const ble_client_peer_t *p,const ble_client_scale_state_t *s,uint8_t b,bool force) { (void)p;(void)s;(void)b;renders++;forced=force;return screen_error; }
static esp_err_t display_ui_clear_touch_acknowledged(void) { clears++;return screen_error; }
esp_err_t ble_client_fetch(const ble_client_peer_t *p,ble_client_scale_state_t *s) { (void)p;(void)s;fetches++;return fetches<=fail_fetches?ESP_FAIL:fetch_error; }
esp_err_t ble_client_fetch_mode(const ble_client_peer_t *p,ble_client_scale_state_t *s,bool full_read) { fetched_full=full_read;return ble_client_fetch(p,s); }
bool ble_client_state_is_compatible(const ble_client_peer_t *p,const ble_client_scale_state_t *s) { (void)p;(void)s;return compatible; }
static esp_err_t pairing_save(const ble_client_peer_t *p) { (void)p;saves++;return save_error; }
esp_err_t ble_client_forget_peer(const ble_client_peer_t *p) { (void)p;forgot++;return ESP_OK; }
esp_err_t ble_client_acknowledge_control(const ble_client_peer_t *p,uint16_t id,uint8_t flags) {
 (void)p;assert(id);assert(flags);command_acks++;return command_ack_error;
}
static esp_err_t pairing_clear(void) { pairing_clears++;return ESP_OK; }
static void enter_pairing_mode(void) { pairing_starts++; }
static void clear_touch_ack_area(void) {}
static esp_err_t epaper_refresh_changed(unsigned interval) { assert(interval==SCALE_FULL_REFRESH_INTERVAL);partial++;return screen_error; }
static esp_err_t epaper_refresh(void) { full++;return screen_error; }
static esp_err_t epaper_sleep(void) { panel_sleep++;return ESP_OK; }
#include "wake_functions.inc"

int main(void) {
 pairing_config_t pairing={0};strcpy(pairing.peer.scale_id,"scale-one");
 ble_client_scale_state_t state={0};state.flags=BLE_SCALE_FLAG_STABLE;state.total_weight_lbs=30;
 assert(!is_touch_wake()&&!strcmp(wake_reason(),"power/reset"));
 causes=BIT(ESP_SLEEP_WAKEUP_TIMER);assert(!is_touch_wake()&&!strcmp(wake_reason(),"timer"));
 causes|=BIT(ESP_SLEEP_WAKEUP_TOUCHPAD);assert(is_touch_wake()&&!strcmp(wake_reason(),"touch"));
 causes=BIT(5);assert(!strcmp(wake_reason(),"other"));
 for(unsigned frequent=0;frequent<2;frequent++) {
  s_periodic_checkin_enabled=frequent;
  for(unsigned failures=0;failures<256;failures++) {
   s_retained.consecutive_scale_failures=failures;configure_wake_sources();
   assert(touch_arms==1&&timer_us==1000000ULL*power_next_check_seconds(frequent,failures,180,3600));
  }
 }
 sleep_for_touch_delay(10);assert(timer_us==10000000&&!touch_arms&&sleeps==1&&resets==1);
 memset(&s_retained,0,sizeof(s_retained));
 assert(render_if_needed(&pairing.peer,&state,80)&&renders==1);
 assert(!render_if_needed(&pairing.peer,&state,80));
 state.total_weight_lbs=31;state.flags=0;assert(!render_if_needed(&pairing.peer,&state,80));
 state.force_refresh_requested=true;assert(render_if_needed(&pairing.peer,&state,80)&&forced&&!command_acks);
 state.command_ack_supported=true;state.control_command_id=17;state.control_flags=4;
 assert(render_if_needed(&pairing.peer,&state,80)&&command_acks==1);
 command_ack_error=ESP_FAIL;assert(render_if_needed(&pairing.peer,&state,80)&&command_acks==2);
 command_ack_error=ESP_OK;
 state.force_refresh_requested=false;state.command_ack_supported=false;state.control_command_id=0;state.control_flags=0;state.flags=BLE_SCALE_FLAG_STABLE;
 state.sequence++;screen_error=ESP_FAIL;assert(!render_if_needed(&pairing.peer,&state,80)&&s_retained.sequence!=state.sequence);
 screen_error=ESP_OK;assert(render_if_needed(&pairing.peer,&state,80));
 state.total_weight_lbs+=.49f;assert(!should_refresh(&pairing.peer,&state,80));
 state.total_weight_lbs+=.02f;assert(should_refresh(&pairing.peer,&state,80));
 remember_displayed_state(&pairing.peer,&state,80);
 state.profile_revision++;assert(should_refresh(&pairing.peer,&state,80));remember_displayed_state(&pairing.peer,&state,80);
 state.display_config_revision++;assert(should_refresh(&pairing.peer,&state,80));remember_displayed_state(&pairing.peer,&state,80);
 state.remaining_servings++;assert(should_refresh(&pairing.peer,&state,80));remember_displayed_state(&pairing.peer,&state,80);
 assert(should_refresh(&pairing.peer,&state,79));
 for(unsigned i=0;i<SCALE_OFFLINE_FAILURE_THRESHOLD;i++)assert(!record_scale_check_failure(&pairing.peer));
 screen_error=ESP_FAIL;assert(!record_scale_check_failure(&pairing.peer)&&!s_retained.scale_offline_displayed);
 screen_error=ESP_OK;assert(record_scale_check_failure(&pairing.peer));unsigned shown=notices;
 for(unsigned i=0;i<300;i++)assert(!record_scale_check_failure(&pairing.peer));
 assert(notices==shown&&s_retained.consecutive_scale_failures==255);
 remember_runtime_settings(&pairing.peer);assert(!s_retained.consecutive_scale_failures);
 state.flags=0;assert(render_if_needed(&pairing.peer,&state,80)&&!s_retained.scale_offline_displayed);
 clear_touch_acknowledgement_if_needed(false,false);clear_touch_acknowledgement_if_needed(true,true);assert(!clears);
 clear_touch_acknowledgement_if_needed(true,false);assert(clears==1);
 assert(present_scale(false)==ESP_OK&&full==1&&panel_sleep==1);
 assert(present_scale(false)==ESP_OK&&partial==1);
 assert(present_scale(true)==ESP_OK&&full==2);
 s_scale_screen_magic=0;screen_error=ESP_FAIL;unsigned slept=panel_sleep;
 assert(present_scale(false)==ESP_FAIL&&!s_scale_screen_magic&&panel_sleep==slept);screen_error=ESP_OK;
 fail_fetches=1;assert(validate_and_save_peer(&pairing.peer,&state)==ESP_OK&&fetches==2&&saves==1);
 fetches=0;fail_fetches=2;assert(validate_and_save_peer(&pairing.peer,&state)==ESP_FAIL&&fetches==2&&saves==1);
 fail_fetches=0;compatible=false;assert(validate_and_save_peer(&pairing.peer,&state)==ESP_ERR_INVALID_VERSION&&saves==1);
 compatible=true;save_error=ESP_FAIL;assert(validate_and_save_peer(&pairing.peer,&state)==ESP_FAIL);save_error=ESP_OK;
 s_retained.consecutive_scale_failures=3;s_lightweight_fetch=true;
 assert(fetch_paired_state_once(&pairing,&state)==ESP_OK&&!fetched_full&&!s_retained.consecutive_scale_failures);
 s_lightweight_fetch=false;assert(fetch_paired_state_once(&pairing,&state)==ESP_OK&&fetched_full);
 assert(!handle_unpair_request(&pairing,&state));state.unpair_requested=true;
 state.command_ack_supported=true;state.control_command_id=33;state.control_flags=1;
 command_ack_error=ESP_FAIL;
 assert(!handle_unpair_request(&pairing,&state)&&forgot==0&&pairing_clears==0&&pairing_starts==0);
 command_ack_error=ESP_OK;
 assert(handle_unpair_request(&pairing,&state)&&command_acks==4&&forgot==1&&pairing_clears==1&&pairing_starts==1&&!s_retained.magic);
 puts("PASS: production wake configuration/backoff, timer-only pour delay, refresh retention, reliable command ACK retry, offline recovery, partial/full selection, pairing retry and unpair transition");
 return 0;
}
