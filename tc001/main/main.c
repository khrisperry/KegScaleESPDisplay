#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "led_strip.h"
#include "ble_client.h"
#include "pairing.h"
#include "pixel_ui.h"
#include "sdkconfig.h"

static const char *TAG="tc001";
static SemaphoreHandle_t lock;
static struct {
    pixel_state_t state;
    char message[64];
    int64_t message_start, last_read;
    bool reset;
    uint32_t rendered;
} shared;
static int64_t now_ms(void) { return esp_timer_get_time()/1000; }
static void message(const char *s) {
    xSemaphoreTake(lock,portMAX_DELAY);
    if(strcmp(shared.message,s)) { snprintf(shared.message,sizeof(shared.message),"%s",s); shared.message_start=now_ms(); }
    xSemaphoreGive(lock);
}
#ifndef CONFIG_TC001_DEMO
static bool reset_pending(void) {
    xSemaphoreTake(lock,portMAX_DELAY);
    bool pending=shared.reset;
    shared.reset=false;
    xSemaphoreGive(lock);
    return pending;
}
static esp_err_t forget(pairing_config_t *config) {
    if(config->paired) {
        esp_err_t err=ble_client_forget_peer(&config->peer);
        if(err!=ESP_OK) return err;
    }
    esp_err_t err=pairing_clear();
    if(err==ESP_OK) memset(config,0,sizeof(*config));
    return err;
}
static void link_task(void *arg) {
    (void)arg;
    pairing_config_t config={0};
    ESP_ERROR_CHECK(pairing_load(&config));
    ESP_ERROR_CHECK(ble_client_init());
    // Battery divider calibration is not yet verified on physical TC001.
    ble_client_set_display_battery_millivolts(0);
    unsigned failures=0;
    if(config.paired) message("LINKING");
    for(;;) {
        if(reset_pending()) {
            if(forget(&config)==ESP_OK) message("PAIR");
            else message("RESET FAILED");
        }
        if(!config.paired) {
            message("PAIR - OPEN SCALE WEB");
            ble_client_peer_t candidates[BLE_CLIENT_MAX_CANDIDATES]={0};
            size_t count=0;
            if(ble_client_scan(candidates,BLE_CLIENT_MAX_CANDIDATES,&count,3000)!=ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(1000));continue;
            }
            size_t active=0;ble_client_peer_t *peer=NULL;
            for(size_t i=0;i<count;i++) if(candidates[i].pairing_mode) { active++;peer=&candidates[i]; }
            if(active!=1) {
                if(active>1) { message("PAIR ONE SCALE");vTaskDelay(pdMS_TO_TICKS(2000)); }
                continue;
            }
            uint32_t code=100000U+esp_random()%900000U;
            char prompt[16];snprintf(prompt,sizeof(prompt),"%06lu",(unsigned long)code);
            message(prompt);
            ESP_LOGI(TAG,"Pairing with %s; enter the code shown on the clock in the scale webpage",peer->scale_id);
            esp_err_t err=ble_client_pair(peer,code);
            ble_client_scale_state_t state={0};
            if(err==ESP_OK) err=ble_client_fetch(peer,&state);
            if(err==ESP_OK && !ble_client_state_is_compatible(peer,&state)) err=ESP_ERR_INVALID_RESPONSE;
            if(err==ESP_OK) err=pairing_save(peer);
            if(err!=ESP_OK) {
                ble_client_forget_peer(peer);
                message("PAIR FAILED");vTaskDelay(pdMS_TO_TICKS(2500));continue;
            }
            config.paired=true;config.peer=*peer;failures=0;
        }
        ble_client_scale_state_t s={0};
        esp_err_t err=ble_client_fetch(&config.peer,&s);
        if(err==ESP_OK && !ble_client_state_is_compatible(&config.peer,&s)) err=ESP_ERR_INVALID_RESPONSE;
        if(err!=ESP_OK) {
            ESP_LOGW(TAG,"Read failed: %s",esp_err_to_name(err));
            xSemaphoreTake(lock,portMAX_DELAY);
            bool stale=now_ms()-shared.last_read>=30000;
            xSemaphoreGive(lock);
            if(stale) message("NO LINK");
            if(++failures>=3) {
                // Resolve a moved address only by the saved logical scale ID.
                ble_client_peer_t candidates[BLE_CLIENT_MAX_CANDIDATES]={0};size_t count=0;
                if(ble_client_scan(candidates,BLE_CLIENT_MAX_CANDIDATES,&count,3000)==ESP_OK)
                    for(size_t i=0;i<count;i++) if(!strcmp(candidates[i].scale_id,config.peer.scale_id)) {
                        config.peer=candidates[i];break;
                    }
            }
            vTaskDelay(pdMS_TO_TICKS(2000));continue;
        }
        failures=0;
        if(s.unpair_requested) {
            if(s.command_ack_supported && s.control_command_id &&
                ble_client_acknowledge_control(&config.peer,s.control_command_id,s.control_flags)!=ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(2000));continue;
            }
            if(forget(&config)!=ESP_OK) message("RESET FAILED");
            continue;
        }
        pixel_state_t view={.servings=s.remaining_servings,.percent=s.remaining_percent,
            .gallons=s.remaining_gallons,.ready=(s.flags & BLE_SCALE_FLAG_KEG_READY)!=0,
            .stable=(s.flags & BLE_SCALE_FLAG_STABLE)!=0};
        snprintf(view.name,sizeof(view.name),"%s",s.keg_name);
        xSemaphoreTake(lock,portMAX_DELAY);
        shared.state=view;shared.last_read=now_ms();shared.message[0]=0;
        uint32_t frame_before=shared.rendered;
        xSemaphoreGive(lock);
        // Refresh is rendered every frame; acknowledge only after one renderer cycle.
        if(s.force_refresh_requested && s.command_ack_supported && s.control_command_id) {
            vTaskDelay(pdMS_TO_TICKS(150));
            xSemaphoreTake(lock,portMAX_DELAY);
            bool rendered=shared.rendered!=frame_before;
            xSemaphoreGive(lock);
            if(rendered) ble_client_acknowledge_control(&config.peer,s.control_command_id,s.control_flags);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

#endif

void app_main(void) {
    // Do not silently erase pairing data on an NVS error.
    ESP_ERROR_CHECK(nvs_flash_init());
    lock=xSemaphoreCreateMutex();assert(lock);
    ESP_LOGI(TAG,"Keg TC001 1.0.0-tc001-dev / BLE / USB updates");
    led_strip_config_t strip_config={.strip_gpio_num=32,.max_leds=PIXEL_COUNT,
        .led_pixel_format=LED_PIXEL_FORMAT_GRB,.led_model=LED_MODEL_WS2812};
    led_strip_rmt_config_t rmt={.clk_src=RMT_CLK_SRC_DEFAULT,.resolution_hz=10000000};
    led_strip_handle_t strip;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config,&rmt,&strip));
    ESP_ERROR_CHECK(led_strip_clear(strip));
    gpio_config_t buttons={.pin_bit_mask=(1ULL<<26)|(1ULL<<14)|(1ULL<<27),
        .mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&buttons));
#ifdef CONFIG_TC001_DEMO
    message("");
    shared.state=(pixel_state_t){.servings=42,.percent=68,.gallons=3.4,.ready=true,.stable=true,.demo=true,.name="COTTAGE IPA"};
#else
    message("PAIR - OPEN SCALE WEB");
    assert(xTaskCreate(link_task,"tc001_link",8192,NULL,4,NULL)==pdPASS);
#endif
    pixel_t frame[PIXEL_COUNT];
    unsigned page=0,brightness=32;
    int64_t page_start=now_ms(),hold_until=0;
    const int pins[3]={26,14,27};
    bool pressed[3]={false},long_fired=false;
    unsigned debounce[3]={0};
    int64_t down_at[3]={0};
    while(true) {
        int64_t now=now_ms();
        for(int i=0;i<3;i++) {
            bool down=gpio_get_level(pins[i])==0;
            if(down!=pressed[i]) debounce[i]++;else debounce[i]=0;
            if(debounce[i]>=2) {
                pressed[i]=down;debounce[i]=0;
                if(down) { down_at[i]=now;if(i==2) long_fired=false; }
                else if(i==2 && !long_fired) brightness=brightness==16?32:(brightness==32?64:16);
                else if(i<2) { page=(page+(i==0?PIXEL_PAGES-1:1))%PIXEL_PAGES;page_start=now;hold_until=now+20000; }
            }
        }
        if(pressed[2] && !long_fired && now-down_at[2]>=5000) {
            #ifndef CONFIG_TC001_DEMO
            xSemaphoreTake(lock,portMAX_DELAY);shared.reset=true;xSemaphoreGive(lock);
            message("RESET");
#endif
            long_fired=true;
        }
        if(now>=hold_until && now-page_start>=8000) { page=(page+1)%PIXEL_PAGES;page_start=now; }
        xSemaphoreTake(lock,portMAX_DELAY);
        pixel_state_t state=shared.state;
        char prompt[sizeof(shared.message)];memcpy(prompt,shared.message,sizeof(prompt));
        int64_t prompt_start=shared.message_start,last_read=shared.last_read;
        xSemaphoreGive(lock);
        if(!strcmp(prompt,"LINKING") && now>=30000) pixel_message(frame,"NO LINK",(uint32_t)(now-page_start),0xff3838);
        else if(prompt[0]) pixel_message(frame,prompt,(uint32_t)(now-prompt_start),0xffb52e);
        else if(!state.demo && now-last_read>=30000) pixel_message(frame,"NO LINK",(uint32_t)(now-page_start),0xff3838);
        else pixel_render(frame,&state,page,(uint32_t)(now-page_start));
        // Manual brightness is capped at 25% for the first hardware bring-up.
        for(unsigned y=0;y<8;y++) for(unsigned x=0;x<32;x++) {
            pixel_t c=frame[y*32+x];
            ESP_ERROR_CHECK(led_strip_set_pixel(strip,pixel_wire_index(x,y,CONFIG_TC001_MATRIX_LAYOUT),
                c.r*brightness/255,c.g*brightness/255,c.b*brightness/255));
        }
        ESP_ERROR_CHECK(led_strip_refresh(strip));
        xSemaphoreTake(lock,portMAX_DELAY);shared.rendered++;xSemaphoreGive(lock);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
