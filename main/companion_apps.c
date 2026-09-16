#include "companion_apps.h"
#include "notification_center.h"
#include "avatar_store.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_random.h"
#include "voice_input.h"
#include "service_config.h"

#define RESPONSE_BYTES (64*1024)
extern const uint8_t device_ca_start[] asm("_binary_device_ca_pem_start");
extern const uint8_t device_client_cert_start[] asm("_binary_device_client_pem_start");
extern const uint8_t device_client_key_start[] asm("_binary_device_client_key_start");
static const lv_font_t *body_font, *heading_font;
static lv_obj_t *agent_view, *agent_header, *agent_status, *agent_list;
static lv_obj_t *detail_panel, *detail_content, *notification;
static void (*go_home)(void);
static TaskHandle_t worker;
static SemaphoreHandle_t lock;
static cJSON *agent_data, *agent_pending;
static bool agent_error;
static bool display_awake=true;
static uint32_t notice_until;
static bool refresh_agents=true;
static lv_obj_t *notes_view, *notes_status, *notes_list, *notes_more, *notes_detail, *notes_content;
static lv_obj_t *notes_record_panel, *notes_record_status, *notes_finish, *notes_retry, *note_bars[9];
static cJSON *notes_data, *notes_pending;
static bool notes_error, refresh_notes=true, notes_recording;
static int notes_offset;
static char note_status[128]="说下想法，自动整理成笔记";
static char retry_id[33];
static char last_note_id[33], detail_note_id[33];
typedef struct { char id[33]; char text[2049]; } note_outbox_t;
static note_outbox_t outbox;
static bool outbox_ready;

static void render_notes(void);
static void create_notes(lv_obj_t *screen);
static void notes_work(void);

static const char *str(cJSON *o, const char *key)
{
    cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);
    return cJSON_IsString(v)?v->valuestring:"";
}
static int number(cJSON *o,const char *key) { cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key); return cJSON_IsNumber(v)?v->valueint:0; }
static bool flag(cJSON *o,const char *key) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,key)); }
static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,int width,const lv_font_t *font)
{
    lv_obj_t *o=lv_label_create(parent); lv_label_set_text(o,text);
    lv_obj_set_pos(o,x,y); lv_obj_set_width(o,width);
    lv_obj_set_style_text_font(o,font,0); lv_obj_set_style_text_color(o,lv_color_hex(0x202735),0);
    return o;
}
static lv_obj_t *button(lv_obj_t *parent,const char *text,int x,int y,int width,lv_event_cb_t cb,void *data)
{
    lv_obj_t *o=lv_button_create(parent); lv_obj_set_size(o,width,44); lv_obj_set_pos(o,x,y);
    lv_obj_set_style_bg_color(o,lv_color_hex(0xE3ECFC),0); lv_obj_set_style_radius(o,22,0);
    lv_obj_set_style_shadow_width(o,0,0); lv_obj_set_style_border_width(o,0,0);
    lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,data);
    lv_obj_t *t=label(o,text,0,0,width,body_font);
    lv_obj_set_style_text_align(t,LV_TEXT_ALIGN_CENTER,0); lv_obj_center(t);
    return o;
}
static lv_obj_t *page(lv_obj_t *parent)
{
    lv_obj_t *o=lv_obj_create(parent); lv_obj_remove_style_all(o); lv_obj_set_size(o,466,466);
    lv_obj_set_style_bg_opa(o,255,0); lv_obj_set_style_bg_color(o,lv_color_hex(0xF4F6FA),0);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE); return o;
}
static lv_obj_t *list(lv_obj_t *parent,int y,int height)
{
    lv_obj_t *o=lv_obj_create(parent); lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,55,y); lv_obj_set_size(o,356,height);
    lv_obj_set_scroll_dir(o,LV_DIR_VER); lv_obj_set_scrollbar_mode(o,LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(o,16,0); lv_obj_set_style_pad_row(o,10,0);
    lv_obj_set_flex_flow(o,LV_FLEX_FLOW_COLUMN); return o;
}
static void home_event(lv_event_t *e) { (void)e; go_home(); }
static void refresh_event(lv_event_t *e)
{
    (void)e; refresh_agents=true;
    lv_label_set_text(agent_status,"正在更新...");
    if(worker) xTaskNotifyGive(worker);
}
static void close_detail(lv_event_t *e) { (void)e; lv_obj_add_flag(detail_panel,LV_OBJ_FLAG_HIDDEN); }
static void open_detail(lv_event_t *e)
{
    int index=(int)(intptr_t)lv_event_get_user_data(e);
    cJSON *task=cJSON_GetArrayItem(cJSON_GetObjectItem(agent_data,"tasks"),index);
    if(!task) return;
    char *content=heap_caps_malloc(4800,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!content) return;
    snprintf(content,4800,"%s\n\n%s · %s\n%s · %s\n%s\n\n%s\n%s\n%s\n\n来源时间（UTC）\n%s%s",
             str(task,"title"),str(task,"agent"),str(task,"stateLabel"),str(task,"machine"),str(task,"project"),
             str(task,"progress"),str(task,"summary"),str(task,"reason"),str(task,"next"),str(task,"sourceAt"),
             flag(task,"stale")?"\n此来源已过期，请以电脑为准":"");
    lv_label_set_text(detail_content,content); free(content);
    lv_obj_scroll_to_y(lv_obj_get_parent(detail_content),0,LV_ANIM_OFF);
    lv_obj_remove_flag(detail_panel,LV_OBJ_FLAG_HIDDEN);
}
static void notify(const char *message)
{
    lv_label_set_text(notification,message);
    lv_obj_remove_flag(notification,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(notification); notice_until=lv_tick_get()+8000;
}
static void detect_agent_changes(cJSON *next)
{
    if(!agent_data) return; /* Initial history is a baseline, never a new alert. */
    cJSON *old_tasks=cJSON_GetObjectItem(agent_data,"tasks"), *task;
    cJSON_ArrayForEach(task,cJSON_GetObjectItem(next,"tasks")) {
        if(flag(task,"stale")) continue;
        const char *state=str(task,"state");
        if(strcmp(state,"done") && strcmp(state,"failed") && strcmp(state,"blocked")) continue;
        cJSON *old;
        cJSON_ArrayForEach(old,old_tasks) {
            if(!strcmp(str(old,"id"),str(task,"id")) && !flag(old,"stale") &&
               (!strcmp(str(old,"state"),"running") || !strcmp(str(old,"state"),"queued")) && strcmp(str(old,"state"),state)) {
                notify(!strcmp(state,"done")?"有任务已结束，请查看工作台":"任务需要关注，请查看工作台");
                char key[256],message[192];
                snprintf(key,sizeof(key),"agent:%s:%s",str(task,"id"),str(task,"sourceAt"));
                const char *title=str(task,"title"); size_t count=strlen(title);
                if(count>140) { count=140; while(count && ((unsigned char)title[count]&0xC0)==0x80) --count; }
                snprintf(message,sizeof(message),"%s：%.*s",!strcmp(state,"done")?"任务已结束":"任务需要关注",(int)count,title);
                notification_post(key,message,14,!strcmp(state,"done")?NOTICE_DONE:NOTICE_FAILED);
                break;
            }
        }
    }
}
static void render_agents(void)
{
    cJSON *counts=cJSON_GetObjectItem(agent_data,"counts");
    if(!flag(agent_data,"ready")) {
        lv_label_set_text(agent_header,"等待电脑同步");
        lv_label_set_text(agent_status,"请保持数字人助手运行");
    } else {
        lv_label_set_text_fmt(agent_header,"%d 运行   %d 排队   %d 异常",number(counts,"running"),number(counts,"queued"),number(counts,"attention"));
        lv_label_set_text_fmt(agent_status,"%d 台机器%s%s",number(agent_data,"machines"),
            number(agent_data,"staleMachines")?" · 有来源已过期":" · 每分钟同步",
            flag(agent_data,"incomplete")?" · 部分任务":"");
    }
    int scroll=lv_obj_get_scroll_y(agent_list);
    lv_obj_clean(agent_list);
    cJSON *task; int i=0;
    cJSON_ArrayForEach(task,cJSON_GetObjectItem(agent_data,"tasks")) {
        lv_obj_t *card=lv_button_create(agent_list); lv_obj_set_size(card,350,119);
        lv_obj_set_style_radius(card,22,0); lv_obj_set_style_pad_all(card,12,0);
        lv_obj_set_style_bg_color(card,lv_color_white(),0); lv_obj_set_style_shadow_width(card,0,0);
        lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(card,open_detail,LV_EVENT_CLICKED,(void *)(intptr_t)i++);
        lv_obj_t *title=label(card,str(task,"title"),0,0,326,body_font);
        lv_obj_set_height(title,52); lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
        char line[160]; snprintf(line,sizeof(line),"%s · %s",str(task,"agent"),str(task,"stateLabel"));
        lv_obj_t *status=label(card,line,0,53,326,body_font);
        lv_obj_set_style_text_color(status,lv_color_hex(flag(task,"stale")?0x687587:0x256CC3),0);
        snprintf(line,sizeof(line),"%s  %s",str(task,"machine"),str(task,"progress"));
        lv_obj_t *source=label(card,line,0,78,326,body_font);
        lv_label_set_long_mode(source,LV_LABEL_LONG_DOT); lv_obj_set_height(source,24);
    }
    if(!i) label(agent_list,flag(agent_data,"ready")?"当前任务板没有任务":"尚未收到任务数据",0,0,330,body_font);
    lv_obj_update_layout(agent_list); lv_obj_scroll_to_y(agent_list,scroll,LV_ANIM_OFF);
}
static cJSON *request(const char *path,const char *body)
{
    char url[256]; snprintf(url,sizeof(url),CLOCK_API_BASE "%s",path);
    esp_http_client_config_t cfg={.url=url,.timeout_ms=15000,.buffer_size=2048,
        .cert_pem=(const char *)device_ca_start,.client_cert_pem=(const char *)device_client_cert_start,
        .client_key_pem=(const char *)device_client_key_start};
    esp_http_client_handle_t client=esp_http_client_init(&cfg);
    if(!client) return NULL;
    if(CLOCK_WALLPAPER_TOKEN[0]) { char auth[192]; snprintf(auth,sizeof(auth),"Bearer %s",CLOCK_WALLPAPER_TOKEN); esp_http_client_set_header(client,"Authorization",auth); }
    if(body) { esp_http_client_set_method(client,HTTP_METHOD_POST); esp_http_client_set_header(client,"Content-Type","application/json"); }
    esp_err_t r=esp_http_client_open(client,body?strlen(body):0);
    if(body) for(size_t n=0;r==ESP_OK && n<strlen(body);) {
        int written=esp_http_client_write(client,body+n,strlen(body)-n);
        if(written<=0) r=ESP_FAIL; else n+=written;
    }
    char *buffer=NULL; cJSON *result=NULL;
    if(r==ESP_OK) {
        int64_t length=esp_http_client_fetch_headers(client);
        int status=esp_http_client_get_status_code(client);
        if(length<RESPONSE_BYTES && status>=200 && status<300) {
            buffer=heap_caps_calloc(1,RESPONSE_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(buffer) {
                int count=esp_http_client_read_response(client,buffer,RESPONSE_BYTES-1);
                if(count>0 && esp_http_client_is_complete_data_received(client)) result=cJSON_Parse(buffer);
            }
        }
    }
    free(buffer); esp_http_client_close(client); esp_http_client_cleanup(client); return result;
}
static void companion_task(void *ctx)
{
    (void)ctx; int64_t next=0, next_avatar=0;
    for(;;) {
        if(refresh_agents || esp_timer_get_time()>=next) {
            refresh_agents=false; cJSON *data=request("/v1/agents",NULL);
            xSemaphoreTake(lock,portMAX_DELAY);
            if(data) { cJSON_Delete(agent_pending); agent_pending=data; }
            agent_error=data==NULL;
            xSemaphoreGive(lock); next=esp_timer_get_time()+60000000;
        }
        notes_work();
        if(esp_timer_get_time()>=next_avatar) {
            avatar_store_sync(); next_avatar=esp_timer_get_time()+60000000;
        }
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(1000));
    }
}
void companion_apps_create(lv_obj_t *screen,const lv_font_t *font,const lv_font_t *title_font,void (*home)(void))
{
    body_font=font; heading_font=title_font; go_home=home;
    agent_view=page(screen);
    lv_obj_t *title=label(agent_view,"Agent 工作台",83,35,300,heading_font); lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_CENTER,0);
    agent_header=label(agent_view,"正在读取任务板",53,81,360,body_font); lv_obj_set_style_text_align(agent_header,LV_TEXT_ALIGN_CENTER,0);
    agent_status=label(agent_view,"每分钟同步",43,110,380,body_font); lv_obj_set_style_text_align(agent_status,LV_TEXT_ALIGN_CENTER,0);
    agent_list=list(agent_view,146,235);
    button(agent_view,"刷新",94,398,130,refresh_event,NULL);
    button(agent_view,"桌面",242,398,130,home_event,NULL);
    detail_panel=page(agent_view);
    lv_obj_t *content=list(detail_panel,90,288);
    detail_content=label(content,"",0,0,346,body_font);
    button(detail_panel,"返回",158,398,150,close_detail,NULL);
    lv_obj_add_flag(detail_panel,LV_OBJ_FLAG_HIDDEN);
    create_notes(screen);
    notification=label(screen,"",83,348,300,body_font);
    lv_obj_set_style_bg_color(notification,lv_color_hex(0xE3ECFC),0); lv_obj_set_style_bg_opa(notification,255,0);
    lv_obj_set_style_radius(notification,16,0); lv_obj_set_style_pad_all(notification,12,0);
    lv_obj_set_style_text_align(notification,LV_TEXT_ALIGN_CENTER,0); lv_obj_add_flag(notification,LV_OBJ_FLAG_HIDDEN);
}
lv_obj_t *companion_apps_view(unsigned app) { return app==1?notes_view:agent_view; }
void companion_apps_activate(unsigned app)
{
    if(app==1) { refresh_notes=true; lv_obj_add_flag(notes_detail,LV_OBJ_FLAG_HIDDEN); }
    else { refresh_agents=true; lv_obj_add_flag(detail_panel,LV_OBJ_FLAG_HIDDEN); }
    if(worker) xTaskNotifyGive(worker);
}
void companion_apps_tick(bool awake)
{
    display_awake=awake;
    if(!lock) return;
    xSemaphoreTake(lock,portMAX_DELAY);
    cJSON *next=agent_pending; agent_pending=NULL; bool error=agent_error; agent_error=false;
    cJSON *next_notes=notes_pending; notes_pending=NULL; bool note_error=notes_error; notes_error=false;
    char message[128]; strlcpy(message,note_status,sizeof(message));
    xSemaphoreGive(lock);
    if(next) { detect_agent_changes(next); cJSON_Delete(agent_data); agent_data=next; render_agents(); }
    if(error) lv_label_set_text(agent_status,agent_data?"更新失败 · 显示上次快照":"连接失败，请点击刷新");
    if(next_notes) {
        cJSON *note;
        cJSON_ArrayForEach(note,cJSON_GetObjectItem(next_notes,"notes")) {
            if(!notes_recording && !strcmp(str(note,"id"),last_note_id) && strcmp(str(note,"state"),"pending")) {
                xSemaphoreTake(lock,portMAX_DELAY);
                strlcpy(note_status,!strcmp(str(note,"state"),"ready")?"笔记已整理，可开始新笔记":"原文已保存，整理失败可稍后重试",sizeof(note_status));
                xSemaphoreGive(lock);
            }
            bool completed=!notes_recording && last_note_id[0] && !strcmp(str(note,"id"),last_note_id) && strcmp(str(note,"state"),"pending");
            cJSON *old;
            cJSON_ArrayForEach(old,cJSON_GetObjectItem(notes_data,"notes"))
                if(!strcmp(str(old,"id"),str(note,"id")) && !strcmp(str(old,"state"),"pending") && strcmp(str(note,"state"),"pending"))
                    completed=true;
            if(completed) {
                char key[64]; snprintf(key,sizeof(key),"note:%s",str(note,"id"));
                bool ready=!strcmp(str(note,"state"),"ready");
                notification_post(key,ready?"语音笔记已整理，点此查看":"笔记原文已保存，整理失败可重试",15,ready?NOTICE_DONE:NOTICE_FAILED);
            }
        }
        cJSON_Delete(notes_data); notes_data=next_notes; render_notes();
    }
    if(note_error) lv_label_set_text(notes_status,notes_data?"连接失败 · 保留上次记录":"连接失败，请点击最新重试");
    if(strcmp(lv_label_get_text(notes_record_status),message)) lv_label_set_text(notes_record_status,message);
    if(notes_recording && voice_input_is_recording()) lv_obj_remove_state(notes_finish,LV_STATE_DISABLED);
    else lv_obj_add_state(notes_finish,LV_STATE_DISABLED);
    if(!awake && notice_until) notice_until=lv_tick_get()+8000;
    if(notice_until && (int32_t)(lv_tick_get()-notice_until)>=0) { notice_until=0; lv_obj_add_flag(notification,LV_OBJ_FLAG_HIDDEN); }
}
esp_err_t companion_apps_start(void)
{
    lock=xSemaphoreCreateMutex(); if(!lock) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs;
    if(nvs_open("voice_notes",NVS_READONLY,&nvs)==ESP_OK) {
        size_t bytes=sizeof(outbox);
        if(nvs_get_blob(nvs,"outbox",&outbox,&bytes)==ESP_OK && bytes==sizeof(outbox) &&
           outbox.id[32]==0 && outbox.text[2048]==0 && strlen(outbox.id)==32) {
            outbox_ready=true; strlcpy(note_status,"正在续传上次笔记",sizeof(note_status));
        }
        nvs_close(nvs);
    }
    return xTaskCreateWithCaps(companion_task,"companion",8192,NULL,3,&worker,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
void companion_apps_debug(cJSON *root)
{
    notification_center_debug(root);
    cJSON_AddStringToObject(root,"agent_status",lv_label_get_text(agent_status));
    cJSON_AddNumberToObject(root,"agent_rows",cJSON_GetArraySize(cJSON_GetObjectItem(agent_data,"tasks")));
    cJSON_AddNumberToObject(root,"notes_rows",cJSON_GetArraySize(cJSON_GetObjectItem(notes_data,"notes")));
    cJSON_AddStringToObject(root,"notes_status",lv_label_get_text(notes_status));
    cJSON_AddStringToObject(root,"note_capture_status",lv_label_get_text(notes_record_status));
    cJSON_AddBoolToObject(root,"notes_recording",notes_recording);
}

static esp_err_t persist_outbox(const note_outbox_t *value)
{
    nvs_handle_t nvs; esp_err_t r=nvs_open("voice_notes",NVS_READWRITE,&nvs);
    if(r!=ESP_OK) return r;
    r=value?nvs_set_blob(nvs,"outbox",value,sizeof(*value)):nvs_erase_key(nvs,"outbox");
    if(r==ESP_ERR_NVS_NOT_FOUND) r=ESP_OK;
    if(r==ESP_OK) r=nvs_commit(nvs);
    nvs_close(nvs); return r;
}
static void notes_transcript(const char *text,void *context)
{
    (void)context;
    xSemaphoreTake(lock,portMAX_DELAY);
    notes_recording=false;
    if(!text||!text[0]||strlen(text)>2048) {
        strlcpy(note_status,"没有识别到内容，请重新录音",sizeof(note_status));
    } else {
        for(int i=0;i<4;++i) snprintf(outbox.id+i*8,9,"%08lx",(unsigned long)esp_random());
        strlcpy(last_note_id,outbox.id,sizeof(last_note_id));
        strlcpy(outbox.text,text,sizeof(outbox.text));
        esp_err_t saved=persist_outbox(&outbox);
        char key[64]; snprintf(key,sizeof(key),"note:%s",outbox.id);
        notification_post(key,"语音笔记正在上传和整理",15,NOTICE_WORKING);
        outbox_ready=true;
        strlcpy(note_status,saved==ESP_OK?"已暂存，正在发送，可离开应用":"暂存失败，请保持开机等待上传",sizeof(note_status));
    }
    xSemaphoreGive(lock); if(worker) xTaskNotifyGive(worker);
}

esp_err_t companion_apps_test_note(void)
{
    if(!lock || notes_recording || outbox_ready || voice_input_is_recording()) return ESP_ERR_INVALID_STATE;
    notes_transcript("圆屏验收测试：明天下午检查配网页面，不要发布。这是自动化测试笔记，不是真实待办。",NULL);
    return ESP_OK;
}
static void record_note(lv_event_t *e)
{
    (void)e;
    lv_obj_remove_flag(notes_record_panel,LV_OBJ_FLAG_HIDDEN);
    xSemaphoreTake(lock,portMAX_DELAY);
    if(!outbox_ready && !notes_recording) {
        esp_err_t r=voice_input_start_with_callback(notes_transcript,NULL);
        if(r==ESP_OK) {
            notes_recording=true; strlcpy(note_status,"正在录音，最长 20 秒",sizeof(note_status));
            lv_obj_remove_state(notes_finish,LV_STATE_DISABLED);
        }
        else strlcpy(note_status,"麦克风忙，请稍后再试",sizeof(note_status));
    }
    lv_label_set_text(notes_record_status,note_status);
    xSemaphoreGive(lock);
}
static void finish_note(lv_event_t *e)
{
    (void)e;
    if(notes_recording && voice_input_finish()==ESP_OK) {
        xSemaphoreTake(lock,portMAX_DELAY); strlcpy(note_status,"正在识别，可先返回桌面",sizeof(note_status)); xSemaphoreGive(lock);
    }
}
static void close_record(lv_event_t *e) { (void)e; lv_obj_add_flag(notes_record_panel,LV_OBJ_FLAG_HIDDEN); }
static void close_note(lv_event_t *e) { (void)e; lv_obj_add_flag(notes_detail,LV_OBJ_FLAG_HIDDEN); }
static void page_notes(lv_event_t *e)
{
    notes_offset=lv_event_get_user_data(e)?number(notes_data,"nextOffset"):0;
    refresh_notes=true; lv_label_set_text(notes_status,"正在读取..."); if(worker) xTaskNotifyGive(worker);
}
static void retry_note(lv_event_t *e)
{
    (void)e;
    const char *id=lv_obj_get_user_data(notes_retry);
    if(!id) return;
    xSemaphoreTake(lock,portMAX_DELAY); strlcpy(retry_id,id,sizeof(retry_id)); xSemaphoreGive(lock);
    lv_obj_add_state(notes_retry,LV_STATE_DISABLED); if(worker) xTaskNotifyGive(worker);
}
static void open_note(lv_event_t *e)
{
    cJSON *note=cJSON_GetArrayItem(cJSON_GetObjectItem(notes_data,"notes"),(int)(intptr_t)lv_event_get_user_data(e));
    if(!note) return;
    int scroll=!strcmp(detail_note_id,str(note,"id")) && !lv_obj_has_flag(notes_detail,LV_OBJ_FLAG_HIDDEN)?lv_obj_get_scroll_y(lv_obj_get_parent(notes_content)):0;
    strlcpy(detail_note_id,str(note,"id"),sizeof(detail_note_id));
    lv_obj_set_user_data(notes_retry,detail_note_id);
    char *body=heap_caps_calloc(1,7000,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!body) return;
    snprintf(body,7000,"%s\n\n%s\n%s",str(note,"title"),str(note,"summary"),str(note,"error"));
    cJSON *todo;
    if(cJSON_GetArraySize(cJSON_GetObjectItem(note,"todos"))) strlcat(body,"\n\n待办建议（未设置提醒）",7000);
    cJSON_ArrayForEach(todo,cJSON_GetObjectItem(note,"todos")) if(cJSON_IsString(todo)) { strlcat(body,"\n- ",7000); strlcat(body,todo->valuestring,7000); }
    strlcat(body,"\n\n识别原文\n",7000); strlcat(body,str(note,"rawText"),7000);
    strlcat(body,"\n\n保存时间（UTC）\n",7000); strlcat(body,str(note,"createdAt"),7000);
    lv_label_set_text(notes_content,body); free(body);
    lv_obj_scroll_to_y(lv_obj_get_parent(notes_content),scroll,LV_ANIM_OFF);
    if(!strcmp(str(note,"state"),"saved")) lv_obj_remove_state(notes_retry,LV_STATE_DISABLED);
    else lv_obj_add_state(notes_retry,LV_STATE_DISABLED);
    lv_obj_remove_flag(notes_detail,LV_OBJ_FLAG_HIDDEN);
}
static void render_notes(void)
{
    lv_label_set_text_fmt(notes_status,"已保存 %d 条 · 第 %d 页",number(notes_data,"total"),number(notes_data,"offset")/12+1);
    if(flag(notes_data,"hasMore")) lv_obj_remove_state(notes_more,LV_STATE_DISABLED);
    else lv_obj_add_state(notes_more,LV_STATE_DISABLED);
    int scroll=lv_obj_get_scroll_y(notes_list);
    lv_obj_clean(notes_list); int i=0; cJSON *note;
    cJSON_ArrayForEach(note,cJSON_GetObjectItem(notes_data,"notes")) {
        lv_obj_t *card=lv_button_create(notes_list); lv_obj_set_size(card,350,106);
        lv_obj_set_style_pad_all(card,12,0); lv_obj_set_style_radius(card,22,0);
        lv_obj_set_style_bg_color(card,lv_color_white(),0); lv_obj_set_style_shadow_width(card,0,0);
        lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(card,open_note,LV_EVENT_CLICKED,(void *)(intptr_t)i++);
        lv_obj_t *title=label(card,str(note,"title"),0,0,326,body_font); lv_obj_set_height(title,52); lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
        const char *state=str(note,"state");
        label(card,!strcmp(state,"ready")?"已整理 · 点击查看":!strcmp(state,"pending")?"原文已保存 · 整理中":"原文已保存 · 可重试",0,55,326,body_font);
        if(!lv_obj_has_flag(notes_detail,LV_OBJ_FLAG_HIDDEN) && !strcmp(str(note,"id"),detail_note_id))
            lv_obj_send_event(card,LV_EVENT_CLICKED,NULL);
    }
    if(!i) label(notes_list,"还没有笔记\n\n点击上方，说下你的想法。\n识别和整理需要联网。",0,0,330,body_font);
    lv_obj_update_layout(notes_list); lv_obj_scroll_to_y(notes_list,scroll,LV_ANIM_OFF);
}
static void notes_wave(lv_timer_t *timer)
{
    (void)timer;
    if(!display_awake||lv_obj_has_flag(notes_view,LV_OBJ_FLAG_HIDDEN)||lv_obj_has_flag(notes_record_panel,LV_OBJ_FLAG_HIDDEN)) return;
    unsigned level=notes_recording&&voice_input_is_recording()?voice_input_level():0;
    static unsigned frame; ++frame;
    for(int i=0;i<9;++i) { int h=8+level*(35+((i*29+frame*13)%65))/180; lv_obj_set_height(note_bars[i],h); lv_obj_set_y(note_bars[i],220-h/2); }
}
static void create_notes(lv_obj_t *screen)
{
    notes_view=page(screen);
    lv_obj_t *title=label(notes_view,"语音随手记",83,35,300,heading_font); lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_CENTER,0);
    notes_status=label(notes_view,"正在读取...",65,86,240,body_font);
    button(notes_view,"最新",307,75,90,page_notes,NULL);
    button(notes_view,"说一条笔记",128,121,210,record_note,NULL);
    notes_list=list(notes_view,179,203);
    notes_more=button(notes_view,"更早",112,398,112,page_notes,(void *)1);
    button(notes_view,"桌面",242,398,112,home_event,NULL);
    notes_detail=page(notes_view);
    lv_obj_t *content=list(notes_detail,90,288); notes_content=label(content,"",0,0,346,body_font);
    button(notes_detail,"返回",108,398,120,close_note,NULL);
    notes_retry=button(notes_detail,"重试整理",238,398,120,retry_note,NULL);
    lv_obj_add_flag(notes_detail,LV_OBJ_FLAG_HIDDEN);
    notes_record_panel=page(notes_view);
    title=label(notes_record_panel,"说下你的想法",73,58,320,heading_font); lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_CENTER,0);
    notes_record_status=label(notes_record_panel,"点击开始录音",73,112,320,body_font);
    lv_obj_set_style_text_align(notes_record_status,LV_TEXT_ALIGN_CENTER,0);
    for(int i=0;i<9;++i) { note_bars[i]=lv_obj_create(notes_record_panel); lv_obj_remove_style_all(note_bars[i]); lv_obj_set_size(note_bars[i],12,8); lv_obj_set_pos(note_bars[i],147+i*20,216); lv_obj_set_style_bg_color(note_bars[i],lv_color_hex(0xE6A12B),0); lv_obj_set_style_bg_opa(note_bars[i],255,0); lv_obj_set_style_radius(note_bars[i],6,0); }
    button(notes_record_panel,"开始",111,282,112,record_note,NULL);
    notes_finish=button(notes_record_panel,"完成",243,282,112,finish_note,NULL);
    label(notes_record_panel,"完成后自动保存，可离开应用",78,348,320,body_font);
    button(notes_record_panel,"返回",158,398,150,close_record,NULL);
    lv_obj_add_flag(notes_record_panel,LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(notes_wave,120,NULL);
}
static void notes_work(void)
{
    static int64_t next_poll, next_send;
    char retry[33]; note_outbox_t *item=heap_caps_calloc(1,sizeof(*item),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!item) return;
    xSemaphoreTake(lock,portMAX_DELAY);
    bool send=outbox_ready && esp_timer_get_time()>=next_send;
    if(send) *item=outbox;
    retry[0]=0;
    if(!send) { strlcpy(retry,retry_id,sizeof(retry)); retry_id[0]=0; }
    xSemaphoreGive(lock);
    if(send || retry[0]) {
        cJSON *body=cJSON_CreateObject();
        cJSON_AddStringToObject(body,"id",send?item->id:retry);
        if(send) cJSON_AddStringToObject(body,"text",item->text); else cJSON_AddBoolToObject(body,"retry",true);
        char *encoded=cJSON_PrintUnformatted(body); cJSON_Delete(body);
        cJSON *result=encoded?request("/v1/notes",encoded):NULL; free(encoded);
        bool acknowledged=result && !strcmp(str(result,"id"),send?item->id:retry);
        xSemaphoreTake(lock,portMAX_DELAY);
        if(send) {
            if(acknowledged) { persist_outbox(NULL); outbox_ready=false; memset(&outbox,0,sizeof(outbox)); strlcpy(note_status,"原文已保存，正在后台整理",sizeof(note_status)); }
            else strlcpy(note_status,"上传未确认，已保留，稍后自动重试",sizeof(note_status));
        } else if(!acknowledged) notes_error=true;
        xSemaphoreGive(lock); cJSON_Delete(result);
        refresh_notes=true; next_send=esp_timer_get_time()+15000000;
        if(acknowledged) notes_offset=0;
    }
    free(item);
    if(refresh_notes || esp_timer_get_time()>=next_poll) {
        refresh_notes=false; int requested_offset=notes_offset;
        char path[80]; snprintf(path,sizeof(path),"/v1/notes?offset=%d",requested_offset);
        cJSON *data=request(path,NULL);
        xSemaphoreTake(lock,portMAX_DELAY);
        if(data && requested_offset==notes_offset) { cJSON_Delete(notes_pending); notes_pending=data; }
        else cJSON_Delete(data);
        if(!data) notes_error=true;
        xSemaphoreGive(lock); next_poll=esp_timer_get_time()+15000000;
    }
}
