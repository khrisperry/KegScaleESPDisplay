#include "audio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"
#include <atomic>
#include <cmath>

namespace tap {
namespace {
constexpr int rate=16000, frames=128;
constexpr const char *tag="tap_audio";
i2s_chan_handle_t output;
QueueHandle_t cues;
std::atomic<bool> enabled{true};
struct Note { int hz, ms; };
constexpr Note cup[]={{1100,90},{0,40}};
constexpr Note start[]={{660,90},{0,30},{990,120},{0,40}};
constexpr Note complete[]={{784,100},{988,100},{1175,180},{0,40}};
constexpr Note stop[]={{880,100},{0,25},{440,160},{0,40}};
void audio_task(void *) {
  const Note *notes=nullptr;
  int count=0,index=0,offset=0;
  float phase=0;
  int16_t pcm[frames*2]{};
  for (;;) {
    SoundCue cue;
    if (xQueueReceive(cues,&cue,0)==pdTRUE) {
      index=offset=0; phase=0;
      switch(cue) {
      case SoundCue::Cup: notes=cup;count=2;break;
      case SoundCue::Start: notes=start;count=4;break;
      case SoundCue::Complete: notes=complete;count=4;break;
      case SoundCue::Stop: notes=stop;count=4;break;
      default: notes=nullptr;count=0;break;
      }
    }
    for(int f=0;f<frames;++f) {
      int16_t sample=0;
      if(notes && index<count && enabled.load()) {
        int length=notes[index].ms*rate/1000;
        int hz=notes[index].hz;
        // 5 ms attack/release avoids clicks; moderate PCM amplitude, with
        // final loudness set by Waveshare's physical volume knob.
        float envelope=std::fmin(1.0f,std::fmin(offset/80.0f,(length-1-offset)/80.0f));
        if(hz) {
          sample=int16_t(std::sin(phase)*5000*envelope);
          phase+=6.28318530718f*hz/rate;
          if(phase>=6.28318530718f)phase-=6.28318530718f;
        }
        if(++offset>=length){offset=0;phase=0;++index;}
      }
      pcm[2*f]=pcm[2*f+1]=sample;
    }
    size_t written=0;
    // Only this low-priority worker waits on DMA. Both stereo channels carry
    // the cue; no SD card, network access, codec control bus, or PWM needed.
    esp_err_t error=i2s_channel_write(output,pcm,sizeof(pcm),&written,50);
    if(error!=ESP_OK || written!=sizeof(pcm)) {
      ESP_LOGW(tag,"Audio write failed: %s; pour control continues",esp_err_to_name(error));
      i2s_channel_disable(output);
      vTaskDelete(nullptr);
      return;
    }
  }
}
}
bool audio_enabled(){return enabled.load();}
bool audio_set_enabled(bool value) {
  nvs_handle_t handle;
  if(nvs_open("tap_audio",NVS_READWRITE,&handle)!=ESP_OK)return false;
  esp_err_t error=nvs_set_u8(handle,"enabled",value);
  if(error==ESP_OK)error=nvs_commit(handle);
  nvs_close(handle);
  if(error!=ESP_OK)return false;
  enabled=value;
  audio_notify(SoundCue::None);
  return true;
}
void audio_notify(SoundCue cue) {
  // Latest event replaces pending audio: STOP cannot sit behind an old
  // detection/start chime. Active sound is replaced within one chunk.
  if(cues && (enabled.load() || cue==SoundCue::None)) xQueueOverwrite(cues,&cue);
}
void audio_start() {
  nvs_handle_t handle;
  if(nvs_open("tap_audio",NVS_READONLY,&handle)==ESP_OK) {
    uint8_t value=1;nvs_get_u8(handle,"enabled",&value);enabled=value!=0;nvs_close(handle);
  }
  i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
  channel.dma_desc_num=4;channel.dma_frame_num=frames;channel.auto_clear=true;
  esp_err_t error=i2s_new_channel(&channel,&output,nullptr);
  if(error==ESP_OK) {
    i2s_std_config_t config{};
    config.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(rate);
    config.slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk=I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk=GPIO_NUM_48;config.gpio_cfg.ws=GPIO_NUM_38;
    config.gpio_cfg.dout=GPIO_NUM_47;config.gpio_cfg.din=I2S_GPIO_UNUSED;
    error=i2s_channel_init_std_mode(output,&config);
  }
  if(error==ESP_OK)error=i2s_channel_enable(output);
  if(error==ESP_OK) {
    cues=xQueueCreate(1,sizeof(SoundCue));
    if(!cues || xTaskCreate(audio_task,"tap_audio",4096,nullptr,2,nullptr)!=pdPASS)error=ESP_ERR_NO_MEM;
  }
  if(error!=ESP_OK) {
    ESP_LOGW(tag,"Audio unavailable: %s; pour control continues",esp_err_to_name(error));
    if(cues){vQueueDelete(cues);cues=nullptr;}
    if(output){i2s_channel_disable(output);i2s_del_channel(output);output=nullptr;}
  } else ESP_LOGI(tag,"PCM5101 speaker: BCLK48/LRCK38/DIN47, sounds %s",enabled?"enabled":"muted");
}
} // namespace tap
