#include "battery.h"
#include "battery_policy.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include <stdlib.h>

static const char *TAG="tc001_battery";
static adc_oneshot_unit_handle_t adc;
static portMUX_TYPE guard=portMUX_INITIALIZER_UNLOCKED;
static tc001_battery_t reading={.raw=-1,.percent=-1,
    .empty=TC001_BATTERY_EMPTY_DEFAULT,.full=TC001_BATTERY_FULL_DEFAULT};
static int64_t next_sample;
static bool filter_ready;
static int filtered;
static int compare(const void *a,const void *b) { return *(const int *)a-*(const int *)b; }
void tc001_battery_get(tc001_battery_t *out) {
    portENTER_CRITICAL(&guard);*out=reading;portEXIT_CRITICAL(&guard);
    if(esp_timer_get_time()/1000-out->sampled_ms>10000) out->valid=false;
}
esp_err_t tc001_battery_start(void) {
    nvs_handle_t n;
    if(nvs_open("tc001_battery",NVS_READONLY,&n)==ESP_OK) {
        uint16_t points[2]={0};size_t size=sizeof(points);
        esp_err_t err=nvs_get_blob(n,"cal",points,&size);nvs_close(n);
        if(err==ESP_OK && size==sizeof(points) && tc001_battery_calibration_valid(points[0],points[1])) {
            reading.empty=points[0];reading.full=points[1];
        }
    }
    // ADC1 is usable alongside Wi-Fi; GPIO34 is ADC1 channel 6.
    // Read at 10 bits/12 dB, matching the raw scale used by TC001/AWTRIX mappings.
    adc_oneshot_unit_init_cfg_t unit={.unit_id=ADC_UNIT_1,.ulp_mode=ADC_ULP_MODE_DISABLE};
    esp_err_t err=adc_oneshot_new_unit(&unit,&adc);if(err!=ESP_OK)return err;
    adc_oneshot_chan_cfg_t channel={.atten=ADC_ATTEN_DB_12,.bitwidth=ADC_BITWIDTH_10};
    err=adc_oneshot_config_channel(adc,ADC_CHANNEL_6,&channel);
    if(err!=ESP_OK) { adc_oneshot_del_unit(adc);adc=NULL; }
    return err;
}
void tc001_battery_poll(int64_t now) {
    if(!adc || now<next_sample) return;
    next_sample=now+2000;
    int values[16],unused;
    esp_err_t err=adc_oneshot_read(adc,ADC_CHANNEL_6,&unused);
    for(unsigned i=0;i<16 && err==ESP_OK;i++) err=adc_oneshot_read(adc,ADC_CHANNEL_6,&values[i]);
    int raw=-1;
    if(err==ESP_OK) { qsort(values,16,sizeof(values[0]),compare);raw=(values[7]+values[8]+1)/2; }
    bool plausible=tc001_battery_raw_valid(raw);
    if(plausible) {
        filtered=filter_ready?(filtered*3+raw*256+2)/4:raw*256;
        filter_ready=true;
    } else filter_ready=false;
    portENTER_CRITICAL(&guard);
    reading.raw=plausible?(filtered+128)/256:raw;
    reading.percent=tc001_battery_percent(reading.raw,reading.empty,reading.full);
    reading.valid=err==ESP_OK && reading.percent>=0;reading.sampled_ms=now;
    portEXIT_CRITICAL(&guard);
    if(err!=ESP_OK) ESP_LOGW(TAG,"Battery ADC read failed: %s",esp_err_to_name(err));
}
esp_err_t tc001_battery_calibrate(int empty,int full) {
    if(!tc001_battery_calibration_valid(empty,full)) return ESP_ERR_INVALID_ARG;
    // Setup task only. Store both endpoints together; never do NVS I/O in a critical section.
    nvs_handle_t n;esp_err_t err=nvs_open("tc001_battery",NVS_READWRITE,&n);
    if(err!=ESP_OK)return err;
    uint16_t points[2]={(uint16_t)empty,(uint16_t)full};
    err=nvs_set_blob(n,"cal",points,sizeof(points));
    if(err==ESP_OK)err=nvs_commit(n);
    nvs_close(n);if(err!=ESP_OK)return err;
    portENTER_CRITICAL(&guard);
    reading.empty=empty;reading.full=full;
    reading.percent=tc001_battery_percent(reading.raw,empty,full);
    reading.valid=reading.percent>=0;
    portEXIT_CRITICAL(&guard);
    return ESP_OK;
}
