#include "library_app.h"
#include "device_api.h"
#include "music_input.h"
#include "wallpaper_input.h"
#include "story_app.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

typedef struct {char id[33],kind[16],title[241],date[32];bool favorite,selected,thumbnail;} item_t;
typedef struct {item_t items[6];int count,offset;bool more,trash,offline,music_only;} page_t;
typedef struct {int type,offset;bool trash,value,music_only;char id[33],action[16];} request_t;
static lv_obj_t *view,*heading,*status,*title_label,*date_label,*preview,*prev,*next,*favorite,*use,*remove_button,*confirm,*kind_label;
static const lv_font_t *body_font,*title_font;
static void (*go_home)(void);
static void (*go_story)(void);
static lv_timer_t *timer;
static SemaphoreHandle_t mutex;
static page_t *page,*incoming;
static uint8_t *thumbnail,*incoming_thumb;
static lv_image_dsc_t image;
static char thumb_id[33],incoming_id[33],message[100]="正在连接作品库";
static bool active,busy,finished,success,deleted_confirmation;
static bool music_was_playing;
static bool next_music_only,music_only,reload_on_finish;
static int item_index,last_type;
static request_t request;

static const char *str(cJSON *o,const char *key) {cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:"";}
static bool flag(cJSON *o,const char *key) {return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,key));}
static item_t *current(void) {return page&&item_index>=0&&item_index<page->count?&page->items[item_index]:NULL;}
static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,int w,const lv_font_t *font)
{
    lv_obj_t *o=lv_label_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);lv_label_set_text(o,text);
    lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(0x263D58),0);lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);return o;
}
static lv_obj_t *button(const char *text,int x,int y,int w,lv_event_cb_t callback,int value)
{
    lv_obj_t *o=lv_button_create(view);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,44);lv_obj_set_style_radius(o,22,0);
    lv_obj_set_style_bg_color(o,lv_color_hex(0xE7EEFC),0);lv_obj_set_style_shadow_width(o,0,0);lv_obj_set_style_pad_all(o,0,0);
    lv_obj_add_event_cb(o,callback,LV_EVENT_CLICKED,(void *)(intptr_t)value);
    lv_obj_t *t=label(o,text,0,0,w,body_font);lv_obj_center(t);return o;
}
static void hidden(lv_obj_t *o,bool hide) {if(hide)lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN);else lv_obj_remove_flag(o,LV_OBJ_FLAG_HIDDEN);}
static void set_button(lv_obj_t *o,const char *text) {lv_label_set_text(lv_obj_get_child(o,0),text);}
static void parse_page(page_t *out,const char *json)
{
    cJSON *root=cJSON_Parse(json);if(!root)return;
    out->more=flag(root,"more");out->trash=flag(root,"trash");cJSON *offset=cJSON_GetObjectItem(root,"offset");out->offset=cJSON_IsNumber(offset)?offset->valueint:0;
    cJSON *row;cJSON_ArrayForEach(row,cJSON_GetObjectItem(root,"items")) {
        if(out->count>=6)break;
        item_t *i=&out->items[out->count];
        const char *id=str(row,"id");if(strlen(id)!=32||strspn(id,"0123456789abcdef")!=32)continue;
        strlcpy(i->id,id,sizeof(i->id));strlcpy(i->kind,str(row,"kind"),sizeof(i->kind));strlcpy(i->title,str(row,"title"),sizeof(i->title));
        strlcpy(i->date,str(row,"dateLabel"),sizeof(i->date));i->favorite=flag(row,"favorite");i->selected=flag(row,"selected");i->thumbnail=flag(row,"thumbnail");out->count++;
    }
    cJSON_Delete(root);
}
static void worker(void *unused)
{
    (void)unused;request_t r=request;char path[180],body[180];size_t length=0;
    size_t capacity=r.type==2?32769:8192;uint8_t *data=heap_caps_calloc(1,capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    bool ok=false;page_t *new_page=NULL;
    if(data) {
        if(r.type==1)snprintf(path,sizeof(path),"/v1/library?offset=%d&trash=%d&kind=%s",r.offset,r.trash,r.music_only?"music":"");
        else if(r.type==2)snprintf(path,sizeof(path),"/v1/library?id=%s&part=thumb",r.id);
        else strlcpy(path,"/v1/library",sizeof(path));
        snprintf(body,sizeof(body),"{\"id\":\"%s\",\"action\":\"%s\",\"value\":%s}",r.id,r.action,r.value?"true":"false");
        ok=device_api(path,r.type==3?body:NULL,data,capacity,&length)==ESP_OK;
        if(r.type==1) {
            const char *cache=r.music_only?"/wallpaper/library-music.json":"/wallpaper/library.json";
            if(!ok) {FILE *f=fopen(cache,"rb");if(f) {length=fread(data,1,capacity-1,f);data[length]=0;fclose(f);}else length=0;}
            new_page=heap_caps_calloc(1,sizeof(page_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(new_page) {parse_page(new_page,(char *)data);new_page->offline=!ok;new_page->music_only=r.music_only;}
            if(ok) {FILE *f=fopen(cache,"wb");if(f){fwrite(data,1,length,f);fclose(f);}}
        } else if(r.type==2)ok=ok&&length==32768;
        else if(ok&&!strcmp(r.action,"select"))wallpaper_input_request_reload();
    }
    xSemaphoreTake(mutex,portMAX_DELAY);
    incoming=new_page;if(r.type==2&&ok) {incoming_thumb=data;data=NULL;strlcpy(incoming_id,r.id,sizeof(incoming_id));}
    success=ok;last_type=r.type;finished=true;
    xSemaphoreGive(mutex);heap_caps_free(data);vTaskDeleteWithCaps(NULL);
}
static bool launch(request_t r)
{
    if(busy)return false;
    busy=true;request=r;
    if(xTaskCreateWithCaps(worker,"library",8192,NULL,3,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){busy=false;strlcpy(message,"内存忙，请稍后刷新",sizeof(message));return false;}
    return true;
}
static void load_page(int offset,bool trash) {deleted_confirmation=false;launch((request_t){.type=1,.offset=offset,.trash=trash,.music_only=music_only});}
static void render(void)
{
    if(!active)return;
    item_t *item=current();lv_label_set_text(heading,page&&page->trash?"回收站":music_only?"我的音乐":"作品相册");
    lv_label_set_text(status,busy?"正在同步…":message);
    lv_label_set_text(title_label,item?item->title:"还没有作品");
    if(item)lv_label_set_text_fmt(date_label,"%s · 第 %d 项",item->date,page->offset+item_index+1);
    else lv_label_set_text(date_label,"生成的壁纸和音乐会保存在这里");
    bool has_thumb=item&&item->thumbnail&&thumbnail&&!strcmp(item->id,thumb_id);
    hidden(preview,!has_thumb);hidden(kind_label,has_thumb);
    lv_label_set_text(kind_label,item?(!strcmp(item->kind,"music")?LV_SYMBOL_AUDIO:!strcmp(item->kind,"story")?LV_SYMBOL_LIST:LV_SYMBOL_IMAGE):LV_SYMBOL_IMAGE);
    if(has_thumb) {image.data=thumbnail;lv_image_set_src(preview,&image);}
    hidden(favorite,!item||page->trash);hidden(use,!item);hidden(remove_button,!item||page->trash||item->selected);
    if(item) {set_button(favorite,item->favorite?"已收藏":"收藏");set_button(use,page->trash?"恢复":!strcmp(item->kind,"music")?(music_input_state().playing?"停止":"播放"):!strcmp(item->kind,"story")?"阅读":item->selected?"已应用":"应用");}
    hidden(confirm,!deleted_confirmation);
    hidden(prev,!page||(!item_index&&!page->offset));hidden(next,!page||(item_index+1>=page->count&&!page->more));
}
static void request_thumb(void)
{
    item_t *item=current();if(!item||!item->thumbnail||page->trash||!strcmp(item->id,thumb_id)||busy)return;
    request_t r={.type=2};strlcpy(r.id,item->id,sizeof(r.id));launch(r);
}
static void action(lv_event_t *event)
{
    int a=(int)(intptr_t)lv_event_get_user_data(event);item_t *item=current();
    if(a==0) {go_home();return;}
    if(busy)return;
    if(a==1)load_page(page?page->offset:0,page&&page->trash);
    else if(a==2)load_page(0,!(page&&page->trash));
    else if(a==3||a==4) {
        deleted_confirmation=false;
        if(a==3) {if(item_index)item_index--;else if(page&&page->offset)load_page(page->offset-6,page->trash);}
        else {if(page&&item_index+1<page->count)item_index++;else if(page&&page->more)load_page(page->offset+6,page->trash);}
        request_thumb();
    } else if(a==7)deleted_confirmation=!deleted_confirmation;
    else if(item) {
        request_t r={.type=3};strlcpy(r.id,item->id,sizeof(r.id));
        if(a==5) {strlcpy(r.action,"favorite",sizeof(r.action));r.value=!item->favorite;}
        else if(a==8)strlcpy(r.action,"trash",sizeof(r.action));
        else if(a==6) {
            if(page->trash)strlcpy(r.action,"restore",sizeof(r.action));
            else if(!strcmp(item->kind,"music")) {
                if(music_input_state().playing){music_input_stop();strlcpy(message,"正在停止播放",sizeof(message));}
                else strlcpy(message,music_input_play_artwork(item->id)==ESP_OK?"正在播放，可切换应用":"声音正忙，请稍后重试",sizeof(message));
                render();return;
            } else if(!strcmp(item->kind,"story")) {
                if(story_app_request(item->id)){go_story();return;}
                strlcpy(message,"故事正在准备，请稍后打开",sizeof(message));render();return;
            } else strlcpy(r.action,"select",sizeof(r.action));
        }
        if(r.action[0])launch(r);
    }
    render();
}
static void tick(lv_timer_t *t)
{
    (void)t;xSemaphoreTake(mutex,portMAX_DELAY);
    bool done=finished,ok=success;int type=last_type;
    if(done) {
        finished=false;busy=false;
        if(incoming){
            if(incoming->music_only==music_only){heap_caps_free(page);page=incoming;item_index=0;}
            else {heap_caps_free(incoming);reload_on_finish=true;}
            incoming=NULL;
        }
        if(incoming_thumb){heap_caps_free(thumbnail);thumbnail=incoming_thumb;incoming_thumb=NULL;strlcpy(thumb_id,incoming_id,sizeof(thumb_id));}
    }
    xSemaphoreGive(mutex);
    if(done&&reload_on_finish){reload_on_finish=false;load_page(0,false);render();return;}
    if(done) {
        strlcpy(message,ok?(type==3?"操作已保存":"作品已同步"):(type==1?"离线 · 显示上次作品列表":"操作失败，请检查网络后重试"),sizeof(message));
        deleted_confirmation=false;
        if(type==3&&ok)load_page(page?page->offset:0,page&&page->trash);
        else if(type==1&&ok&&active)request_thumb();
        if(!active){heap_caps_free(thumbnail);thumbnail=NULL;thumb_id[0]=0;}
        render();
    }
    bool playing=music_input_state().playing;
    if(playing!=music_was_playing){
        if(!playing&&active&&current()&&!strcmp(current()->kind,"music"))strlcpy(message,strstr(music_input_status(),"FAILED")?"播放失败，请稍后重试":"播放已结束，可再次播放",sizeof(message));
        music_was_playing=playing;render();
    }
}
void library_app_create(lv_obj_t *screen,const lv_font_t *body,const lv_font_t *title,void (*home)(void),void (*open_story)(void))
{
    body_font=body;title_font=title;go_home=home;go_story=open_story;mutex=xSemaphoreCreateMutex();
    view=lv_obj_create(screen);lv_obj_remove_style_all(view);lv_obj_set_size(view,466,466);lv_obj_remove_flag(view,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(view,lv_color_white(),0);lv_obj_set_style_bg_opa(view,255,0);
    image=(lv_image_dsc_t){.header.magic=LV_IMAGE_HEADER_MAGIC,.header.cf=LV_COLOR_FORMAT_RGB565,.header.w=128,.header.h=128,.header.stride=256,.data_size=32768};
    timer=lv_timer_create(tick,150,NULL);
}
lv_obj_t *library_app_view(void) {return view;}
void library_app_open_music(void) {next_music_only=true;}
void library_app_set_active(bool enabled)
{
    if(active==enabled)return;
    active=enabled;
    if(!enabled){lv_obj_clean(view);heap_caps_free(thumbnail);thumbnail=NULL;thumb_id[0]=0;return;}
    music_only=next_music_only;next_music_only=false;
    if(page&&page->music_only!=music_only){heap_caps_free(page);page=NULL;item_index=0;}
    if(busy)reload_on_finish=true;
    heading=label(view,"作品相册",73,34,320,title_font);status=label(view,"",55,73,356,body_font);
    title_label=label(view,"",65,99,336,body_font);lv_label_set_long_mode(title_label,LV_LABEL_LONG_DOT);lv_obj_set_height(title_label,50);
    preview=lv_image_create(view);lv_obj_set_pos(preview,169,150);kind_label=label(view,LV_SYMBOL_IMAGE,169,190,128,&lv_font_montserrat_48);
    prev=button(LV_SYMBOL_LEFT,62,200,50,action,3);next=button(LV_SYMBOL_RIGHT,354,200,50,action,4);
    lv_obj_set_style_text_font(lv_obj_get_child(prev,0),&lv_font_montserrat_20,0);
    lv_obj_set_style_text_font(lv_obj_get_child(next,0),&lv_font_montserrat_20,0);
    date_label=label(view,"",60,280,346,body_font);
    favorite=button("收藏",107,310,120,action,5);use=button("应用",239,310,120,action,6);
    remove_button=button("移入回收站",123,359,220,action,7);confirm=button("确认移入？",123,359,220,action,8);
    lv_obj_set_height(remove_button,34);lv_obj_set_height(confirm,34);
    button("桌面",123,400,68,action,0);button("刷新",199,400,68,action,1);button("回收",275,400,68,action,2);
    load_page(page?page->offset:0,page&&page->trash);render();
}
void library_app_debug(cJSON *root)
{
    cJSON_AddBoolToObject(root,"library_active",active);cJSON_AddBoolToObject(root,"library_busy",busy);
    cJSON_AddBoolToObject(root,"library_music_only",music_only);
    cJSON_AddStringToObject(root,"library_kind",current()?current()->kind:"");
    cJSON_AddNumberToObject(root,"library_count",page?page->count:0);cJSON_AddNumberToObject(root,"library_index",item_index);
    cJSON_AddStringToObject(root,"library_id",current()?current()->id:"");cJSON_AddBoolToObject(root,"library_trash",page&&page->trash);
    cJSON_AddBoolToObject(root,"library_favorite",current()&&current()->favorite);cJSON_AddStringToObject(root,"library_message",message);
}
