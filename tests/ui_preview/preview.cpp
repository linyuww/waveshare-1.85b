// Headless LVGL render of the production assets and AppLauncher flex layout.
// The phone manager and touch driver are not emulated. Geometry follows the
// Brookesia AppLauncher/Icon updateByNewData/createMixObject implementations.
#include "lvgl.h"
#include "ui_assets.h"
#include "ui_layout.hpp"
#include "battery_info.hpp"
#include "ui_app_shell.hpp"
#include <cstdio>
#include <initializer_list>
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
namespace l = launcher_layout;
extern "C" { LV_IMAGE_DECLARE(scr_bg); LV_IMAGE_DECLARE(esp_brookesia_image_large_status_bar_wifi_level3_36_36); LV_FONT_DECLARE(esp_brookesia_font_maison_neue_book_18); }
static uint16_t buffer[360*360];
static uint16_t pixels[360*360];
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p) {
    auto *src=reinterpret_cast<uint16_t *>(p);
    for(int y=a->y1;y<=a->y2;++y) for(int x=a->x1;x<=a->x2;++x) pixels[y*360+x]=*src++;
    lv_display_flush_ready(d);
}
static lv_obj_t *box(lv_obj_t *parent,int w,int h) {
    auto *o=lv_obj_create(parent); lv_obj_remove_style_all(o); lv_obj_set_size(o,w,h);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE); return o;
}
static lv_obj_t *text(lv_obj_t *parent,const char *str) {
    auto *o=lv_label_create(parent);lv_obj_remove_style_all(o);
    lv_obj_set_style_text_font(o,&ui_font,0);lv_obj_set_style_text_color(o,lv_color_hex(0xFFFFFF),0);
    lv_label_set_text(o,str);return o;
}
static void save(const char *path) {
    lv_obj_update_layout(lv_screen_active()); lv_refr_now(nullptr);
    auto *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n360 360\n255\n");
    for(auto v:pixels){unsigned char rgb[]={static_cast<unsigned char>(((v>>11)&31)*255/31),static_cast<unsigned char>(((v>>5)&63)*255/63),static_cast<unsigned char>((v&31)*255/31)};fwrite(rgb,1,3,f);}fclose(f);
}
int main(int argc,char **argv) {
    assert(argc==2);lv_init();auto *d=lv_display_create(360,360);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d,buffer,nullptr,sizeof(buffer),LV_DISPLAY_RENDER_MODE_FULL);lv_display_set_flush_cb(d,flush);
    auto *screen=lv_screen_active();lv_obj_remove_style_all(screen);lv_obj_set_style_bg_opa(screen,255,0);
    lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    const lv_image_dsc_t *icons[]={&icon_settings,&icon_clock,&icon_network,&icon_about,&icon_codex,&icon_fitness,&icon_assistant,&icon_music};
    const char *names[]={"设置","时钟","网络测试","设备信息","Codex Micro","健身","小智助手","音乐播放器"};
    for(int page=0;page<2;++page) {
        lv_obj_clean(screen);auto *wall=lv_image_create(screen);lv_image_set_src(wall,&scr_bg);lv_obj_set_pos(wall,0,0);
        // Static values for inspection; live firmware uses its existing status bar.
        auto *status=box(screen,360,36);lv_obj_align(status,LV_ALIGN_TOP_MID,0,0);
        lv_obj_set_flex_flow(status,LV_FLEX_FLOW_ROW);lv_obj_set_flex_align(status,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);lv_obj_set_style_pad_column(status,3,0);
        auto *wifi=lv_image_create(status);lv_obj_remove_style_all(wifi);lv_image_set_src(wifi,&esp_brookesia_image_large_status_bar_wifi_level3_36_36);
        lv_image_set_scale(wifi,17*256/36);lv_obj_set_size(wifi,17,17);lv_image_set_inner_align(wifi,LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_style_image_recolor(wifi,lv_color_hex(0xFFFFFF),0);lv_obj_set_style_image_recolor_opa(wifi,255,0);
        auto *clock=text(status,"18:58");lv_obj_set_style_text_font(clock,&esp_brookesia_font_maison_neue_book_18,0);
        auto *main=box(screen,l::screen,l::height);lv_obj_align(main,LV_ALIGN_TOP_MID,0,l::top);
        auto *table=box(main,l::table_width,l::table_height);lv_obj_align(table,LV_ALIGN_TOP_MID,0,0);
        lv_obj_set_flex_flow(table,LV_FLEX_FLOW_ROW_WRAP);lv_obj_set_flex_align(table,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
        int rowpad=(l::table_height-2*l::tile)/3,colpad=(l::table_width-2*l::tile)/3;
        lv_obj_set_style_pad_row(table,rowpad,0);lv_obj_set_style_pad_ver(table,rowpad,0);
        lv_obj_set_style_pad_column(table,colpad,0);lv_obj_set_style_pad_hor(table,colpad,0);
        for(int i=page*4;i<(page==0?4:8);++i) {
            auto *tile=box(table,l::tile,l::tile);lv_obj_set_flex_flow(tile,LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(tile,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);lv_obj_set_style_pad_row(tile,l::label_gap,0);
            auto *imagebox=box(tile,l::icon,l::icon);auto *img=lv_image_create(imagebox);lv_obj_remove_style_all(img);
            lv_image_set_src(img,icons[i]);lv_obj_set_size(img,l::icon,l::icon);lv_image_set_inner_align(img,LV_IMAGE_ALIGN_CENTER);lv_obj_center(img);
            auto *name=text(tile,names[i]);lv_obj_update_layout(screen);
            for(auto *o:{imagebox,name}) {lv_area_t a;lv_obj_get_coords(o,&a);for(int x:{a.x1,a.x2})for(int y:{a.y1,a.y2})assert(std::hypot(x-179.5,y-179.5)<178);}
            lv_area_t a;lv_obj_get_coords(imagebox,&a);printf("page %d %s: icon (%d,%d)-(%d,%d)\n",page+1,names[i],a.x1,a.y1,a.x2,a.y2);
        }
        auto *indicator=box(main,l::screen,l::indicator_height);lv_obj_align(indicator,LV_ALIGN_BOTTOM_MID,0,-l::indicator_bottom);
        lv_obj_set_flex_flow(indicator,LV_FLEX_FLOW_ROW);lv_obj_set_flex_align(indicator,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(indicator,l::indicator_gap,0);
        for(int i=0;i<2;++i){auto *dot=box(indicator,i==page?l::active_dot_width:l::dot,l::dot);lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(dot,lv_color_hex(i==page?0xFFFFFF:0x5F7C96),0);lv_obj_set_style_bg_opa(dot,255,0);}
        char path[512];snprintf(path,sizeof(path),"%s/desktop-%d.ppm",argv[1],page+1);save(path);
    }
    // Check the production battery formatter with a complete reference sample.
    // Values below are inspection fixtures, not newly measured hardware results.
    battery::Sample sample;sample.valid=true;sample.stale=false;sample.percent=32;sample.gaugePercent=32;
    sample.voltageMv=4138;sample.currentMa=0;sample.temperatureDeciC=340;sample.remainingCapacityMah=957;
    sample.fullChargeCapacityMah=3000;sample.designCapacityMah=3000;sample.internalTemperatureDeciC=340;sample.healthPercent=100;sample.externalPower=true;sample.statusBits=0x5028;
    char telemetry[1536];formatBatteryInfo(telemetry,sizeof(telemetry),sample,1000);
    assert(strstr(telemetry,"4138 mV"));assert(strstr(telemetry,"电流：0 mA"));assert(strstr(telemetry,"预计放空：--"));
    for (const unsigned char *p=reinterpret_cast<const unsigned char *>(telemetry);*p;) {
        uint32_t cp=*p++;
        if(cp>=0xC0) {int remaining=cp<0xE0?1:cp<0xF0?2:3;cp &= (1u << (6-remaining))-1;while(remaining--)cp=(cp<<6)|(*p++&63);}
        if(cp=='\n' || cp==' ')continue;
        lv_font_glyph_dsc_t glyph{};bool found=lv_font_get_glyph_dsc(&ui_font,&glyph,cp,0);
        if(!found || glyph.is_placeholder)fprintf(stderr,"Missing battery text glyph U+%04X\n",cp);
        assert(found && !glyph.is_placeholder);
    }
    FILE *f=fopen((std::string(argv[1])+"/battery-text.txt").c_str(),"wb");assert(f);fwrite(telemetry,1,strlen(telemetry),f);fclose(f);
    lv_obj_clean(screen);
    // This frame is the same helper the firmware calls, with inspection data.
    const auto shell=createLauncherAppShell(screen,"设备信息",true);
    auto *info=text(shell.body,telemetry);lv_obj_set_width(info,LV_PCT(100));
    lv_obj_update_layout(screen);
    for(auto *obj:{shell.title,lv_obj_get_child(shell.home,0)}) {
        lv_area_t a;lv_obj_get_coords(obj,&a);
        for(int x:{a.x1,a.x2})for(int y:{a.y1,a.y2})assert(std::hypot(x-179.5,y-179.5)<178);
    }
    lv_area_t content;lv_obj_get_content_coords(shell.body,&content);
    for(int x:{content.x1,content.x2})for(int y:{content.y1,content.y2})assert(std::hypot(x-179.5,y-179.5)<178);
    printf("App content: (%d,%d)-(%d,%d)\n",content.x1,content.y1,content.x2,content.y2);
    save((std::string(argv[1])+"/device-info.ppm").c_str());
    lv_deinit();return 0;
}
