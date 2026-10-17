#include "app.h"
#include "board.h"
#include "tap_control.h"
#include "pour_history.h"
#include "serving_vessel.h"
#include "pour_button.h"
#include "audio.h"
#include "idle_status.h"
#include "servings_format.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "lvgl.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <initializer_list>
namespace {
Settings saved{};
State reading{};
lv_obj_t *content, *title, *notice, *auto_btn, *auto_text, *auto_caption, *footer;
vessel_artwork::View vessel;
serving_vessel::Kind vessel_kind=serving_vessel::Kind::Pint;
lv_obj_t *value, *units, *network_list, *scale_list;
lv_obj_t *ssid, *password, *host, *keyboard;
int view = 0, page = 0;
uint32_t generation = 1;
bool pairing = false;
bool pour_state_known=false, displayed_pouring=false;
const uint32_t BG=0x101c26, CARD=0x203441, TEXT=0xf2f6f8;
void show_home();
void show_setup();
void text_if_changed(lv_obj_t *object,const char *text) {
  if(std::strcmp(lv_label_get_text(object),text)!=0)lv_label_set_text(object,text);
}
void message(const char *s) { text_if_changed(notice,s ? s : ""); }
bool submit(const char *kind, cJSON *json=nullptr) {
  Action a{}; a.ui_generation=generation;
  snprintf(a.kind,sizeof(a.kind),"%s",kind);
  char *text=json ? cJSON_PrintUnformatted(json) : nullptr;
  const bool fits=!text || strlen(text)<sizeof(a.body);
  snprintf(a.body,sizeof(a.body),"%s",text ? text : "{}");
  cJSON_free(text); cJSON_Delete(json);
  if (!fits || !actions || xQueueSend(actions,&a,0)!=pdTRUE) { message("Busy; try again"); return false; }
  return true;
}
lv_obj_t *label(lv_obj_t *parent,const char *text,const lv_font_t *font=&lv_font_montserrat_16) {
  auto l=lv_label_create(parent); lv_label_set_text(l,text);
  lv_obj_set_style_text_color(l,lv_color_hex(TEXT),0);
  lv_obj_set_style_text_font(l,font,0);
  lv_obj_set_width(l,LV_PCT(100)); lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP); return l;
}
lv_obj_t *button(lv_obj_t *parent,const char *text,lv_event_cb_t cb=nullptr) {
  auto b=lv_button_create(parent); lv_obj_set_size(b,LV_PCT(100),40);
  lv_obj_set_style_bg_color(b,lv_color_hex(0x24577e),0);
  auto l=lv_label_create(b); lv_label_set_text(l,text); lv_obj_center(l);
  if(cb)lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,nullptr);
  return b;
}
void home_value() {
  if(page || !value)return;
  char b[96];
  text_if_changed(title,reading.name[0]?reading.name:"TouchPour");
  const auto kind=serving_vessel::for_serving(reading.serving);
  if(!vessel.root || kind!=vessel_kind) {
    if(vessel.root)lv_obj_delete(vessel.root);
    vessel_kind=kind;
    vessel=vessel_artwork::create(content,serving_vessel::artwork_kind(kind),18,0);
  }
  vessel_artwork::update(vessel,reading.valid,reading.percent);
  if(!reading.valid) {
    text_if_changed(value,"--"); text_if_changed(units,"Waiting for scale");
    return;
  }
  if(view==0) { snprintf(b,sizeof(b),"%u",touchpour_complete_servings(reading.servings)); text_if_changed(value,b); text_if_changed(units,"beers remaining"); }
  else if(view==1) { snprintf(b,sizeof(b),"%.0f%%",reading.percent); text_if_changed(value,b); text_if_changed(units,"keg remaining"); }
  else { snprintf(b,sizeof(b),"%.1f",reading.gallons); text_if_changed(value,b); text_if_changed(units,"gallons remaining"); }
}
void change_view(lv_event_t *e) {
  if(page || pairing || tap::busy())return;
  auto dir=lv_indev_get_gesture_dir(lv_indev_active());
  if(dir!=LV_DIR_LEFT && dir!=LV_DIR_RIGHT)return;
  view=(view+(dir==LV_DIR_LEFT?1:2))%3; show_home();
  lv_indev_wait_release(lv_indev_active());
  (void)e;
}
void clear_page() {
  ++generation;
  if(keyboard) { lv_obj_delete(keyboard); keyboard=nullptr; }
  lv_obj_clean(content); vessel={}; value=units=network_list=scale_list=ssid=password=host=nullptr;
}
void show_home() {
  clear_page(); page=0; tap::setup_mode=false;
  // Restore the dedicated home screen controls after leaving Setup.
  lv_obj_remove_flag(auto_btn,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_size(content,224,64);
  lv_obj_set_style_bg_opa(content,LV_OPA_TRANSP,0);
  lv_obj_set_style_pad_all(content,0,0);
  lv_obj_set_pos(notice,8,272);
  lv_obj_remove_flag(footer,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(auto_btn,45,108);lv_obj_set_size(auto_btn,150,150);
  lv_obj_set_scrollbar_mode(content,LV_SCROLLBAR_MODE_OFF);
  lv_obj_remove_flag(content,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_layout(content,LV_LAYOUT_NONE);
  value=label(content,"--",&lv_font_montserrat_36);
  lv_obj_set_pos(value,78,0);lv_obj_set_size(value,142,42);
  units=label(content,"Waiting for scale",&lv_font_montserrat_14);
  lv_obj_set_pos(units,78,45);lv_obj_set_size(units,146,18);
  lv_label_set_long_mode(units,LV_LABEL_LONG_DOT);
  for(auto o:{value,units}) lv_obj_add_flag(o,LV_OBJ_FLAG_GESTURE_BUBBLE);
  home_value(); message("Place cup below the sensor");
}
void focus(lv_event_t *e) {
  if(!keyboard) {
    keyboard=lv_keyboard_create(lv_screen_active());lv_obj_set_size(keyboard,240,150);
    lv_obj_align(keyboard,LV_ALIGN_BOTTOM_MID,0,0);
    lv_obj_add_event_cb(keyboard,[](lv_event_t *){lv_obj_delete(keyboard);keyboard=nullptr;},LV_EVENT_READY,nullptr);
    lv_obj_add_event_cb(keyboard,[](lv_event_t *){lv_obj_delete(keyboard);keyboard=nullptr;},LV_EVENT_CANCEL,nullptr);
  }
  lv_keyboard_set_textarea(keyboard,static_cast<lv_obj_t *>(lv_event_get_target(e)));
}
lv_obj_t *field(const char *caption,const char *text,bool secret=false) {
  label(content,caption,&lv_font_montserrat_14);auto f=lv_textarea_create(content);
  lv_obj_set_size(f,LV_PCT(100),38);lv_textarea_set_one_line(f,true);
  lv_textarea_set_max_length(f,secret?64:127);lv_textarea_set_password_mode(f,secret);
  lv_textarea_set_text(f,text);lv_obj_add_event_cb(f,focus,LV_EVENT_FOCUSED,nullptr);return f;
}
void show_setup() {
  if(tap::busy()) { message("Stop pouring before Setup"); return; }
  clear_page(); page=1; tap::setup_mode=true;
  // Setup cannot pour: use the space normally reserved for Auto Pour
  // for scrollable Wi-Fi/scale controls, keeping the status line visible.
  lv_obj_add_flag(auto_btn,LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(footer,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_size(content,224,242);
  lv_obj_set_style_bg_opa(content,LV_OPA_COVER,0);
  lv_obj_set_style_pad_all(content,8,0);
  lv_obj_set_pos(notice,8,289);
  lv_label_set_text(title,"TouchPour Setup");
  lv_obj_add_flag(content,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(content,LV_DIR_VER);
  lv_obj_set_scrollbar_mode(content,LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_flex_flow(content,LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(content,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
  button(content,"Back to display",[](lv_event_t *){show_home();});
  ssid=field("Wi-Fi network",saved.ssid);lv_textarea_set_max_length(ssid,32);
  button(content,"Scan Wi-Fi",[](lv_event_t *){submit("scan_wifi");});
  network_list=lv_dropdown_create(content);lv_obj_set_width(network_list,LV_PCT(100));lv_dropdown_set_options(network_list,"Scan to list networks");
  lv_obj_add_event_cb(network_list,[](lv_event_t *){char b[100];lv_dropdown_get_selected_str(network_list,b,sizeof(b));lv_textarea_set_text(ssid,b);},LV_EVENT_VALUE_CHANGED,nullptr);
  password=field("Wi-Fi password",saved.password,true);
  button(content,"Show / hide password",[](lv_event_t *){lv_textarea_set_password_mode(password,!lv_textarea_get_password_mode(password));});
  host=field("Scale IP or hostname",saved.host);
  button(content,"Find scale",[](lv_event_t *){submit("discover");});
  scale_list=lv_dropdown_create(content);lv_obj_set_width(scale_list,LV_PCT(100));lv_dropdown_set_options(scale_list,"Find scale or enter IP above");
  lv_obj_add_event_cb(scale_list,[](lv_event_t *){char b[128];lv_dropdown_get_selected_str(scale_list,b,sizeof(b));if(strncmp(b,"Manual",6))lv_textarea_set_text(host,b);},LV_EVENT_VALUE_CHANGED,nullptr);
  button(content,"Save and connect",[](lv_event_t *){
    auto j=cJSON_CreateObject();cJSON_AddStringToObject(j,"ssid",lv_textarea_get_text(ssid));
    cJSON_AddStringToObject(j,"password",lv_textarea_get_text(password));cJSON_AddStringToObject(j,"host",lv_textarea_get_text(host));
    cJSON_AddNumberToObject(j,"brightness",saved.brightness);cJSON_AddNumberToObject(j,"slot",0);submit("settings",j);
  });
  button(content,"Remove scale pairing",[](lv_event_t *){submit("forget");});
  button(content,tap::audio_enabled()?"Sounds: On (tap to mute)":"Sounds: Muted (tap to enable)",[](lv_event_t *event){
    bool enabled=!tap::audio_enabled();
    if(!tap::audio_set_enabled(enabled)){message("Could not save sound setting");return;}
    auto b=static_cast<lv_obj_t *>(lv_event_get_target(event));
    lv_label_set_text(lv_obj_get_child(b,0),enabled?"Sounds: On (tap to mute)":"Sounds: Muted (tap to enable)");
  });
  button(content,"Test speaker",[](lv_event_t *){
    bool played=tap::audio_test();
    message(played?"Speaker test playing":tap::audio_enabled()?"Audio unavailable - see serial":"Enable sounds to test speaker");
  });
  button(content,"Test pour tune",[](lv_event_t *){
    bool played=tap::audio_preview();
    message(played?"Playing 99 Bottles preview":tap::audio_enabled()?"Audio unavailable - see serial":"Enable sounds to test tune");
  });
  label(content,"Use Add touchscreen on the scale to pair. Pour calibration, servo tests and last five pours are on this device's website.",&lv_font_montserrat_14);
  auto netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");esp_netif_ip_info_t ip{};char b[96];
  if(netif && esp_netif_get_ip_info(netif,&ip)==ESP_OK) {
    snprintf(b,sizeof(b),"http://" IPSTR,IP2STR(&ip.ip));label(content,b);
    if(ip.ip.addr) {auto qr=lv_qrcode_create(content);lv_qrcode_set_size(qr,160);lv_qrcode_update(qr,b,strlen(b));}
  }
  snprintf(b,sizeof(b),"TouchPour %s",esp_app_get_description()->version);label(content,b);
  message("Pouring disabled while Setup is open");
}
void auto_action(lv_event_t *) {
  if(page || pairing)return;
  if(tap::busy()) {tap::stop_requested=true;return;}
  tap::Action a{tap::Action::START,0};
  if(xQueueSend(tap::actions,&a,0)!=pdTRUE)message("Busy; try again");
}
void poll(lv_timer_t *) {
  if(xSemaphoreTake(tap::state_mutex,0)!=pdTRUE)return;
  bool active=tap::pouring_active() || tap::manual_test_active;
  bool ready=tap::control.ready(tap::settings,esp_timer_get_time()/1000);
  bool blocked=page || pairing || tap::setup_mode || tap::firmware_updating || tap::manual_test_active || tap::history_saving;
  const tap::Settings config=tap::settings;
  const tap::Sample measurement=tap::sample;
  const uint32_t age=uint32_t(esp_timer_get_time()/1000)-measurement.at;
  const bool fresh=age<=uint32_t(tap::settings.stale_ms);
  const char *idle=tap::idle_status(config,measurement,uint32_t(esp_timer_get_time()/1000),ready,tap::control.reason);
  xSemaphoreGive(tap::state_mutex);
  if(active || (!blocked && ready))lv_obj_remove_state(auto_btn,LV_STATE_DISABLED);else lv_obj_add_state(auto_btn,LV_STATE_DISABLED);
  // Fonts, alignment and visibility invalidate the button even when unchanged.
  // Keep the 50 ms safety/status poll, but repaint the face only on transitions.
  if(!pour_state_known || active!=displayed_pouring) {
    lv_label_set_text(auto_text,active?"EMERGENCY\nSTOP":"POUR");
    lv_obj_set_style_text_font(auto_text,active?&lv_font_montserrat_20:&lv_font_montserrat_28,0);
    lv_obj_align(auto_text,LV_ALIGN_CENTER,0,active?0:-10);
    if(active)lv_obj_add_flag(auto_caption,LV_OBJ_FLAG_HIDDEN);else lv_obj_remove_flag(auto_caption,LV_OBJ_FLAG_HIDDEN);
    pour_button::update(auto_btn,active);
    displayed_pouring=active;pour_state_known=true;
  }
  char distance[48];
  if(measurement.valid && measurement.filtered_valid && fresh)snprintf(distance,sizeof(distance),"LiDAR: %d mm",measurement.stable_distance());
  else snprintf(distance,sizeof(distance),"LiDAR: %s",measurement.valid&&!fresh?"stale":"no reading");
  text_if_changed(footer,distance);
  if(!page && !pairing) {
    if(active)message("Touch anywhere to stop");
    else message(ready&&reading.valid&&!reading.online?"Cup ready - Scale stale":idle);
  }
}
}
void ui_start(const Settings &s) {
  saved=s; touchpour_board_start();touchpour_brightness(s.brightness>=10?s.brightness:85);
  configASSERT(lvgl_port_lock(0));auto root=lv_screen_active();
  lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(root,lv_color_hex(BG),0);
  lv_obj_set_style_text_font(root,&lv_font_montserrat_14,0);
  title=label(root,"TouchPour");lv_obj_set_pos(title,8,7);lv_obj_set_width(title,177);lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
  auto menu=button(root,LV_SYMBOL_LIST,[](lv_event_t *){page?show_home():show_setup();});lv_obj_set_pos(menu,192,3);lv_obj_set_size(menu,40,30);
  content=lv_obj_create(root);lv_obj_set_pos(content,8,38);lv_obj_set_size(content,224,200);
  lv_obj_set_style_bg_color(content,lv_color_hex(CARD),0);lv_obj_set_style_border_width(content,0,0);lv_obj_set_style_pad_all(content,8,0);lv_obj_set_style_pad_row(content,8,0);
  lv_obj_add_event_cb(content,change_view,LV_EVENT_GESTURE,nullptr);
  notice=label(root,"");lv_obj_set_pos(notice,8,243);lv_obj_set_size(notice,224,18);lv_label_set_long_mode(notice,LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(notice,LV_TEXT_ALIGN_CENTER,0);
  auto_btn=button(root,"POUR",auto_action);lv_obj_set_pos(auto_btn,45,108);lv_obj_set_size(auto_btn,150,150);auto_text=lv_obj_get_child(auto_btn,0);
  pour_button::style(auto_btn);
  lv_obj_set_style_text_font(auto_text,&lv_font_montserrat_28,0);
  lv_obj_set_style_text_color(auto_text,lv_color_hex(TEXT),0);
  lv_obj_set_style_text_align(auto_text,LV_TEXT_ALIGN_CENTER,0);
  auto_caption=label(auto_btn,"Auto fill",&lv_font_montserrat_14);
  lv_obj_set_style_text_align(auto_caption,LV_TEXT_ALIGN_CENTER,0);lv_obj_align(auto_caption,LV_ALIGN_CENTER,0,24);
  footer=label(root,"LiDAR: waiting",&lv_font_montserrat_14);
  lv_obj_set_pos(footer,8,296);lv_obj_set_size(footer,224,18);lv_obj_set_style_text_align(footer,LV_TEXT_ALIGN_CENTER,0);
  if(saved.ssid[0])show_home();else show_setup();lv_timer_create(poll,50,nullptr);lvgl_port_unlock();
}
void ui_state(const State &s) {if(lvgl_port_lock(1000)){reading=s;home_value();lvgl_port_unlock();}}
void ui_message(const char *s) {if(!notice)return;if(lvgl_port_lock(1000)){message(s);lvgl_port_unlock();}}
void ui_message_for_generation(const char *s,uint32_t g){if(lvgl_port_lock(1000)){if(g==generation)message(s);lvgl_port_unlock();}}
void ui_result(bool ok,const char *op,const char *err){char b[128];snprintf(b,sizeof(b),"%s: %s",op,ok?"Done":err);ui_message(b);}
void ui_pair_code(const char *code){if(lvgl_port_lock(1000)){pairing=true;tap::setup_mode=true;char b[80];snprintf(b,sizeof(b),"Pairing code: %s",code);message(b);lvgl_port_unlock();}}
void ui_paired(){if(lvgl_port_lock(1000)){pairing=false;tap::setup_mode=page!=0;message("Scale paired");lvgl_port_unlock();}}
bool touchscreen_pairing_overlay_visible(){return pairing;}
void touchscreen_pairing_window(uint32_t seconds){char b[80];snprintf(b,sizeof(b),"Pairing: %lu seconds remaining",(unsigned long)seconds);ui_message(b);}
void touchscreen_pairing_ended(){if(lvgl_port_lock(1000)){pairing=false;tap::setup_mode=page!=0;message("Pairing ended");lvgl_port_unlock();}}
void ui_settings_applied(const Settings &s){if(lvgl_port_lock(1000)){saved=s;touchpour_brightness(s.brightness);lvgl_port_unlock();}}
void ui_scale_profiles(const char *,bool,const char *,bool,uint8_t){}
void ui_discovered(const char *h){if(lvgl_port_lock(1000)){if(page && host)lv_textarea_set_text(host,h);lvgl_port_unlock();}}
void ui_discovered_options_for_generation(const char *s,uint32_t g){if(lvgl_port_lock(1000)){if(page&&scale_list&&g==generation)lv_dropdown_set_options(scale_list,s);lvgl_port_unlock();}}
void ui_discovered_options(const char *s){ui_discovered_options_for_generation(s,generation);}
void ui_networks(const char *s,uint32_t g){if(lvgl_port_lock(1000)){if(page&&network_list&&g==generation)lv_dropdown_set_options(network_list,s);lvgl_port_unlock();}}
void touchscreen_home_discovery_result(int,const char *,const char *,const char *s){ui_message(s);}
void touchscreen_home_set_update_available(bool){}
// Dedicated TouchPour images only. The existing touchscreen OTA feed targets
// incompatible LCD hardware. Initial builds use USB or the device's web upload.
esp_err_t touchscreen_ota(bool){ui_message("Use TouchPour .bin on device website");return ESP_ERR_NOT_SUPPORTED;}
void ui_update_checking(){}
void ui_update_status(const char *,const char *,bool,bool){}
void ui_update_installing(const char *){}
void ui_update_progress(int){}
void ui_update_complete(const char *){}
void ui_update_error(const char *s,bool){ui_message(s);}
