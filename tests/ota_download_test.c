#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psa/crypto.h"
#include "display_ota.h"

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define HTTP_TIMEOUT_MS 60000
#define DOWNLOAD_BUFFER_SIZE 4096
typedef struct { const char *url; int timeout_ms; void *crt_bundle_attach; bool keep_alive_enable,disable_auto_redirect; int max_redirection_count; } esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
typedef struct { int id; } esp_partition_t;
typedef unsigned esp_ota_handle_t;
static void *esp_crt_bundle_attach;
static const esp_partition_t partition={1};
enum scenario { SUCCESS, OPEN_FAIL, HEADER_FAIL, HTTP_404, NO_PARTITION, BEGIN_FAIL, READ_FAIL, TRUNCATED, WRONG_HASH, WRITE_FAIL, END_FAIL, BOOT_FAIL, NO_MEMORY };
static enum scenario scenario;
static unsigned reads,closed,cleaned,aborted,ended,booted,writes;
static const char payload[]="test firmware bytes";
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) { assert(!strncmp(config->url,"https://",8)); return (void *)1; }
static esp_err_t esp_http_client_open(esp_http_client_handle_t c,int len) { (void)c;(void)len;return scenario==OPEN_FAIL?ESP_FAIL:ESP_OK; }
static int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { (void)c;return scenario==HEADER_FAIL?-1:(int64_t)sizeof(payload); }
static int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c;return scenario==HTTP_404?404:200; }
static void esp_http_client_close(esp_http_client_handle_t c) { (void)c;closed++; }
static void esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c;cleaned++; }
static const esp_partition_t *esp_ota_get_next_update_partition(const void *p) { (void)p;return scenario==NO_PARTITION?NULL:&partition; }
static esp_err_t esp_ota_begin(const esp_partition_t *p,size_t size,esp_ota_handle_t *h) {
  assert(p==&partition&&size==sizeof(payload));
  if (scenario==BEGIN_FAIL) return ESP_FAIL;
  *h=42;return ESP_OK;
}
static int esp_http_client_read(esp_http_client_handle_t c,char *buffer,int cap) {
  (void)c;assert(cap>=(int)sizeof(payload)); reads++;
  if (reads==1) { size_t n=scenario==TRUNCATED?3:sizeof(payload);memcpy(buffer,payload,n);return (int)n; }
  return scenario==READ_FAIL?-1:0;
}
static esp_err_t esp_ota_write(esp_ota_handle_t h,const void *data,size_t size) {
  assert(h==42&&size>0&&data);writes++;return scenario==WRITE_FAIL?ESP_FAIL:ESP_OK;
}
static void esp_ota_abort(esp_ota_handle_t h) { assert(h==42);aborted++; }
static esp_err_t esp_ota_end(esp_ota_handle_t h) { assert(h==42);ended++;return scenario==END_FAIL?ESP_FAIL:ESP_OK; }
static esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { assert(p==&partition&&ended==1);booted++;return scenario==BOOT_FAIL?ESP_FAIL:ESP_OK; }
static void vTaskDelay(unsigned ms) { (void)ms; }
static void *test_malloc(size_t size) { return scenario==NO_MEMORY?NULL:malloc(size); }
#define malloc test_malloc
#include "ota_download.inc"
#undef malloc

int main(void) {
  assert(psa_crypto_init()==PSA_SUCCESS);
  for (scenario=SUCCESS;scenario<=NO_MEMORY;scenario++) {
    reads=closed=cleaned=aborted=ended=booted=writes=0;
    ble_client_update_bundle_t bundle={.size_bytes=sizeof(payload)};
    strcpy(bundle.url,"https://example.invalid/firmware.bin");
    uint8_t digest[32];size_t n=0;
    assert(psa_hash_compute(PSA_ALG_SHA_256,(const uint8_t *)payload,sizeof(payload),digest,sizeof(digest),&n)==PSA_SUCCESS);
    for(size_t i=0;i<32;i++) snprintf(bundle.sha256+2*i,3,"%02x",digest[i]);
    if(scenario==WRONG_HASH) bundle.sha256[0]=bundle.sha256[0]=='a'?'b':'a';
    esp_err_t result=download_and_stage_once(&bundle);
    assert(closed==1&&cleaned==1);
    if(scenario==SUCCESS) assert(result==ESP_OK&&writes==1&&ended==1&&booted==1&&!aborted);
    else {
      assert(result!=ESP_OK);
      assert(booted==(scenario==BOOT_FAIL?1U:0U));
      if(scenario==READ_FAIL||scenario==TRUNCATED||scenario==WRONG_HASH||scenario==WRITE_FAIL||scenario==NO_MEMORY)
        assert(aborted==1&&!ended&&!booted);
    }
  }
  puts("PASS: production OTA download success, HTTP/network failures, truncated/corrupt image, flash failures, cleanup and boot-selection gate");
}
