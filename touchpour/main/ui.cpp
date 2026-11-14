#include "app.h"
#include "board.h"
#include "tap_control.h"
#include "pour_history.h"
#include "serving_vessel.h"
#include "pour_button.h"
#include "audio.h"
#include "pin_lock.h"
#include "pin_pad.h"
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
lv_obj_t *content, *title, *notice, *auto_btn, *auto_text, *footer;
// V0.1.23 refresh: header link line, divider, keg level bar, status pill, Small Cup.
lv_obj_t *menu_btn, *menu_icon, *link_dot, *link_text, *divider, *pill, *pill_dot, *small_btn;
lv_obj_t *level_bar, *level_text;
pour_button::Split split_button;
vessel_artwork::View vessel;
serving_vessel::Kind vessel_kind=serving_vessel::Kind::Pint;
lv_obj_t *value, *units, *network_list, *scale_list;
lv_obj_t *ssid, *password, *host, *keyboard;
int view = 0, page = 0;
uint32_t generation = 1;
bool pairing = false;
bool pour_state_known=false, displayed_pouring=false;
const uint32_t BG=0x0d1821, SURFACE=0x16252f, RAISED=0x1e3240, LINE=0x2a4252,
               TEXT=0xf2f6f8, MUTED=0x93a8b6, ACCENT=0x7fd0ff, DANGER=0xff8a90;
// Pour button x positions: left of SMALL CUP when idle, centred while pouring.
constexpr int POUR_X_IDLE=8, POUR_X_CENTRED=45, POUR_Y=120;
void show_home();
void show_setup();
void message(const char *s);
enum class PinMode { Unlock, VerifyChange, New, Confirm, Remove };
PinMode pin_mode=PinMode::Unlock;
pin_pad::View pin_view;
char current_pin[5]{},new_pin[5]{};
void show_pin(PinMode mode);
void begin_pin(PinMode mode);
void clear_pin_secrets(){std::memset(current_pin,0,sizeof(current_pin));std::memset(new_pin,0,sizeof(new_pin));}
void pin_cancel(){pin_pad::destroy(pin_view);clear_pin_secrets();show_setup();}
void pin_submit(const char *pin){
  using tap::pin_lock::Result;
  Result result=Result::Ok;
  if(pin_mode==PinMode::Unlock){
    result=tap::pin_lock::unlock(pin);
    if(result==Result::Ok){pin_pad::destroy(pin_view);clear_pin_secrets();show_home();return;}
  }else if(pin_mode==PinMode::VerifyChange){
    result=tap::pin_lock::check(pin);
    if(result==Result::Ok){std::memcpy(current_pin,pin,5);show_pin(PinMode::New);return;}
  }else if(pin_mode==PinMode::New){
    std::memcpy(new_pin,pin,5);show_pin(PinMode::Confirm);return;
  }else if(pin_mode==PinMode::Confirm){
    if(std::strcmp(new_pin,pin)!=0){
      std::memset(new_pin,0,sizeof(new_pin));show_pin(PinMode::New);
      lv_label_set_text(pin_view.message,"PINs differ - try again");return;
    }
    result=tap::pin_lock::change(current_pin,new_pin);
    if(result==Result::Ok){clear_pin_secrets();tap::setup_mode=false;show_pin(PinMode::Unlock);return;}
  }else if(pin_mode==PinMode::Remove){
    result=tap::pin_lock::remove(pin);
    if(result==Result::Ok){pin_pad::destroy(pin_view);clear_pin_secrets();show_setup();return;}
  }
  lv_label_set_text(pin_view.message,result==Result::Wait?"Too many tries - wait":
      result==Result::StorageError?"Could not read/save PIN":"Incorrect PIN");
}
void show_pin(PinMode mode){
  pin_mode=mode;
  const char *heading=mode==PinMode::Unlock?"Enter PIN to unlock":
      mode==PinMode::New?"Choose a 4-digit PIN":mode==PinMode::Confirm?"Confirm new PIN":
      mode==PinMode::Remove?"PIN to remove lock":"Enter current PIN";
  pin_pad::create(pin_view,lv_screen_active(),heading,pin_submit,
                  mode==PinMode::Unlock?nullptr:pin_cancel);
}
void begin_pin(PinMode mode){
  if(tap::busy()){message("Stop pouring before PIN setup");return;}
  clear_pin_secrets();show_pin(mode);
}
void text_if_changed(lv_obj_t *object,const char *text) {
  if(std::strcmp(lv_label_get_text(object),text)!=0)lv_label_set_text(object,text);
}
void message(const char *s) { text_if_changed(notice,s ? s : ""); }
// Status pill colour: grey neutral, green ready, amber waiting, red pouring.
enum class Tone { Neutral, Ready, Waiting, Pouring };
Tone pill_tone=Tone::Neutral;
bool pill_tone_known=false;
void set_tone(Tone tone) {
  if(pill_tone_known && tone==pill_tone)return;
  pill_tone=tone;pill_tone_known=true;
  static const uint32_t dot[]={0x93a8b6,0x3ad38a,0xf5b740,0xff525b};
  static const uint32_t fill[]={0x16252f,0x16252f,0x231f14,0x2a1418};
  static const uint32_t edge[]={0x2a4252,0x2a4252,0x5a4a22,0x6a2a31};
  const int i=static_cast<int>(tone);
  lv_obj_set_style_bg_color(pill_dot,lv_color_hex(dot[i]),0);
  lv_obj_set_style_bg_color(pill,lv_color_hex(fill[i]),0);
  lv_obj_set_style_border_color(pill,lv_color_hex(edge[i]),0);
}
const char *pouring_text(tap::PourKind kind) {
  return kind==tap::PourKind::Half?"1/2 pour - tap to stop":
         kind==tap::PourKind::Small?"Small cup - tap to stop":"Full pour - tap to stop";
}
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
lv_obj_t *label(lv_obj_t *parent,const char *text,const lv_font_t *font=&lv_font_montserrat_16,uint32_t color=TEXT) {
  auto l=lv_label_create(parent); lv_label_set_text(l,text);
  lv_obj_set_style_text_color(l,lv_color_hex(color),0);
  lv_obj_set_style_text_font(l,font,0);
  lv_obj_set_width(l,LV_PCT(100)); lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP); return l;
}
// Small raised square button: header menu/back, Scan, Find, show password.
lv_obj_t *raised(lv_obj_t *parent,const char *text,lv_event_cb_t cb,int width,int height) {
  auto b=lv_button_create(parent);lv_obj_remove_style_all(b);
  lv_obj_set_size(b,width,height);lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(b,8,0);
  lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
  lv_obj_set_style_bg_color(b,lv_color_hex(RAISED),0);
  lv_obj_set_style_bg_color(b,lv_color_hex(0x347fbb),LV_STATE_PRESSED);
  lv_obj_set_style_border_width(b,1,0);
  lv_obj_set_style_border_color(b,lv_color_hex(LINE),0);
  auto l=lv_label_create(b);lv_label_set_text(l,text);
  lv_obj_set_style_text_color(l,lv_color_hex(TEXT),0);
  lv_obj_set_style_text_font(l,&lv_font_montserrat_14,0);lv_obj_center(l);
  if(cb)lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,nullptr);
  return b;
}
void set_link() {
  // Header line under the beer name: scale connection at a glance.
  const bool online=reading.valid && reading.online;
  const char *text=!reading.valid?"Waiting for scale":online?"Scale connected":"Scale offline";
  if(std::strcmp(lv_label_get_text(link_text),text)==0)return;
  lv_label_set_text(link_text,text);
  lv_obj_set_style_bg_color(link_dot,lv_color_hex(!reading.valid?MUTED:online?0x3ad38a:0xf5b740),0);
}
void home_value() {
  if(page || !value)return;
  char b[96];
  text_if_changed(title,reading.name[0]?reading.name:"TouchPour");
  set_link();
  const auto kind=serving_vessel::for_serving(reading.serving);
  if(!vessel.root || kind!=vessel_kind) {
    if(vessel.root)lv_obj_delete(vessel.root);
    vessel_kind=kind;
    vessel=vessel_artwork::create(content,serving_vessel::artwork_kind(kind),10,2);
  }
  vessel_artwork::update(vessel,reading.valid,reading.percent);
  float percent=reading.valid?reading.percent:0.0f;
  if(!(percent>0.0f))percent=0.0f;
  if(percent>100.0f)percent=100.0f;
  const int level=int(std::lround(percent));
  if(lv_bar_get_value(level_bar)!=level)lv_bar_set_value(level_bar,level,LV_ANIM_OFF);
  if(!reading.valid) {
    text_if_changed(value,"--"); text_if_changed(units,"Waiting for scale"); text_if_changed(level_text,"--");
    return;
  }
  snprintf(b,sizeof(b),"%d%%",level); text_if_changed(level_text,b);
  if(view==0) { snprintf(b,sizeof(b),"%u",touchpour_complete_servings(reading.servings)); text_if_changed(value,b); text_if_changed(units,"beers remaining"); }
  else if(view==1) { snprintf(b,sizeof(b),"%.0f%%",reading.percent); text_if_changed(value,b); text_if_changed(units,"keg remaining"); }
  else { snprintf(b,sizeof(b),"%.1f",reading.gallons); text_if_changed(value,b); text_if_changed(units,"gallons remaining"); }
}
void change_view(lv_event_t *e) {
  if(page || pairing || tap::pin_lock::locked() || pin_view.root || tap::busy())return;
  auto dir=lv_indev_get_gesture_dir(lv_indev_active());
  if(dir!=LV_DIR_LEFT && dir!=LV_DIR_RIGHT)return;
  view=(view+(dir==LV_DIR_LEFT?1:2))%3; show_home();
  lv_indev_wait_release(lv_indev_active());
  (void)e;
}
void clear_page() {
  ++generation;
  if(keyboard) { lv_obj_delete(keyboard); keyboard=nullptr; }
  lv_obj_clean(content); vessel={};
  value=units=level_bar=level_text=network_list=scale_list=ssid=password=host=nullptr;
}
void show_hidden(lv_obj_t *object,bool visible) {
  if(visible)lv_obj_remove_flag(object,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(object,LV_OBJ_FLAG_HIDDEN);
}
// Home: beer name + link line, menu at the right. Setup: back arrow + "Setup".
void chrome(bool home) {
  lv_obj_set_pos(menu_btn,home?196:8,8);
  lv_label_set_text(menu_icon,home?LV_SYMBOL_LIST:LV_SYMBOL_LEFT);
  lv_obj_set_pos(title,home?12:52,home?6:12);
  lv_obj_set_style_text_font(title,home?&lv_font_montserrat_16:&lv_font_montserrat_18,0);
  for(auto object:{link_dot,link_text,auto_btn,footer})show_hidden(object,home);
  show_hidden(small_btn,home && !displayed_pouring);
}
void show_home() {
  if(tap::pin_lock::locked()){tap::setup_mode=false;show_pin(PinMode::Unlock);return;}
  clear_page(); page=0; tap::setup_mode=false;
  chrome(true);
  lv_obj_set_pos(content,8,46);lv_obj_set_size(content,224,70);
  lv_obj_set_style_bg_opa(content,LV_OPA_TRANSP,0);
  lv_obj_set_style_pad_all(content,0,0);
  lv_obj_set_pos(pill,8,278);
  lv_obj_set_pos(auto_btn,displayed_pouring?POUR_X_CENTRED:POUR_X_IDLE,POUR_Y);lv_obj_set_size(auto_btn,150,150);
  lv_obj_set_scrollbar_mode(content,LV_SCROLLBAR_MODE_OFF);
  lv_obj_remove_flag(content,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_layout(content,LV_LAYOUT_NONE);
  value=label(content,"--",&lv_font_montserrat_36);
  lv_obj_set_pos(value,72,0);lv_obj_set_size(value,150,40);
  units=label(content,"Waiting for scale",&lv_font_montserrat_14,MUTED);
  lv_obj_set_pos(units,73,40);lv_obj_set_size(units,150,18);
  lv_label_set_long_mode(units,LV_LABEL_LONG_DOT);
  level_bar=lv_bar_create(content);
  lv_obj_set_pos(level_bar,73,60);lv_obj_set_size(level_bar,112,5);
  lv_bar_set_range(level_bar,0,100);lv_bar_set_value(level_bar,0,LV_ANIM_OFF);
  lv_obj_set_style_radius(level_bar,3,0);lv_obj_set_style_radius(level_bar,3,LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(level_bar,LV_OPA_COVER,0);lv_obj_set_style_bg_color(level_bar,lv_color_hex(RAISED),0);
  lv_obj_set_style_bg_opa(level_bar,LV_OPA_COVER,LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(level_bar,lv_color_hex(0xd89a2b),LV_PART_INDICATOR);
  lv_obj_set_style_pad_all(level_bar,0,0);
  level_text=label(content,"--",&lv_font_montserrat_14,MUTED);
  lv_obj_set_pos(level_text,186,55);lv_obj_set_size(level_text,38,16);
  lv_obj_set_style_text_align(level_text,LV_TEXT_ALIGN_RIGHT,0);
  for(auto o:{value,units,level_bar,level_text}) lv_obj_add_flag(o,LV_OBJ_FLAG_GESTURE_BUBBLE);
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
// ---- Setup building blocks ------------------------------------------------
void section(const char *text) {
  auto l=label(content,text,&lv_font_montserrat_14,MUTED);
  lv_obj_set_style_text_letter_space(l,1,0);lv_obj_set_style_pad_top(l,6,0);
}
lv_obj_t *group(bool padded) {
  auto g=lv_obj_create(content);lv_obj_remove_style_all(g);
  lv_obj_set_size(g,LV_PCT(100),LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(g,LV_OPA_COVER,0);lv_obj_set_style_bg_color(g,lv_color_hex(SURFACE),0);
  lv_obj_set_style_border_width(g,1,0);lv_obj_set_style_border_color(g,lv_color_hex(LINE),0);
  lv_obj_set_style_radius(g,12,0);
  lv_obj_set_style_pad_all(g,padded?10:0,0);lv_obj_set_style_pad_row(g,padded?6:0,0);
  lv_obj_set_flex_flow(g,LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(g,LV_OBJ_FLAG_SCROLLABLE);
  return g;
}
void rule(lv_obj_t *parent) {
  auto r=lv_obj_create(parent);lv_obj_remove_style_all(r);
  lv_obj_set_size(r,LV_PCT(100),1);
  lv_obj_set_style_bg_opa(r,LV_OPA_COVER,0);lv_obj_set_style_bg_color(r,lv_color_hex(LINE),0);
}
// A full-width list row; danger rows are red text without a chevron.
lv_obj_t *row(lv_obj_t *parent,const char *text,lv_event_cb_t cb,bool danger=false) {
  auto b=lv_button_create(parent);lv_obj_remove_style_all(b);
  lv_obj_set_size(b,LV_PCT(100),44);lv_obj_remove_flag(b,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_hor(b,12,0);
  lv_obj_set_style_bg_color(b,lv_color_hex(RAISED),LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(b,LV_OPA_COVER,LV_STATE_PRESSED);
  auto l=lv_label_create(b);lv_label_set_text(l,text);
  lv_obj_set_style_text_font(l,&lv_font_montserrat_16,0);
  lv_obj_set_style_text_color(l,lv_color_hex(danger?DANGER:TEXT),0);
  lv_obj_align(l,LV_ALIGN_LEFT_MID,0,0);
  if(!danger){
    auto chevron=lv_label_create(b);lv_label_set_text(chevron,LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chevron,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_color(chevron,lv_color_hex(MUTED),0);
    lv_obj_align(chevron,LV_ALIGN_RIGHT_MID,0,0);
  }
  if(cb)lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,nullptr);
  return b;
}
// Caption plus a dark input; `line` receives the row so a button can sit beside it.
lv_obj_t *field(lv_obj_t *parent,const char *caption,const char *text,bool secret=false,lv_obj_t **line=nullptr) {
  label(parent,caption,&lv_font_montserrat_14,MUTED);
  auto holder=lv_obj_create(parent);lv_obj_remove_style_all(holder);
  lv_obj_set_size(holder,LV_PCT(100),LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(holder,LV_FLEX_FLOW_ROW);lv_obj_set_style_pad_column(holder,6,0);
  lv_obj_remove_flag(holder,LV_OBJ_FLAG_SCROLLABLE);
  auto f=lv_textarea_create(holder);
  lv_obj_set_height(f,38);lv_obj_set_flex_grow(f,1);lv_textarea_set_one_line(f,true);
  lv_obj_set_style_bg_color(f,lv_color_hex(BG),0);lv_obj_set_style_bg_opa(f,LV_OPA_COVER,0);
  lv_obj_set_style_border_width(f,1,0);lv_obj_set_style_border_color(f,lv_color_hex(LINE),0);
  lv_obj_set_style_border_color(f,lv_color_hex(ACCENT),LV_STATE_FOCUSED);
  lv_obj_set_style_radius(f,8,0);lv_obj_set_style_text_color(f,lv_color_hex(TEXT),0);
  lv_obj_set_style_text_font(f,&lv_font_montserrat_14,0);
  lv_obj_set_style_pad_hor(f,10,0);lv_obj_set_style_pad_ver(f,10,0);
  lv_textarea_set_max_length(f,secret?64:127);lv_textarea_set_password_mode(f,secret);
  lv_textarea_set_text(f,text);lv_obj_add_event_cb(f,focus,LV_EVENT_FOCUSED,nullptr);
  if(line)*line=holder;
  return f;
}
lv_obj_t *choices(lv_obj_t *parent,const char *options) {
  auto d=lv_dropdown_create(parent);lv_obj_set_width(d,LV_PCT(100));lv_dropdown_set_options(d,options);
  lv_obj_set_style_bg_color(d,lv_color_hex(BG),0);lv_obj_set_style_bg_opa(d,LV_OPA_COVER,0);
  lv_obj_set_style_border_width(d,1,0);lv_obj_set_style_border_color(d,lv_color_hex(LINE),0);
  lv_obj_set_style_radius(d,8,0);lv_obj_set_style_text_color(d,lv_color_hex(TEXT),0);
  lv_obj_set_style_text_font(d,&lv_font_montserrat_14,0);
  auto list=lv_dropdown_get_list(d);
  lv_obj_set_style_bg_color(list,lv_color_hex(SURFACE),0);lv_obj_set_style_border_color(list,lv_color_hex(LINE),0);
  lv_obj_set_style_text_color(list,lv_color_hex(TEXT),0);
  return d;
}
void show_setup() {
  if(tap::pin_lock::locked()){show_pin(PinMode::Unlock);return;}
  if(tap::busy()) { message("Stop pouring before Setup"); return; }
  clear_page(); page=1; tap::setup_mode=true;
  // Setup cannot pour: the pour buttons hide and the area below the status
  // pill becomes one scrolling list of grouped settings.
  chrome(false);
  lv_label_set_text(title,"Setup");
  lv_obj_set_pos(pill,8,50);
  set_tone(Tone::Waiting);
  lv_obj_set_pos(content,8,78);lv_obj_set_size(content,224,242);
  lv_obj_set_style_bg_opa(content,LV_OPA_TRANSP,0);
  lv_obj_set_style_pad_all(content,0,0);lv_obj_set_style_pad_bottom(content,12,0);
  lv_obj_set_style_pad_row(content,8,0);
  lv_obj_add_flag(content,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(content,LV_DIR_VER);
  lv_obj_set_scrollbar_mode(content,LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_flex_flow(content,LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(content,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);

  section("SCREEN LOCK");
  auto lock=group(false);
  if(tap::pin_lock::configured()){
    row(lock,"Lock screen",[](lv_event_t *){
      if(tap::pin_lock::lock()){clear_pin_secrets();tap::setup_mode=false;show_pin(PinMode::Unlock);}
    });
    rule(lock);
    row(lock,"Change PIN",[](lv_event_t *){begin_pin(PinMode::VerifyChange);});
    rule(lock);
    row(lock,"Remove PIN",[](lv_event_t *){begin_pin(PinMode::Remove);},true);
  }else row(lock,"Set lock PIN",[](lv_event_t *){begin_pin(PinMode::New);});

  section("WI-FI");
  auto wifi=group(true);lv_obj_t *line=nullptr;
  ssid=field(wifi,"Network",saved.ssid,false,&line);lv_textarea_set_max_length(ssid,32);
  raised(line,"Scan",[](lv_event_t *){submit("scan_wifi");},56,38);
  network_list=choices(wifi,"Scan to list networks");
  lv_obj_add_event_cb(network_list,[](lv_event_t *){char b[100];lv_dropdown_get_selected_str(network_list,b,sizeof(b));lv_textarea_set_text(ssid,b);},LV_EVENT_VALUE_CHANGED,nullptr);
  password=field(wifi,"Password",saved.password,true,&line);
  raised(line,LV_SYMBOL_EYE_OPEN,[](lv_event_t *){lv_textarea_set_password_mode(password,!lv_textarea_get_password_mode(password));},44,38);

  section("SCALE");
  auto scale=group(true);
  host=field(scale,"Scale IP or hostname",saved.host,false,&line);
  raised(line,"Find",[](lv_event_t *){submit("discover");},56,38);
  scale_list=choices(scale,"Find scale or enter IP above");
  lv_obj_add_event_cb(scale_list,[](lv_event_t *){char b[128];lv_dropdown_get_selected_str(scale_list,b,sizeof(b));if(strncmp(b,"Manual",6))lv_textarea_set_text(host,b);},LV_EVENT_VALUE_CHANGED,nullptr);
  label(scale,"To pair, choose Add touchscreen on the scale.",&lv_font_montserrat_14,MUTED);
  auto forget=row(scale,"Remove scale pairing",[](lv_event_t *){submit("forget");},true);
  lv_obj_set_style_pad_hor(forget,0,0);lv_obj_set_height(forget,36);

  auto save=lv_button_create(content);lv_obj_remove_style_all(save);
  lv_obj_set_size(save,LV_PCT(100),44);
  lv_obj_set_style_radius(save,10,0);lv_obj_set_style_bg_opa(save,LV_OPA_COVER,0);
  lv_obj_set_style_bg_grad_dir(save,LV_GRAD_DIR_VER,0);
  lv_obj_set_style_bg_color(save,lv_color_hex(0x33b8ff),0);lv_obj_set_style_bg_grad_color(save,lv_color_hex(0x0a6fd0),0);
  lv_obj_set_style_bg_color(save,lv_color_hex(0x1a8ad8),LV_STATE_PRESSED);
  auto save_text=lv_label_create(save);lv_label_set_text(save_text,"Save and connect");
  lv_obj_set_style_text_color(save_text,lv_color_hex(0xffffff),0);
  lv_obj_set_style_text_font(save_text,&lv_font_montserrat_16,0);lv_obj_center(save_text);
  lv_obj_add_event_cb(save,[](lv_event_t *){
    auto j=cJSON_CreateObject();cJSON_AddStringToObject(j,"ssid",lv_textarea_get_text(ssid));
    cJSON_AddStringToObject(j,"password",lv_textarea_get_text(password));cJSON_AddStringToObject(j,"host",lv_textarea_get_text(host));
    cJSON_AddNumberToObject(j,"brightness",saved.brightness);cJSON_AddNumberToObject(j,"slot",0);submit("settings",j);
  },LV_EVENT_CLICKED,nullptr);

  section("SOUND");
  auto sound=group(false);
  auto sound_row=lv_obj_create(sound);lv_obj_remove_style_all(sound_row);
  lv_obj_set_size(sound_row,LV_PCT(100),44);lv_obj_set_style_pad_hor(sound_row,12,0);
  lv_obj_remove_flag(sound_row,LV_OBJ_FLAG_SCROLLABLE);
  auto sound_text=lv_label_create(sound_row);lv_label_set_text(sound_text,"Pour sounds");
  lv_obj_set_style_text_font(sound_text,&lv_font_montserrat_16,0);
  lv_obj_set_style_text_color(sound_text,lv_color_hex(TEXT),0);lv_obj_align(sound_text,LV_ALIGN_LEFT_MID,0,0);
  auto toggle=lv_switch_create(sound_row);lv_obj_set_size(toggle,44,24);lv_obj_align(toggle,LV_ALIGN_RIGHT_MID,0,0);
  lv_obj_set_style_bg_color(toggle,lv_color_hex(LINE),0);
  lv_obj_set_style_bg_color(toggle,lv_color_hex(0x1a8aa8),
                            lv_style_selector_t(LV_PART_INDICATOR)|lv_style_selector_t(LV_STATE_CHECKED));
  if(tap::audio_enabled())lv_obj_add_state(toggle,LV_STATE_CHECKED);
  lv_obj_add_event_cb(toggle,[](lv_event_t *event){
    auto sw=static_cast<lv_obj_t *>(lv_event_get_target(event));
    const bool enabled=lv_obj_has_state(sw,LV_STATE_CHECKED);
    if(!tap::audio_set_enabled(enabled)){
      message("Could not save sound setting");
      if(enabled)lv_obj_remove_state(sw,LV_STATE_CHECKED);else lv_obj_add_state(sw,LV_STATE_CHECKED);
    }
  },LV_EVENT_VALUE_CHANGED,nullptr);

  section("DEVICE WEBSITE");
  auto site=group(true);
  lv_obj_set_flex_align(site,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  auto about=label(site,"Set Full, 1/2 and Small Cup levels, test the servo and see the last five pours.",&lv_font_montserrat_14,0xc8d5de);
  lv_obj_set_style_text_align(about,LV_TEXT_ALIGN_CENTER,0);
  auto netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");esp_netif_ip_info_t ip{};char b[96];
  if(netif && esp_netif_get_ip_info(netif,&ip)==ESP_OK && ip.ip.addr) {
    snprintf(b,sizeof(b),"http://" IPSTR,IP2STR(&ip.ip));
    auto qr=lv_qrcode_create(site);lv_qrcode_set_size(qr,120);lv_qrcode_update(qr,b,strlen(b));
    lv_obj_set_style_border_width(qr,4,0);lv_obj_set_style_border_color(qr,lv_color_hex(0xffffff),0);
    auto url=label(site,b,&lv_font_montserrat_14,ACCENT);lv_obj_set_style_text_align(url,LV_TEXT_ALIGN_CENTER,0);
  }else label(site,"Connect to Wi-Fi to show the address.",&lv_font_montserrat_14,MUTED);

  snprintf(b,sizeof(b),"TouchPour %s",esp_app_get_description()->version);
  auto version=label(content,b,&lv_font_montserrat_14,MUTED);lv_obj_set_style_text_align(version,LV_TEXT_ALIGN_CENTER,0);
  message("Pouring paused in Setup");
}
// Never fall back to another pour type when the chosen one is unavailable.
void request_pour(tap::PourKind kind){
  if(page || pairing || tap::pin_lock::locked() || pin_view.root)return;
  if(tap::busy()){tap::stop_requested=true;return;}
  if(kind!=tap::PourKind::Full){
    const bool half=kind==tap::PourKind::Half;
    if(xSemaphoreTake(tap::state_mutex,pdMS_TO_TICKS(20))!=pdTRUE){message("Busy; try again");return;}
    const int target=half?tap::half_stop_mm:tap::small_stop_mm;
    const bool ready=tap::extra_ready(tap::settings,tap::control,tap::sample,esp_timer_get_time()/1000,target);
    xSemaphoreGive(tap::state_mutex);
    if(!ready){
      if(half)message(target?"Half level reached or cup not ready":"Set half level on website");
      else message(target?"Small-cup level reached or not ready":"Set small-cup level on website");
      return;
    }
  }
  tap::Action action{kind==tap::PourKind::Half?tap::Action::START_HALF:
                     kind==tap::PourKind::Small?tap::Action::START_SMALL:tap::Action::START,0};
  if(xQueueSend(tap::actions,&action,0)!=pdTRUE)message("Busy; try again");
}
void auto_action(lv_event_t *){
  // Include the lower rim in the half-pour hit region, even if LVGL dispatches
  // a tap on a disabled half child to its parent. Never fall back to full pour.
  bool half=false;
  if(auto input=lv_indev_active()){
    lv_point_t point;lv_indev_get_point(input,&point);
    lv_area_t area;lv_obj_get_coords(auto_btn,&area);half=pour_button::half_at(area.y1,point.y);
  }
  request_pour(half?tap::PourKind::Half:tap::PourKind::Full);
}
void half_action(lv_event_t *){request_pour(tap::PourKind::Half);}
void small_action(lv_event_t *){request_pour(tap::PourKind::Small);}
void poll(lv_timer_t *) {
  if(tap::pin_lock::locked()){
    if(!pin_view.root){clear_pin_secrets();tap::setup_mode=false;show_pin(PinMode::Unlock);}
    uint32_t seconds=tap::pin_lock::retry_seconds();
    if(seconds){char text[40];snprintf(text,sizeof(text),"Try again in %lu s",(unsigned long)seconds);text_if_changed(pin_view.message,text);}
    else if(std::strncmp(lv_label_get_text(pin_view.message),"Try again",9)==0)text_if_changed(pin_view.message,"Enter PIN");
    return;
  }
  if(pin_view.root)return;
  if(xSemaphoreTake(tap::state_mutex,0)!=pdTRUE)return;
  bool active=tap::pouring_active() || tap::manual_test_active;
  bool ready=tap::control.ready(tap::settings,esp_timer_get_time()/1000);
  bool half_ready=tap::half_ready(tap::settings,tap::control,tap::sample,esp_timer_get_time()/1000,tap::half_stop_mm);
  bool small_ready=tap::small_ready(tap::settings,tap::control,tap::sample,esp_timer_get_time()/1000,tap::small_stop_mm);
  bool blocked=page || pairing || tap::setup_mode || tap::firmware_updating || tap::manual_test_active || tap::history_saving;
  const bool automatic=tap::control.pouring;
  const tap::PourKind kind=tap::active_pour;
  const tap::Settings config=tap::settings;
  const tap::Sample measurement=tap::sample;
  const uint32_t age=uint32_t(esp_timer_get_time()/1000)-measurement.at;
  const bool fresh=age<=uint32_t(tap::settings.stale_ms);
  const char *idle=tap::idle_status(config,measurement,uint32_t(esp_timer_get_time()/1000),ready,tap::control.reason);
  xSemaphoreGive(tap::state_mutex);
  if(active || (!blocked && ready))lv_obj_remove_state(auto_btn,LV_STATE_DISABLED);else lv_obj_add_state(auto_btn,LV_STATE_DISABLED);
  if(active || (!blocked && half_ready))lv_obj_remove_state(split_button.half,LV_STATE_DISABLED);else lv_obj_add_state(split_button.half,LV_STATE_DISABLED);
  if(!blocked && small_ready)lv_obj_remove_state(small_btn,LV_STATE_DISABLED);else lv_obj_add_state(small_btn,LV_STATE_DISABLED);
  // Fonts, alignment and visibility invalidate the button even when unchanged.
  // Keep the 50 ms safety/status poll, but repaint the face only on transitions.
  // While pouring the stop face is centred and SMALL CUP is hidden.
  if(!pour_state_known || active!=displayed_pouring) {
    lv_label_set_text(auto_text,active?"EMERGENCY\nSTOP":"FULL\nPOUR");
    lv_obj_align(auto_text,LV_ALIGN_CENTER,0,active?0:-28);
    lv_obj_set_x(auto_btn,active?POUR_X_CENTRED:POUR_X_IDLE);
    show_hidden(small_btn,!active && !page);
    pour_button::split_state(split_button,active);
    pour_button::update(auto_btn,active);
    displayed_pouring=active;pour_state_known=true;
  }
  char distance[48];
  if(measurement.valid && measurement.filtered_valid && fresh)snprintf(distance,sizeof(distance),"LiDAR: %d mm",measurement.stable_distance());
  else snprintf(distance,sizeof(distance),"LiDAR: %s",measurement.valid&&!fresh?"stale":"no reading");
  text_if_changed(footer,distance);
  if(!page && !pairing) {
    if(active){set_tone(Tone::Pouring);message(automatic?pouring_text(kind):"Touch anywhere to stop");}
    else{
      set_tone(!blocked && ready?Tone::Ready:Tone::Waiting);
      message(ready&&reading.valid&&!reading.online?"Cup ready - Scale stale":idle);
    }
  }
}
}
void ui_start(const Settings &s) {
  saved=s; touchpour_board_start();touchpour_brightness(s.brightness>=10?s.brightness:85);
  configASSERT(lvgl_port_lock(0));auto root=lv_screen_active();
  lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(root,lv_color_hex(BG),0);
  lv_obj_set_style_text_font(root,&lv_font_montserrat_14,0);
  // Header: beer name, scale link line and the menu/back button.
  title=label(root,"TouchPour");lv_obj_set_pos(title,12,6);lv_obj_set_width(title,176);lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
  link_dot=lv_obj_create(root);lv_obj_remove_style_all(link_dot);
  lv_obj_set_pos(link_dot,12,31);lv_obj_set_size(link_dot,6,6);
  lv_obj_set_style_radius(link_dot,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_opa(link_dot,LV_OPA_COVER,0);
  lv_obj_set_style_bg_color(link_dot,lv_color_hex(MUTED),0);lv_obj_remove_flag(link_dot,LV_OBJ_FLAG_CLICKABLE);
  link_text=label(root,"Waiting for scale",&lv_font_montserrat_14,MUTED);
  lv_obj_set_pos(link_text,22,25);lv_obj_set_width(link_text,166);lv_label_set_long_mode(link_text,LV_LABEL_LONG_DOT);
  menu_btn=raised(root,LV_SYMBOL_LIST,[](lv_event_t *){page?show_home():show_setup();},36,30);
  menu_icon=lv_obj_get_child(menu_btn,0);lv_obj_set_style_text_font(menu_icon,&lv_font_montserrat_16,0);
  divider=lv_obj_create(root);lv_obj_remove_style_all(divider);
  lv_obj_set_pos(divider,8,44);lv_obj_set_size(divider,224,1);
  lv_obj_set_style_bg_opa(divider,LV_OPA_COVER,0);lv_obj_set_style_bg_color(divider,lv_color_hex(RAISED),0);
  content=lv_obj_create(root);lv_obj_set_pos(content,8,46);lv_obj_set_size(content,224,70);
  lv_obj_set_style_bg_opa(content,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(content,0,0);
  lv_obj_set_style_pad_all(content,0,0);lv_obj_set_style_pad_row(content,8,0);
  lv_obj_add_event_cb(content,change_view,LV_EVENT_GESTURE,nullptr);
  // Status pill: coloured dot + one line of status text.
  pill=lv_obj_create(root);lv_obj_remove_style_all(pill);
  lv_obj_set_pos(pill,8,278);lv_obj_set_size(pill,224,20);
  lv_obj_set_style_radius(pill,10,0);lv_obj_set_style_bg_opa(pill,LV_OPA_COVER,0);
  lv_obj_set_style_border_width(pill,1,0);
  lv_obj_set_flex_flow(pill,LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(pill,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(pill,6,0);
  lv_obj_remove_flag(pill,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(pill,LV_OBJ_FLAG_SCROLLABLE);
  pill_dot=lv_obj_create(pill);lv_obj_remove_style_all(pill_dot);lv_obj_set_size(pill_dot,7,7);
  lv_obj_set_style_radius(pill_dot,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_opa(pill_dot,LV_OPA_COVER,0);
  lv_obj_remove_flag(pill_dot,LV_OBJ_FLAG_CLICKABLE);
  notice=label(pill,"",&lv_font_montserrat_14);
  lv_obj_set_width(notice,LV_SIZE_CONTENT);lv_obj_set_style_max_width(notice,196,0);
  lv_label_set_long_mode(notice,LV_LABEL_LONG_DOT);
  set_tone(Tone::Neutral);
  // Split FULL / 1/2 pour button and the separate SMALL CUP button.
  auto_btn=lv_button_create(root);auto_text=lv_label_create(auto_btn);lv_label_set_text(auto_text,"FULL\nPOUR");
  pour_button::style(auto_btn);
  lv_obj_set_pos(auto_btn,POUR_X_IDLE,POUR_Y);lv_obj_set_size(auto_btn,150,150);
  lv_obj_add_event_cb(auto_btn,auto_action,LV_EVENT_CLICKED,nullptr);
  lv_obj_set_style_text_font(auto_text,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(auto_text,lv_color_hex(0xffffff),0);
  lv_obj_set_style_text_align(auto_text,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_align(auto_text,LV_ALIGN_CENTER,0,-28);
  split_button=pour_button::add_half(auto_btn,half_action);
  small_btn=pour_button::add_small(root,small_action);
  lv_obj_set_pos(small_btn,166,POUR_Y+(150-pour_button::kSmallSize)/2);
  footer=label(root,"LiDAR: waiting",&lv_font_montserrat_14,MUTED);
  lv_obj_set_pos(footer,8,302);lv_obj_set_size(footer,224,16);lv_obj_set_style_text_align(footer,LV_TEXT_ALIGN_CENTER,0);
  if(saved.ssid[0])show_home();else show_setup();lv_timer_create(poll,50,nullptr);lvgl_port_unlock();
}
void ui_state(const State &s) {if(lvgl_port_lock(1000)){reading=s;home_value();lvgl_port_unlock();}}
void ui_message(const char *s) {if(!notice)return;if(lvgl_port_lock(1000)){message(s);lvgl_port_unlock();}}
void ui_message_for_generation(const char *s,uint32_t g){if(lvgl_port_lock(1000)){if(g==generation)message(s);lvgl_port_unlock();}}
void ui_result(bool ok,const char *op,const char *err){char b[128];snprintf(b,sizeof(b),"%s: %s",op,ok?"Done":err);ui_message(b);}
void ui_pair_code(const char *code){if(lvgl_port_lock(1000)){pairing=true;if(!tap::pin_lock::locked())tap::setup_mode=true;char b[80];snprintf(b,sizeof(b),"Pairing code: %s",code);message(b);lvgl_port_unlock();}}
void ui_paired(){if(lvgl_port_lock(1000)){pairing=false;tap::setup_mode=!tap::pin_lock::locked()&&page!=0;message("Scale paired");lvgl_port_unlock();}}
bool touchscreen_pairing_overlay_visible(){return pairing;}
void touchscreen_pairing_window(uint32_t seconds){char b[80];snprintf(b,sizeof(b),"Pairing: %lu seconds remaining",(unsigned long)seconds);ui_message(b);}
void touchscreen_pairing_ended(){if(lvgl_port_lock(1000)){pairing=false;tap::setup_mode=!tap::pin_lock::locked()&&page!=0;message("Pairing ended");lvgl_port_unlock();}}
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
