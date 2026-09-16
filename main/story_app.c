#include "story_app.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "content_assets.h"
#include "content_index.h"
#include "audio_bus.h"
#include "device_api.h"
#include "notification_center.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdatomic.h>

typedef struct {char title[49],text[193],left[25],right[25],hash[65];int picture,a,b;unsigned bytes;} scene_t;
typedef struct {char id[33],title[49];scene_t nodes[5];} book_t;
static lv_obj_t *view,*heading,*status_label,*picture,*text_box,*text_label,*left,*right,*menu,*play_button;
static const lv_font_t *body_font,*title_font;
static void (*go_home)(void);
static uint8_t *pixels;
static lv_image_dsc_t image;
static book_t cached,*incoming;
static bool active,online,busy,finished,ok,autoplay,clear_job;
static unsigned scene;
static SemaphoreHandle_t mutex;
static char job_id[33],download_id[33],cleanup_id[33],theme[16],message[128]="内置故事 · 离线可听";
static char cleanup_target[33];
static atomic_bool cleaning;
static uint32_t next_poll;
static int operation;
static const char *download_error="下载未完成，请稍后重试";
static volatile int storage_errno,storage_stage;
static volatile size_t storage_written,storage_free;
static bool save_progress(void)
{
    nvs_handle_t h;if(nvs_open("story_app",NVS_READWRITE,&h)!=ESP_OK)return false;
    esp_err_t result=nvs_set_u8(h,"scene",scene);
    if(result==ESP_OK)result=nvs_set_u8(h,"online",online);
    if(result==ESP_OK)result=nvs_set_str(h,"job",job_id);
    if(result==ESP_OK)result=nvs_set_str(h,"theme",theme);
    if(result==ESP_OK)result=nvs_set_str(h,"book",cached.id);
    if(result==ESP_OK)result=nvs_commit(h);
    nvs_close(h);return result==ESP_OK;
}
static const char *str(cJSON *j,const char *key){cJSON *v=cJSON_GetObjectItem(j,key);return cJSON_IsString(v)?v->valuestring:"";}
static bool copy(cJSON *j,const char *key,char *out,size_t capacity){const char *v=str(j,key);if(!*v||strlen(v)>=capacity)return false;strlcpy(out,v,capacity);return true;}
static bool number(cJSON *j,const char *key,int min,int max,int *out){cJSON *v=cJSON_GetObjectItem(j,key);if(!cJSON_IsNumber(v)||v->valuedouble!=v->valueint||v->valueint<min||v->valueint>max)return false;*out=v->valueint;return true;}
static bool parse_book(book_t *book,const char *data)
{
    cJSON *j=cJSON_Parse(data);if(!j)return false;
    cJSON *nodes=cJSON_GetObjectItem(j,"nodes");bool valid=copy(j,"title",book->title,sizeof(book->title))&&cJSON_GetArraySize(nodes)==5;
    for(int i=0;i<5&&valid;i++) {
        cJSON *n=cJSON_GetArrayItem(nodes,i);scene_t *s=&book->nodes[i];int bytes;
        valid=copy(n,"title",s->title,sizeof(s->title))&&copy(n,"text",s->text,sizeof(s->text))&&copy(n,"left",s->left,sizeof(s->left))&&copy(n,"right",s->right,sizeof(s->right))&&copy(n,"audio",s->hash,sizeof(s->hash))&&strlen(s->hash)==64&&strspn(s->hash,"0123456789abcdef")==64&&number(n,"picture",0,11,&s->picture)&&number(n,"a",-1,4,&s->a)&&number(n,"b",0,4,&s->b)&&number(n,"bytes",3200,512000,&bytes);
        if(valid){s->bytes=bytes;valid=!(bytes%2)&&s->a==(i==0?1:i<3?3:-1)&&s->b==(i==0?2:i<3?4:0);}
    }
    cJSON_Delete(j);return valid;
}
static void audio_path(char *out,size_t capacity,const char *id,unsigned node){snprintf(out,capacity,"/wallpaper/story-%.16s-%u.pcm",id,node);}
static void book_path(char *out,size_t capacity,const char *id){snprintf(out,capacity,"/wallpaper/story-%.16s.json",id);}
static bool atomic_file(const char *path,const void *data,size_t length)
{
    storage_errno=0;storage_written=0;storage_stage=1;
    /* SPIFFS cannot atomically replace an existing filename. These are immutable,
     * content-addressed files: accept identical retries and never unlink a live book. */
    FILE *old=fopen(path,"rb");
    if(old){uint8_t part[256];size_t offset=0;bool same=true;while(offset<length){size_t n=length-offset;if(n>sizeof(part))n=sizeof(part);if(fread(part,1,n,old)!=n||memcmp(part,(const uint8_t *)data+offset,n)){same=false;break;}offset+=n;}if(same&&fgetc(old)!=EOF)same=false;fclose(old);return same;}
    char temp[100];snprintf(temp,sizeof(temp),"%s.t",path);errno=0;FILE *f=fopen(temp,"wb");if(!f){storage_errno=errno;return false;}
    /* SPIFFS GC reclaims a bounded number of erase blocks per write. A large
     * single fwrite can report ENOSPC despite megabytes of deleted free pages. */
    storage_stage=2;size_t written=0;bool result=true;
    while(written<length){size_t bytes=length-written;if(bytes>4096)bytes=4096;
        size_t n=fwrite((const uint8_t *)data+written,1,bytes,f);written+=n;storage_written=written;
        if(n!=bytes){storage_errno=errno;result=false;break;}
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if(fclose(f)){storage_stage=3;storage_errno=errno;result=false;}
    if(result){storage_stage=4;result=rename(temp,path)==0;if(!result)storage_errno=errno;}
    if(!result)unlink(temp);
    return result;
}
static bool verified_audio(const char *path,const scene_t *scene_data)
{
    FILE *f=fopen(path,"rb");if(!f)return false;
    uint8_t buffer[1024],digest[32];size_t total=0,n;mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);mbedtls_sha256_starts(&hash,0);
    while((n=fread(buffer,1,sizeof(buffer),f))>0){total+=n;mbedtls_sha256_update(&hash,buffer,n);}
    bool valid=!ferror(f)&&total==scene_data->bytes;fclose(f);mbedtls_sha256_finish(&hash,digest);mbedtls_sha256_free(&hash);
    char hex[65];for(int i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",digest[i]);return valid&&!strcmp(hex,scene_data->hash);
}
static bool download_book(const char *id,book_t *book)
{
    if(cached.id[0]&&strcmp(id,cached.id)&&!strncmp(id,cached.id,16))return false;
    download_error="内存正忙，请稍后重试";
    char path[128],file[100];size_t length;uint8_t *data=heap_caps_malloc(512001,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!data)return false;
    char *manifest=heap_caps_malloc(8192,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);bool result=false;
    snprintf(path,sizeof(path),"/v1/stories?id=%s",id);
    download_error="故事内容下载失败，请稍后重试";
    if(!manifest||device_api(path,NULL,manifest,8192,&length)!=ESP_OK||!parse_book(book,manifest))goto done;
    strlcpy(book->id,id,sizeof(book->id));size_t total=0,used=0,needed=0;
    for(int i=0;i<5;i++)needed+=book->nodes[i].bytes;
    download_error="设备存储不足，原故事仍保留";
    if(needed>1900000||esp_spiffs_info("wallpaper",&total,&used)!=ESP_OK||total-used<needed+16000)goto done;
    storage_free=total-used;
    for(int i=0;i<5;i++){
        snprintf(path,sizeof(path),"/v1/stories?id=%s&scene=%d",id,i);
        download_error="语音下载中断，请稍后重试";
        if(device_api(path,NULL,data,512001,&length)!=ESP_OK||length!=book->nodes[i].bytes)goto done;
        unsigned char digest[32];char hex[65];mbedtls_sha256(data,length,digest,0);
        for(int j=0;j<32;j++)snprintf(hex+j*2,3,"%02x",digest[j]);
        download_error="语音校验失败，请重新下载";
        if(strcmp(hex,book->nodes[i].hash))goto done;
        download_error="语音保存失败，原故事仍保留";
        audio_path(file,sizeof(file),id,i);if(!atomic_file(file,data,length))goto done;
        download_error="存储校验失败，原故事仍保留";
        if(!verified_audio(file,&book->nodes[i]))goto done;
    }
    /* Publish immutable manifest, then the UI commits its pointer in transactional NVS. */
    cJSON *j=cJSON_Parse(manifest);cJSON_AddStringToObject(j,"id",id);char *encoded=cJSON_PrintUnformatted(j);
    book_path(file,sizeof(file),id);result=encoded&&atomic_file(file,encoded,strlen(encoded));cJSON_free(encoded);cJSON_Delete(j);
done:
    if(!result)ESP_LOGW("story","%s errno=%d",download_error,errno);
    if(!result&&strcmp(id,cached.id))for(int i=0;i<5;i++){audio_path(file,sizeof(file),id,i);unlink(file);}
    heap_caps_free(data);heap_caps_free(manifest);return result;
}
static void worker(void *unused)
{
    (void)unused;char data[1024],path[128],body[128],id[33]="";bool success=false,pending=false;size_t length;book_t *book=NULL;
    if(operation==2)strlcpy(id,download_id,sizeof(id));
    else{
        snprintf(path,sizeof(path),"/v1/stories?job=%s",job_id);
        snprintf(body,sizeof(body),"{\"requestId\":\"%s\",\"theme\":\"%s\"}",job_id,theme);
        esp_err_t response=device_api("/v1/stories",body,data,sizeof(data),&length);
        if(response==ESP_OK){
            cJSON *j=cJSON_Parse(data);const char *state=str(j,"status");
            if(!strcmp(state,"done"))strlcpy(id,str(j,"id"),sizeof(id));
            else if(!strcmp(state,"working"))pending=true;
            cJSON_Delete(j);
        }else pending=response!=ESP_ERR_INVALID_ARG; /* Keep uncertain request IDs; a definitive rejection creates no job. */
    }
    if(strlen(id)==32&&strspn(id,"0123456789abcdef")==32){book=heap_caps_calloc(1,sizeof(book_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);success=book&&download_book(id,book);}
    xSemaphoreTake(mutex,portMAX_DELAY);incoming=success?book:NULL;if(!success)heap_caps_free(book);
    ok=success;finished=true;
    clear_job=success||(!pending&&!id[0]);
    xSemaphoreGive(mutex);vTaskDeleteWithCaps(NULL);
}
static void cleanup_worker(void *unused)
{
    (void)unused;char path[100];
    for(int i=0;i<5;i++){audio_path(path,sizeof(path),cleanup_target,i);unlink(path);}
    book_path(path,sizeof(path),cleanup_target);unlink(path);
    atomic_store(&cleaning,false);vTaskDeleteWithCaps(NULL);
}
static bool launch(int op)
{
    if(busy||atomic_load(&cleaning))return false;
    operation=op;busy=true;
    if(xTaskCreateWithCaps(worker,"story_fetch",8192,NULL,3,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){busy=false;return false;}return true;
}
static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,int w,const lv_font_t *font)
{
    lv_obj_t *o=lv_label_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);lv_label_set_text(o,text);lv_obj_set_style_text_font(o,font,0);
    lv_obj_set_style_text_color(o,lv_color_hex(0x30435D),0);lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);return o;
}
static void action(lv_event_t *e);
static lv_obj_t *button(lv_obj_t *parent,const char *text,int x,int y,int w,int id)
{
    lv_obj_t *o=lv_button_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,44);lv_obj_set_style_radius(o,22,0);lv_obj_set_style_shadow_width(o,0,0);lv_obj_set_style_pad_all(o,0,0);lv_obj_set_style_bg_color(o,lv_color_hex(0xE8E6FC),0);
    lv_obj_add_event_cb(o,action,LV_EVENT_CLICKED,(void *)(intptr_t)id);lv_obj_t *t=label(o,text,0,0,w,body_font);lv_obj_center(t);return o;
}
static void render(void)
{
    if(!active)return;
    const scene_t *s=&cached.nodes[scene];const story_node_t *b=&builtin_story[scene];
    lv_label_set_text(heading,online?s->title:b->title);lv_label_set_text(text_label,online?s->text:b->text);
    lv_label_set_text(status_label,busy||job_id[0]?"故事制作中，可以切换应用":message);
    lv_label_set_text(lv_obj_get_child(left,0),online?s->left:b->left);lv_label_set_text(lv_obj_get_child(right,0),online?s->right:b->right);
    if(pixels){content_picture(online?s->picture:b->picture,128,pixels);lv_image_cache_drop(&image);lv_image_set_src(picture,&image);}
    lv_obj_scroll_to_y(text_box,0,LV_ANIM_OFF);
}
static void read_scene(void)
{
    esp_err_t result;
    if(online){char path[100];audio_path(path,sizeof(path),cached.id,scene);result=audio_local_play_file(path,cached.nodes[scene].bytes);}
    else result=audio_local_play_asset(builtin_story[scene].audio,builtin_story[scene].bytes);
    if(result==ESP_OK)autoplay=false;
}
static void action(lv_event_t *e)
{
    int a=(int)(intptr_t)lv_event_get_user_data(e);
    if(a==0){go_home();return;}
    if(a==1||a==2){int node=online?(a==1?cached.nodes[scene].a:cached.nodes[scene].b):(a==1?builtin_story[scene].a:builtin_story[scene].b);if(node>=0)scene=node;audio_local_stop();autoplay=true;save_progress();render();}
    if(a==3){audio_local_stop();autoplay=true;}
    if(a==4){lv_obj_remove_flag(menu,LV_OBJ_FLAG_HIDDEN);}
    if(a==5){lv_obj_add_flag(menu,LV_OBJ_FLAG_HIDDEN);}
    if(a==6||a==7){
        if(a==7&&!cached.id[0])return;
        online=a==7;scene=0;audio_local_stop();autoplay=true;save_progress();lv_obj_add_flag(menu,LV_OBJ_FLAG_HIDDEN);strlcpy(message,online?"已下载 · 离线可听":"内置故事 · 离线可听",sizeof(message));render();
    }
    if(a>=10&&a<=12){
        if(busy||job_id[0]){lv_obj_add_flag(menu,LV_OBJ_FLAG_HIDDEN);return;}
        snprintf(job_id,sizeof(job_id),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());
        strlcpy(theme,(const char *[]){"friends","nature","courage"}[a-10],sizeof(theme));save_progress();
        launch(0);lv_obj_add_flag(menu,LV_OBJ_FLAG_HIDDEN);render();notification_post("story","新故事正在编写和配音",20,NOTICE_WORKING);
    }
}
static void tick(lv_timer_t *t)
{
    (void)t;xSemaphoreTake(mutex,portMAX_DELAY);bool done=finished,success=ok;
    if(done){finished=false;busy=false;if(clear_job)job_id[0]=0;if(incoming){audio_local_stop();strlcpy(cleanup_id,cached.id,sizeof(cleanup_id));cached=*incoming;heap_caps_free(incoming);incoming=NULL;online=true;scene=0;autoplay=active;}}
    xSemaphoreGive(mutex);
    if(done){
        strlcpy(message,success?"新故事已下载 · 离线可听":operation==2?download_error:job_id[0]?"网络暂忙，稍后自动查询":"本次未完成，原故事仍可听",sizeof(message));
        if(!save_progress()){cleanup_id[0]=0;strlcpy(message,"进度保存失败，旧故事仍保留",sizeof(message));}
        next_poll=lv_tick_get()+15000;render();
        if(success)notification_post("story","新故事已保存，点此听故事",20,NOTICE_DONE);
        else if(job_id[0])notification_post("story","故事任务继续处理中，可切换应用",20,NOTICE_WORKING);
        else if(!job_id[0])notification_post("story","新故事未完成，可稍后重试",20,NOTICE_FAILED);
    }
    if(cleanup_id[0]&&!audio_local_state().playing&&!atomic_load(&cleaning)){
        if(!strcmp(cleanup_id,cached.id))cleanup_id[0]=0;
        else {
            strlcpy(cleanup_target,cleanup_id,sizeof(cleanup_target));atomic_store(&cleaning,true);
            if(xTaskCreateWithCaps(cleanup_worker,"story_cleanup",4096,NULL,2,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS)cleanup_id[0]=0;
            else atomic_store(&cleaning,false);
        }
    }
    if(!busy&&job_id[0]&&(int32_t)(lv_tick_get()-next_poll)>=0){next_poll=lv_tick_get()+15000;launch(1);}
    if(active&&autoplay&&!audio_local_state().playing)read_scene();
    if(active)lv_label_set_text(lv_obj_get_child(play_button,0),audio_local_state().playing?"重听":"朗读");
}
void story_app_create(lv_obj_t *screen,const lv_font_t *body,const lv_font_t *title,void (*home)(void))
{
    body_font=body;title_font=title;go_home=home;mutex=xSemaphoreCreateMutex();
    view=lv_obj_create(screen);lv_obj_remove_style_all(view);lv_obj_set_size(view,466,466);lv_obj_remove_flag(view,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(view,lv_color_hex(0xFAF7FF),0);lv_obj_set_style_bg_opa(view,255,0);
    char selected_id[33]="",manifest_path[100]="/wallpaper/story-current.json";nvs_handle_t selected;
    if(nvs_open("story_app",NVS_READONLY,&selected)==ESP_OK){size_t size=sizeof(selected_id);nvs_get_str(selected,"book",selected_id,&size);nvs_close(selected);}
    if(strlen(selected_id)==32&&strspn(selected_id,"0123456789abcdef")==32)book_path(manifest_path,sizeof(manifest_path),selected_id);
    FILE *f=fopen(manifest_path,"rb");if(f){char *data=heap_caps_calloc(1,8192,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(data){fread(data,1,8191,f);if(parse_book(&cached,data)){cJSON *j=cJSON_Parse(data);strlcpy(cached.id,str(j,"id"),sizeof(cached.id));cJSON_Delete(j);if(strlen(cached.id)!=32||strspn(cached.id,"0123456789abcdef")!=32)cached.id[0]=0;}heap_caps_free(data);}fclose(f);}
    if(cached.id[0])for(int i=0;i<5;i++){char path[100];struct stat info;audio_path(path,sizeof(path),cached.id,i);if(stat(path,&info)||info.st_size!=cached.nodes[i].bytes){cached.id[0]=0;break;}}
    strlcpy(theme,"friends",sizeof(theme));
    nvs_handle_t h;if(nvs_open("story_app",NVS_READONLY,&h)==ESP_OK){uint8_t n=0,v=0;size_t len=sizeof(job_id);nvs_get_u8(h,"scene",&n);nvs_get_u8(h,"online",&v);nvs_get_str(h,"job",job_id,&len);len=sizeof(theme);nvs_get_str(h,"theme",theme,&len);scene=n<5?n:0;online=v&&cached.id[0];nvs_close(h);}
    strlcpy(message,online?"已下载 · 离线可听":"内置故事 · 离线可听",sizeof(message));next_poll=lv_tick_get()+25000;
    lv_timer_create(tick,200,NULL);
}
lv_obj_t *story_app_view(void){return view;}
bool story_app_request(const char *id)
{
    if(!id||strlen(id)!=32||strspn(id,"0123456789abcdef")!=32||busy||job_id[0]||atomic_load(&cleaning))return false;
    if(!strcmp(id,cached.id)){online=true;scene=0;autoplay=active;save_progress();render();return true;}
    strlcpy(download_id,id,sizeof(download_id));return launch(2);
}
void story_app_set_active(bool value)
{
    if(active==value)return;
    active=value;
    if(!value){audio_local_stop();autoplay=false;lv_obj_clean(view);lv_image_cache_drop(&image);heap_caps_free(pixels);pixels=NULL;return;}
    heading=label(view,"故事屋",83,29,300,body_font);status_label=label(view,"",55,64,356,body_font);
    pixels=heap_caps_calloc(1,32768,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);image=(lv_image_dsc_t){.header.magic=LV_IMAGE_HEADER_MAGIC,.header.cf=LV_COLOR_FORMAT_RGB565,.header.w=128,.header.h=128,.header.stride=256,.data_size=32768,.data=pixels};
    picture=lv_image_create(view);lv_obj_set_pos(picture,169,99);
    text_box=lv_obj_create(view);lv_obj_remove_style_all(text_box);lv_obj_set_pos(text_box,61,232);lv_obj_set_size(text_box,344,106);lv_obj_set_scroll_dir(text_box,LV_DIR_VER);
    text_label=label(text_box,"",4,0,328,body_font);lv_obj_set_style_text_align(text_label,LV_TEXT_ALIGN_LEFT,0);
    left=button(view,"",67,346,160,1);right=button(view,"",239,346,160,2);
    button(view,"桌面",119,403,72,0);play_button=button(view,"朗读",197,403,72,3);button(view,"书架",275,403,72,4);
    menu=lv_obj_create(view);lv_obj_remove_style_all(menu);lv_obj_set_size(menu,466,466);lv_obj_set_style_bg_color(menu,lv_color_hex(0xFAF7FF),0);lv_obj_set_style_bg_opa(menu,255,0);lv_obj_remove_flag(menu,LV_OBJ_FLAG_SCROLLABLE);
    label(menu,"我的故事书",83,35,300,title_font);button(menu,"内置：小猫找星星",90,90,286,6);button(menu,"最近下载的故事",90,142,286,7);
    label(menu,"家长创作 · 每日最多八次",63,201,340,body_font);button(menu,"分享与友谊",110,238,246,10);button(menu,"观察大自然",110,290,246,11);button(menu,"勇敢试一试",110,342,246,12);button(menu,"返回",178,404,110,5);
    lv_obj_add_flag(menu,LV_OBJ_FLAG_HIDDEN);autoplay=true;render();
}
void story_app_debug(cJSON *root)
{
    cJSON_AddBoolToObject(root,"story_active",active);cJSON_AddBoolToObject(root,"story_busy",busy);cJSON_AddBoolToObject(root,"story_online",online);cJSON_AddNumberToObject(root,"story_scene",scene);
    cJSON_AddStringToObject(root,"story_id",cached.id);cJSON_AddStringToObject(root,"story_job",job_id);cJSON_AddStringToObject(root,"story_message",message);
    cJSON_AddNumberToObject(root,"story_storage_errno",storage_errno);cJSON_AddNumberToObject(root,"story_storage_stage",storage_stage);cJSON_AddNumberToObject(root,"story_storage_written",storage_written);cJSON_AddNumberToObject(root,"story_storage_free",storage_free);
    cJSON_AddBoolToObject(root,"story_cleaning",atomic_load(&cleaning));
}
