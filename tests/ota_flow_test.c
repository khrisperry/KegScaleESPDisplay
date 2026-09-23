#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble_client.h"
#include "display_ota.h"
#include "ota_authorization_policy.h"

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define KEY_OTA_SCREEN_PENDING "ota_pending"
#define pdMS_TO_TICKS(n) (n)
typedef struct { ble_client_peer_t peer; } pairing_config_t;
typedef struct { char version[33]; } esp_app_desc_t;
static const esp_app_desc_t app = {.version="V1.3.2"};
static ble_client_update_bundle_t supplied;
static esp_err_t fetch_result, install_result;
static unsigned fetches, installs, progress, failures, renders, frees;
static bool pending, fail_alloc;
static jmp_buf restart;
static const esp_app_desc_t *esp_app_get_description(void) { return &app; }
static void vTaskDelay(unsigned ms) { (void)ms; }
static void esp_restart(void) { assert(installs == 1 && progress == 1 && frees == 1); longjmp(restart,1); }
static esp_err_t set_display_state_flag(const char *key, bool value) {
  assert(!strcmp(key, KEY_OTA_SCREEN_PENDING)); pending=value; return ESP_OK;
}
static esp_err_t display_ui_show_message(const char *title,const char *message,const char *detail) {
  (void)title; (void)detail;
  if (!strcmp(message,"UPDATE IN PROGRESS")) { assert(pending && !installs); progress++; }
  else { assert(!strcmp(message,"UPDATE FAILED") && installs==1); failures++; }
  return ESP_OK;
}
esp_err_t ble_client_fetch_update_bundle(const ble_client_peer_t *peer,ble_client_update_bundle_t *bundle) {
  (void)peer; fetches++; assert(!progress); *bundle=supplied; return fetch_result;
}
esp_err_t display_ota_install(ble_client_update_bundle_t *bundle) {
  assert(bundle->ssid[0] && pending && progress==1); installs++; return install_result;
}
static void render_if_needed(const ble_client_peer_t *peer,ble_client_scale_state_t *state,uint8_t battery) {
  (void)peer; (void)battery; assert(!pending && state->force_refresh_requested); renders++;
}
static void *test_calloc(size_t count,size_t size) { return fail_alloc ? NULL : calloc(count,size); }
static void test_free(void *ptr) {
  const unsigned char *p=ptr;
  for (size_t i=0;i<sizeof(ble_client_update_bundle_t);i++) assert(p[i]==0);
  frees++; free(ptr);
}
#define calloc test_calloc
#define free test_free
#include "ota_flow.inc"
#undef calloc
#undef free

static ble_client_scale_state_t state;
static const pairing_config_t pairing={0};
static void reset(void) {
  memset(&state,0,sizeof(state)); memset(&supplied,0,sizeof(supplied));
  state.update.valid=true; state.update.size_bytes=100;
  strcpy(state.update.hardware,DISPLAY_OTA_HARDWARE_ID); strcpy(state.update.version,"V1.3.3");
  memset(state.update.sha256,'a',64);
  supplied.size_bytes=100; strcpy(supplied.hardware,state.update.hardware);
  strcpy(supplied.version,state.update.version); strcpy(supplied.sha256,state.update.sha256);
  strcpy(supplied.ssid,"test-network"); strcpy(supplied.password,"test-password");
  fetches=installs=progress=failures=renders=frees=0; pending=fail_alloc=false;
  fetch_result=ESP_OK; install_result=ESP_FAIL;
}
static void run(void) { install_display_update_if_needed(&pairing,&state,80); }
int main(void) {
  reset(); fetch_result=ESP_ERR_TIMEOUT; run();
  assert(fetches==1&&!installs&&!progress&&!pending&&frees==1);
  reset(); strcpy(state.update.hardware,"incompatible"); run(); assert(!fetches&&!progress);
  reset(); strcpy(state.update.version,app.version); run(); assert(!fetches&&!progress);
  reset(); state.update.valid=false; run(); assert(!fetches&&!progress);
  reset(); state.update.size_bytes=0; run(); assert(!fetches&&!progress);
  reset(); supplied.size_bytes++; run(); assert(fetches==1&&!installs&&!progress&&frees==1);
  reset(); fail_alloc=true; run(); assert(!fetches&&!progress&&!frees);
  // Wi-Fi timeout and HTTP/download failure must restore the ordinary display.
  for (int i=0;i<2;i++) {
    reset(); install_result=i?ESP_FAIL:ESP_ERR_TIMEOUT; run();
    assert(installs==1&&progress==1&&failures==1&&renders==1&&!pending&&frees==1);
  }
  reset(); install_result=ESP_OK;
  if (setjmp(restart)==0) { run(); assert(!"Successful installation must restart"); }
  assert(pending&&!failures&&!renders&&frees==1);
  puts("PASS: production OTA flow authorization, compatibility, failure screen recovery, bundle wiping, successful restart");
}
