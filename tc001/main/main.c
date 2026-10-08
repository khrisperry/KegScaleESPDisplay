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
#include "wifi_link.h"
#include "wifi_policy.h"
#include "pixel_ui.h"
#include "sdkconfig.h"

static const char *TAG="tc001";
static int64_t now_ms(void) { return esp_timer_get_time()/1000; }

void app_main(void) {
    // Do not silently erase pairing data on an NVS error.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI(TAG,"Keg TC001 1.0.1-tc001-dev / Wi-Fi / USB updates");
    led_strip_config_t strip_config={.strip_gpio_num=32,.max_leds=PIXEL_COUNT,
        .color_component_format=LED_STRIP_COLOR_COMPONENT_FMT_GRB,.led_model=LED_MODEL_WS2812};
    led_strip_rmt_config_t rmt={.clk_src=RMT_CLK_SRC_DEFAULT,.resolution_hz=10000000};
    led_strip_handle_t strip;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config,&rmt,&strip));
    ESP_ERROR_CHECK(led_strip_clear(strip));
    gpio_config_t buttons={.pin_bit_mask=(1ULL<<26)|(1ULL<<14)|(1ULL<<27),
        .mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&buttons));
#ifndef CONFIG_TC001_DEMO
    ESP_ERROR_CHECK(tc001_wifi_start());
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
            tc001_wifi_setup_request();
#endif
            long_fired=true;
        }
        if(now>=hold_until && now-page_start>=8000) { page=(page+1)%PIXEL_PAGES;page_start=now; }
#ifdef CONFIG_TC001_DEMO
        pixel_state_t state={.servings=42,.percent=68,.gallons=3.4,.ready=true,.stable=true,.demo=true,.name="COTTAGE IPA"};
        pixel_render(frame,&state,page,(uint32_t)(now-page_start));
#else
        tc001_view_t view;tc001_wifi_view(&view);
        if(view.setup_active) {
            char prompt[96];
            uint32_t elapsed=(uint32_t)(now-view.setup_start_ms);
            switch((elapsed/8000)%4) {
            case 0: snprintf(prompt,sizeof(prompt),"JOIN %s",view.setup_ssid);break;
            case 1: snprintf(prompt,sizeof(prompt),"%s",view.setup_password);break;
            case 2: snprintf(prompt,sizeof(prompt),"192.168.4.1");break;
            default: snprintf(prompt,sizeof(prompt),"WIFI SETUP");break;
            }
            pixel_message(frame,prompt,elapsed%8000,0x48baff);
        } else if(view.message[0]) pixel_message(frame,view.message,(uint32_t)(now-view.message_start_ms),0xffb52e);
        else if(!view.online || tc001_state_stale(now,view.last_state_ms)) pixel_message(frame,"NO LINK",0,0xff3838);
        else pixel_render(frame,&view.state,page,(uint32_t)(now-page_start));
#endif
        // Manual brightness is capped at 25% for the first hardware bring-up.
        for(unsigned y=0;y<8;y++) for(unsigned x=0;x<32;x++) {
            pixel_t c=frame[y*32+x];
            ESP_ERROR_CHECK(led_strip_set_pixel(strip,pixel_wire_index(x,y,CONFIG_TC001_MATRIX_LAYOUT),
                c.r*brightness/255,c.g*brightness/255,c.b*brightness/255));
        }
        ESP_ERROR_CHECK(led_strip_refresh(strip));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
