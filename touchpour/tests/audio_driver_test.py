"""Exercise real audio initialization/playback with simulated I2S and NVS."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "components/tap_control/audio.cpp").read_text()
source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
harness = r'''
#include "audio.h"
#include "pour_melody.h"
#include <atomic>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <algorithm>
using esp_err_t=int;
using i2s_chan_handle_t=void*;
using QueueHandle_t=void*;
using nvs_handle_t=int;
constexpr int ESP_OK=0,ESP_ERR_NO_MEM=1;
constexpr int I2S_NUM_0=0,I2S_ROLE_MASTER=1,I2S_GPIO_UNUSED=-1;
constexpr int GPIO_NUM_48=48,GPIO_NUM_38=38,GPIO_NUM_47=47;
constexpr int I2S_DATA_BIT_WIDTH_16BIT=16,I2S_SLOT_MODE_STEREO=2;
constexpr int pdTRUE=1,pdPASS=1,NVS_READWRITE=1,NVS_READONLY=0;
struct i2s_chan_config_t{int dma_desc_num=0,dma_frame_num=0;bool auto_clear=false;};
struct i2s_std_config_t{
 struct Clock{int sample_rate_hz=0;}clk_cfg;
 struct Slot{int data_bit_width=0,slot_mode=0;}slot_cfg;
 struct Gpio{int mclk=0,bclk=0,ws=0,dout=0,din=0;}gpio_cfg;
};
#define I2S_CHANNEL_DEFAULT_CONFIG(a,b) i2s_chan_config_t{}
#define I2S_STD_CLK_DEFAULT_CONFIG(r) i2s_std_config_t::Clock{r}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(b,m) i2s_std_config_t::Slot{b,m}
template<class... T>void log(const char*,const char*,T...){}
#define ESP_LOGI(...) log(__VA_ARGS__)
#define ESP_LOGW(...) log(__VA_ARGS__)
const char* esp_err_to_name(int){return "test";}
i2s_std_config_t configured;
void(*worker)(void*)=nullptr;
bool pending=false;
tap::SoundCue queued=tap::SoundCue::None;
int writes=0,peak=0,limit=200,nonzero_writes=0,last_nonzero=0,interrupt_at=0;
tap::SoundCue interrupt_cue=tap::SoundCue::None;
bool saved_enabled=true;
struct Done{};
int xQueueReceive(void*,void* value,int){if(!pending)return 0;memcpy(value,&queued,sizeof(queued));pending=false;return pdTRUE;}
void xQueueOverwrite(void*,const void* value){memcpy(&queued,value,sizeof(queued));pending=true;}
void* xQueueCreate(int count,size_t size){assert(count==1&&size==sizeof(tap::SoundCue));return &pending;}
void vQueueDelete(void*){}
int xTaskCreate(void(*fn)(void*),const char*,int,void*,int priority,void*){assert(priority>5&&priority<8);worker=fn;return pdPASS;}
void vTaskDelete(void*){}
int nvs_open(const char*,int,int* h){*h=1;return ESP_OK;}
int nvs_get_u8(int,const char*,uint8_t* value){*value=saved_enabled;return ESP_OK;}
int nvs_set_u8(int,const char*,uint8_t value){saved_enabled=value;return ESP_OK;}
int nvs_commit(int){return ESP_OK;}
void nvs_close(int){}
int i2s_new_channel(const i2s_chan_config_t* config,void** output,void*){assert(config->dma_desc_num==8&&config->dma_frame_num==128);*output=&pending;return ESP_OK;}
int i2s_channel_init_std_mode(void*,const i2s_std_config_t* config){configured=*config;return ESP_OK;}
int i2s_channel_enable(void*){return ESP_OK;}
int i2s_channel_disable(void*){return ESP_OK;}
int i2s_del_channel(void*){return ESP_OK;}
int i2s_channel_write(void*,const void* data,size_t size,size_t* written,int){
 const auto pcm=static_cast<const int16_t*>(data);
 int chunk_peak=0;
 for(size_t i=0;i<size/sizeof(int16_t);i+=2){assert(pcm[i]==pcm[i+1]);chunk_peak=std::max(chunk_peak,std::abs(int(pcm[i])));}
 peak=std::max(peak,chunk_peak);
 if(chunk_peak){++nonzero_writes;last_nonzero=writes;}
 *written=size;
 ++writes;
 if(writes==interrupt_at){queued=interrupt_cue;pending=true;}
 if(writes>=limit)throw Done{};
 return ESP_OK;
}
'''+source+r'''
void run(int chunks=200){writes=peak=nonzero_writes=last_nonzero=0;limit=chunks;try{worker(nullptr);}catch(Done&){} }
int main(){
 tap::audio_start();assert(worker);
 // This no-MCLK clock pair is permitted by the PCM5101 PLL table.
 assert(configured.clk_cfg.sample_rate_hz==44100);
 assert(configured.slot_cfg.data_bit_width==16&&configured.slot_cfg.slot_mode==2);
 assert(configured.clk_cfg.sample_rate_hz*configured.slot_cfg.data_bit_width*2==1411200);
 assert(configured.gpio_cfg.mclk==-1&&configured.gpio_cfg.bclk==48&&configured.gpio_cfg.ws==38&&configured.gpio_cfg.dout==47);
 tap::audio_notify(tap::SoundCue::Complete);run();assert(peak>4000&&peak<=5000);
 assert(tap::audio_test());run();assert(peak>4000);
 assert(tap::audio_set_enabled(false)&&!saved_enabled&&!tap::audio_test());run();assert(peak==0);
 tap::audio_notify(tap::SoundCue::Start);run();assert(peak==0);
 assert(tap::audio_set_enabled(true)&&saved_enabled);assert(tap::audio_test());run();assert(peak>4000);
 // 18.5 seconds exceeds a verse: pour music must wrap and keep playing.
 tap::audio_notify(tap::SoundCue::Start);run(6400);assert(last_nonzero>6300);
 // Preview plays one complete verse and does not loop indefinitely.
 assert(tap::audio_preview());run(6400);assert(nonzero_writes>4000&&last_nonzero<5100);
 // Stop/completion interrupt the loop and then stay silent.
 for(auto cue:{tap::SoundCue::Stop,tap::SoundCue::Complete,tap::SoundCue::None}){
  interrupt_at=200;interrupt_cue=cue;
  tap::audio_notify(tap::SoundCue::Start);run(1000);
  assert(last_nonzero<370);interrupt_at=0;
 }
 puts("PASS: real speaker clock/pins, stereo PCM generation, test cue, mute/save/unmute, audio worker above display and below control, buffered DMA");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory)
    (path / "test.cpp").write_text(harness)
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root / "components/tap_control"),
                    str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
