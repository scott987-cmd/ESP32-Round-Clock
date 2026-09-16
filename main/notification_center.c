#include "notification_center.h"
#include "app_views.h"
#include "notification_store.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"

typedef notification_entry_t entry_t;
static notification_history_t history;
static QueueHandle_t inbox;
static lv_obj_t *panel, *cards, *badge, *badge_text, *summary;
static const lv_font_t *font;
static void (*open_view)(unsigned);
static bool visible, dirty, save_failed;
static uint32_t last_save;
static lv_point_t start;

esp_err_t notification_post(const char *key,const char *text,unsigned view,notice_state_t state)
{
    if(!inbox || !key || !text || strlen(key)>512 ||
       strlen(text)>=sizeof(((entry_t *)0)->text) || view>=APP_VIEW_COUNT || state>NOTICE_FAILED) return ESP_ERR_INVALID_ARG;
    entry_t item={.view=view,.state=state,.unread=state!=NOTICE_WORKING,.created=time(NULL)};
    if(mbedtls_sha256((const unsigned char *)key,strlen(key),item.key,0)!=0) return ESP_FAIL;
    item.fixture=!strncmp(key,"__test_notice:",14);
    strlcpy(item.text,text,sizeof(item.text));
    return xQueueSend(inbox,&item,0)==pdTRUE?ESP_OK:ESP_ERR_NO_MEM;
}
static lv_obj_t *text_at(lv_obj_t *parent,const char *text,int x,int y,int width)
{
    lv_obj_t *o=lv_label_create(parent); lv_label_set_text(o,text);
    lv_obj_set_pos(o,x,y); lv_obj_set_width(o,width);
    lv_obj_set_style_text_font(o,font,0); lv_obj_set_style_text_color(o,lv_color_hex(0x26354A),0);
    return o;
}
static void save(void)
{
    nvs_handle_t handle; esp_err_t r=nvs_open("notice_center",NVS_READWRITE,&handle);
    if(r==ESP_OK) { r=nvs_set_blob(handle,"history",&history,sizeof(history));
        if(r==ESP_OK) r=nvs_commit(handle);
        nvs_close(handle); }
    save_failed=r!=ESP_OK; dirty=save_failed; last_save=lv_tick_get();
}
static void render(void);
void notification_center_close(void)
{
    if(!panel) return;
    visible=false; lv_obj_add_flag(panel,LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(badge,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(badge);
}
static void close_event(lv_event_t *e) { (void)e; notification_center_close(); }
static void card_event(lv_event_t *e)
{
    unsigned i=(unsigned)(uintptr_t)lv_event_get_user_data(e);
    if(i>=history.count) return;
    history.entries[i].unread=0; dirty=true;
    unsigned view=history.entries[i].view; notification_center_close(); render(); open_view(view);
}
static void read_event(lv_event_t *e)
{
    (void)e;
    for(unsigned i=0;i<history.count;++i) history.entries[i].unread=0;
    dirty=true; render();
}
static lv_obj_t *button(lv_obj_t *parent,const char *title,int x,int y,int width,lv_event_cb_t cb)
{
    lv_obj_t *o=lv_button_create(parent); lv_obj_set_pos(o,x,y); lv_obj_set_size(o,width,42);
    lv_obj_set_style_radius(o,21,0); lv_obj_set_style_shadow_width(o,0,0);
    lv_obj_set_style_bg_color(o,lv_color_hex(0xE0EAFB),0);
    lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t *t=text_at(o,title,0,0,width); lv_obj_set_style_text_align(t,LV_TEXT_ALIGN_CENTER,0); lv_obj_center(t);
    return o;
}
static void render(void)
{
    unsigned unread=0,working=0;
    for(unsigned i=0;i<history.count;++i) { unread+=!!history.entries[i].unread; working+=history.entries[i].state==NOTICE_WORKING; }
    lv_label_set_text_fmt(badge_text,"消息 %u",unread);
    lv_label_set_text_fmt(summary,save_failed?"保存失败 · 正在重试":"%u 条未读 · %u 项处理中",unread,working);
    if(!inbox) lv_label_set_text(summary,"通知队列不可用，请重启设备");
    if(!visible) return;
    int scroll=lv_obj_get_scroll_y(cards); lv_obj_clean(cards);
    lv_obj_set_flex_align(cards,history.count?LV_FLEX_ALIGN_START:LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_START);
    if(!history.count) {
        lv_obj_t *empty=text_at(cards,"暂时没有消息\n\n完成结果会留在这里",0,0,310);
        lv_obj_set_style_text_align(empty,LV_TEXT_ALIGN_CENTER,0);
    }
    for(unsigned i=0;i<history.count;++i) {
        entry_t *item=&history.entries[i];
        lv_obj_t *card=lv_button_create(cards); lv_obj_set_size(card,340,114);
        lv_obj_set_style_bg_color(card,lv_color_white(),0); lv_obj_set_style_radius(card,20,0);
        lv_obj_set_style_shadow_width(card,0,0); lv_obj_set_style_pad_all(card,12,0);
        lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(card,card_event,LV_EVENT_CLICKED,(void *)(uintptr_t)i);
        lv_obj_t *t=text_at(card,item->text,0,0,312); lv_obj_set_height(t,52); lv_label_set_long_mode(t,LV_LABEL_LONG_DOT);
        char line[100],stamp[24]=""; time_t when=item->created; struct tm local;
        if(when>1700000000 && localtime_r(&when,&local)) strftime(stamp,sizeof(stamp),"%m/%d %H:%M",&local);
        snprintf(line,sizeof(line),"%s%s  %s",item->state==NOTICE_WORKING?"处理中":item->state==NOTICE_FAILED?"需处理":"已完成",
                 item->unread?" · 未读":"",stamp);
        t=text_at(card,line,0,63,312); lv_obj_set_style_text_color(t,lv_color_hex(item->state==NOTICE_FAILED?0xA63B35:0x376AB1),0);
    }
    lv_obj_update_layout(cards); lv_obj_scroll_to_y(cards,scroll,LV_ANIM_OFF);
}
static void show(void)
{
    visible=true; lv_obj_add_flag(badge,LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(panel,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(panel); render();
}
static void edge_event(lv_event_t *e)
{
    lv_event_code_t code=lv_event_get_code(e);
    if(code==LV_EVENT_PRESSED) lv_indev_get_point(lv_indev_active(),&start);
    else if(code==LV_EVENT_RELEASED) {
        lv_point_t end; lv_indev_get_point(lv_indev_active(),&end);
        if(end.y-start.y>50 || (abs(end.x-start.x)<12 && abs(end.y-start.y)<12)) show();
    }
}
void notification_center_create(lv_obj_t *screen,const lv_font_t *body,const lv_font_t *small,const lv_font_t *heading,void (*open)(unsigned))
{
    font=body; open_view=open;
    inbox=xQueueCreateWithCaps(20,sizeof(entry_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    history.version=2;
    nvs_handle_t handle;
    if(nvs_open("notice_center",NVS_READONLY,&handle)==ESP_OK) {
        size_t bytes=sizeof(history);
        if(nvs_get_blob(handle,"history",&history,&bytes)!=ESP_OK || bytes!=sizeof(history) || history.version!=2 || history.count>NOTICE_LIMIT)
            memset(&history,0,sizeof(history));
        nvs_close(handle);
    }
    history.version=2;
    for(unsigned i=0;i<history.count;++i) {
        entry_t *item=&history.entries[i]; item->text[191]=0;
        if(item->view>=APP_VIEW_COUNT) item->view=0;
        if(item->state==NOTICE_WORKING) { item->state=NOTICE_FAILED; item->unread=1;
            strlcpy(item->text,"设备已重启，请进入应用确认上次任务",sizeof(item->text)); dirty=true; }
    }
    panel=lv_obj_create(screen); lv_obj_remove_style_all(panel); lv_obj_set_size(panel,466,466);
    lv_obj_set_style_bg_opa(panel,255,0); lv_obj_set_style_bg_color(panel,lv_color_hex(0xF0F5FC),0);
    lv_obj_remove_flag(panel,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title=text_at(panel,"通知中心",110,40,246); lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_font(title,heading,0);
    summary=text_at(panel,"",58,79,350); lv_obj_set_style_text_align(summary,LV_TEXT_ALIGN_CENTER,0);
    cards=lv_obj_create(panel); lv_obj_remove_style_all(cards); lv_obj_set_pos(cards,63,120); lv_obj_set_size(cards,340,258);
    lv_obj_set_scroll_dir(cards,LV_DIR_VER); lv_obj_set_flex_flow(cards,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cards,10,0); lv_obj_set_scrollbar_mode(cards,LV_SCROLLBAR_MODE_AUTO);
    button(panel,"全部已读",85,397,146,read_event); button(panel,"返回",245,397,136,close_event);
    lv_obj_add_flag(panel,LV_OBJ_FLAG_HIDDEN);
    badge=lv_obj_create(screen); lv_obj_remove_style_all(badge); lv_obj_set_pos(badge,185,6); lv_obj_set_size(badge,96,22);
    lv_obj_add_flag(badge,LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(badge,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(badge,lv_color_hex(0xE0EAFB),0); lv_obj_set_style_bg_opa(badge,220,0); lv_obj_set_style_radius(badge,14,0);
    badge_text=text_at(badge,"消息 0",0,0,96); lv_obj_set_style_text_font(badge_text,small,0);
    lv_obj_set_style_text_align(badge_text,LV_TEXT_ALIGN_CENTER,0); lv_obj_center(badge_text);
    lv_obj_add_event_cb(badge,edge_event,LV_EVENT_ALL,NULL); render();
}
void notification_center_tick(void)
{
    if(!inbox) return;
    bool changed=false; entry_t item;
    while(xQueueReceive(inbox,&item,0)==pdTRUE) {
        if(notification_history_apply(&history,&item)) { changed=true; dirty=true; }
    }
    if(dirty && lv_tick_get()-last_save>2000) {save(); changed=true;}
    if(changed) render();
}
void notification_center_debug(cJSON *root)
{
    unsigned unread=0; for(unsigned i=0;i<history.count;++i) unread+=!!history.entries[i].unread;
    cJSON_AddBoolToObject(root,"notifications_open",visible);
    cJSON_AddNumberToObject(root,"notification_count",history.count);
    cJSON_AddNumberToObject(root,"notification_unread",unread);
    cJSON_AddBoolToObject(root,"notification_save_failed",save_failed);
}
esp_err_t notification_center_test(unsigned phase)
{
    if(phase==5) {
        /* Cleanup only the exact two local short-recording regression outcomes.
           The runner must first verify that no prior user notification exists. */
        const char *keys[]={"music","wallpaper"};
        const char *messages[]={"音乐处理失败，点此查看并重试","壁纸处理失败，点此查看并重试"};
        for(unsigned k=0;k<2;++k) {
            unsigned char hash[32];
            if(mbedtls_sha256((const unsigned char *)keys[k],strlen(keys[k]),hash,0)) return ESP_FAIL;
            for(unsigned i=0;i<history.count;++i) {
                entry_t *item=&history.entries[i];
                if(!memcmp(item->key,hash,32) && item->state==NOTICE_FAILED && !strcmp(item->text,messages[k])) {
                    memmove(item,item+1,(history.count-i-1)*sizeof(*item)); --history.count; dirty=true; break;
                }
            }
        }
        render(); return ESP_OK;
    }
    if(phase==0) {
        for(unsigned i=0;i<history.count;) {
            if(history.entries[i].fixture) {
                memmove(&history.entries[i],&history.entries[i+1],(history.count-i-1)*sizeof(entry_t)); --history.count;
            } else ++i;
        }
        dirty=true; render(); return ESP_OK;
    }
    if(phase==1) return notification_post("__test_notice:music","验收测试：音乐处理中（不会调用模型）",10,NOTICE_WORKING);
    if(phase==2) return notification_post("__test_notice:music","验收测试：音乐完成（无真实歌曲）",10,NOTICE_DONE);
    if(phase==3) return notification_post("__test_notice:wallpaper","验收测试：失败提示（不会生成壁纸）",12,NOTICE_FAILED);
    if(phase==4) return notification_post("__test_notice:note","验收测试：笔记通知（不写真实笔记）",15,NOTICE_DONE);
    return ESP_ERR_INVALID_ARG;
}
