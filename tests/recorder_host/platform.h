#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
typedef int esp_err_t;
#define ESP_OK 0
#define DRAM_ATTR
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef int portMUX_TYPE;
extern uint32_t test_free, test_min, test_largest;
extern int64_t test_time;
extern bool test_isr;
static inline int64_t esp_timer_get_time(void) { return test_time; }
static inline unsigned heap_caps_get_free_size(unsigned x) { (void)x; return test_free; }
static inline unsigned heap_caps_get_minimum_free_size(unsigned x) { (void)x; return test_min; }
static inline unsigned heap_caps_get_largest_free_block(unsigned x) { (void)x; return test_largest; }
static inline void *xTaskGetCurrentTaskHandle(void) { return (void *)0x1234; }
static inline bool xPortInIsrContext(void) { return test_isr; }
typedef struct {uint32_t pc, sp, next_pc; void *exc_frame;} esp_backtrace_frame_t;
static inline void esp_backtrace_get_start(uint32_t *pc,uint32_t *sp,uint32_t *next) { *pc=0x42000000; *sp=0; *next=0x42001000; }
static inline bool esp_backtrace_get_next_frame(esp_backtrace_frame_t *f) { f->pc+=4; return f->pc < 0x42000030; }
typedef void *esp_timer_handle_t;
typedef struct {void (*callback)(void *); const char *name; bool skip_unhandled_events;} esp_timer_create_args_t;
static inline int esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *t) { (void)a; *t=(void *)1; return ESP_OK; }
static inline int esp_timer_start_periodic(esp_timer_handle_t t,uint64_t us) { (void)t;(void)us;return ESP_OK; }
static inline int heap_caps_register_failed_alloc_callback(void (*f)(size_t,uint32_t,const char *)) { (void)f;return ESP_OK; }
typedef struct {int rssi;unsigned primary;unsigned char bssid[6];} wifi_ap_record_t;
static inline int esp_wifi_sta_get_ap_info(wifi_ap_record_t *a) { (void)a;return -1; }
static inline void test_log(const char *tag,const char *fmt,...) { (void)tag; (void)fmt; }
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
