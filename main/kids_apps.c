#include "kids_apps.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "content_assets.h"
#include "audio_bus.h"
#include "voice_input.h"
#include "pet_dialogue.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

/* Progress is small, independent of LVGL, and survives scene destruction/reboot. */
typedef struct {uint32_t version;uint16_t pats,meals;uint8_t asleep;} pet_state_t;
typedef struct {uint32_t version;uint8_t deck[6],words[3],matched;uint16_t turns,wins;} flip_state_t;
static pet_state_t pet={.version=1};
static flip_state_t flip;
static lv_obj_t *roots[2],*message,*cards[6],*pictures[6],*backs[6],*counter,*pet_voice_button,*pet_voice_label,*pet_wave[5],*pet_picture,*pet_status_pill;
static const lv_font_t *body_font,*title_font;
static void (*go_home)(void);
static int active=-1,first=-1,second=-1;
static uint32_t hide_at,smile_until,frame;
static uint8_t *flip_pixels[3],*pet_pixels;
static lv_image_dsc_t images[3];
static lv_image_dsc_t pet_image;
static unsigned pet_applied_sequence,pet_spoken_sequence;
static int pet_displayed_phase=-1;
static unsigned pet_displayed_sequence;
static int pet_visual_state=-1;
static uint8_t pet_kind,pet_preview_kind,pet_page;
static const char *pet_names[]={"团团","旺旺","小羽","跳跳"};
static void save(const char *key,const void *data,size_t bytes);
static void pet_apply_voice(void)
{
    pet_dialogue_state_t s=pet_dialogue_state();
    if(s.phase==PET_READY&&s.sequence!=pet_applied_sequence){
        if(s.action==1){pet.pats++;pet.asleep=0;}else if(s.action==2){pet.meals++;pet.asleep=0;}else if(s.action==3)pet.asleep=1;else if(s.action==4)pet.asleep=0;
        if(s.action)save("pet",&pet,sizeof(pet));
        pet_applied_sequence=s.sequence;smile_until=lv_tick_get()+2200;
    }
    if(active!=0||pet_page!=0||!message)return;
    if(s.phase==PET_READY&&s.sequence!=pet_spoken_sequence&&pet_dialogue_play()==ESP_OK)pet_spoken_sequence=s.sequence;
    const char *text=s.phase==PET_LISTENING?"我在听，讲完再点一次":
        s.phase==PET_THINKING?(voice_input_is_processing()?"正在识别你的声音…":"正在准备回答，可以先去玩别的"):s.reply;
    if(*text&&(pet_displayed_phase!=(int)s.phase||pet_displayed_sequence!=s.sequence))lv_label_set_text(message,text);
    pet_displayed_phase=s.phase;pet_displayed_sequence=s.sequence;
}
static void save(const char *key,const void *data,size_t bytes)
{
    nvs_handle_t handle;
    if(nvs_open("kids_apps",NVS_READWRITE,&handle)==ESP_OK){if(nvs_set_blob(handle,key,data,bytes)==ESP_OK)nvs_commit(handle);nvs_close(handle);}
}
static void load(const char *key,void *data,size_t bytes)
{
    nvs_handle_t handle;size_t length=bytes;
    if(nvs_open("kids_apps",NVS_READONLY,&handle)==ESP_OK){nvs_get_blob(handle,key,data,&length);nvs_close(handle);}
}
static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,int w,const lv_font_t *font)
{
    lv_obj_t *o=lv_label_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);lv_label_set_text(o,text);
    lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(0x30435D),0);lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);return o;
}
static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color,int radius)
{
    lv_obj_t *o=lv_obj_create(parent);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,255,0);lv_obj_set_style_radius(o,radius,0);lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);return o;
}
static void action(lv_event_t *e);
static lv_obj_t *button(const char *text,int x,int y,int w,int id)
{
    lv_obj_t *o=box(roots[active],x,y,w,44,0xE6ECF8,22);lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o,action,LV_EVENT_CLICKED,(void *)(intptr_t)id);lv_obj_t *t=label(o,text,0,0,w,body_font);lv_obj_center(t);return o;
}
static lv_obj_t *pet_button(lv_obj_t *parent,const char *text,int x,int y,int w,int h,uint32_t color,int id)
{
    lv_obj_t *o=box(parent,x,y,w,h,color,h/2);lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o,action,LV_EVENT_CLICKED,(void *)(intptr_t)id);
    lv_obj_t *t=label(o,text,8,0,w-16,body_font);lv_obj_center(t);return o;
}
static bool pet_load_picture(uint8_t kind)
{
    if(kind>=4)return false;
    if(!pet_pixels)pet_pixels=heap_caps_calloc(1,220*220*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!pet_pixels)return false;
    lv_image_cache_drop(&pet_image);
    if(!content_pet_picture(kind,pet_pixels))return false;
    pet_image=(lv_image_dsc_t){.header.magic=LV_IMAGE_HEADER_MAGIC,.header.cf=LV_COLOR_FORMAT_RGB565,
        .header.w=220,.header.h=220,.header.stride=440,.data_size=220*220*2,.data=pet_pixels};
    return true;
}
static void pet_render_page(void *argument)
{
    pet_page=(uint8_t)(uintptr_t)argument;
    if(active!=0)return;
    lv_obj_t *root=roots[0];lv_obj_clean(root);message=NULL;pet_picture=NULL;pet_voice_button=NULL;
    pet_status_pill=NULL;for(int i=0;i<5;i++)pet_wave[i]=NULL;
    if(pet_page==0){
        pet_dialogue_set_pet(pet_kind);
        if(pet_load_picture(pet_kind)){
            pet_picture=lv_image_create(root);lv_image_set_src(pet_picture,&pet_image);lv_obj_set_pos(pet_picture,123,43);
            lv_obj_add_flag(pet_picture,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(pet_picture,action,LV_EVENT_CLICKED,(void *)1);
        }else label(root,"宠物图片暂时不可用",83,140,300,body_font);
        pet_status_pill=box(root,70,270,326,54,0xF1F4F8,27);
        char greeting[64];snprintf(greeting,sizeof(greeting),pet.asleep?"%s在睡觉，轻触叫醒它":"%s · 点一下，说说你的想法",pet_names[pet_kind]);
        message=label(pet_status_pill,greeting,16,7,294,body_font);
        lv_obj_set_height(message,40);lv_label_set_long_mode(message,LV_LABEL_LONG_WRAP);
        lv_obj_add_flag(pet_status_pill,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(pet_status_pill,action,LV_EVENT_CLICKED,(void *)5);
        pet_voice_button=pet_button(root,"和我说话",83,332,300,60,0x6ED8B6,4);pet_voice_label=lv_obj_get_child(pet_voice_button,0);
        for(int i=0;i<5;i++){pet_wave[i]=box(pet_voice_button,24+i*10,27,6,6,0xFFFFFF,3);lv_obj_remove_flag(pet_wave[i],LV_OBJ_FLAG_CLICKABLE);}
        pet_button(root,"照顾",103,400,124,44,0xFFE0A8,6);pet_button(root,"换伙伴",239,400,124,44,0xD9D3FF,7);
        pet_visual_state=-1;pet_displayed_phase=-1;pet_apply_voice();
    }else if(pet_page==1){
        label(root,"照顾小伙伴",83,36,300,title_font);label(root,"每个按钮都大一点，慢慢选",63,76,340,body_font);
        pet_button(root,"摸摸头",74,112,150,92,0xFFD7C8,1);
        pet_button(root,"喂点心",242,112,150,92,0xFFE6A8,2);
        pet_button(root,pet.asleep?"叫醒它":"哄它睡觉",74,220,150,92,0xD7E8FF,3);
        pet_button(root,"重听回答",242,220,150,92,0xDDD6FF,5);
        message=label(root,"点一个动作，马上就有回应",63,326,340,body_font);
        pet_button(root,"返回宠物",103,370,260,66,0x6ED8B6,8);
    }else{
        label(root,"选择新伙伴",83,36,300,title_font);
        if(pet_load_picture(pet_preview_kind)){
            pet_picture=lv_image_create(root);lv_image_set_src(pet_picture,&pet_image);lv_obj_set_pos(pet_picture,123,68);
        }else label(root,"宠物图片暂时不可用",83,150,300,body_font);
        pet_button(root,"<",61,148,68,68,0xE8ECF5,9);pet_button(root,">",337,148,68,68,0xE8ECF5,10);
        label(root,pet_names[pet_preview_kind],83,291,300,title_font);
        pet_button(root,"选择这个伙伴",103,342,260,66,0x6ED8B6,11);
        pet_button(root,"返回",171,416,124,38,0xE8ECF5,8);
    }
}
static void pet_show_page(uint8_t page){lv_async_call(pet_render_page,(void *)(uintptr_t)page);}
static void flip_render(void)
{
    if(active!=1)return;
    for(int i=0;i<6;i++) {
        bool reveal=(flip.matched&(1<<i))||i==first||i==second;
        if(reveal){lv_obj_remove_flag(pictures[i],LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(backs[i],LV_OBJ_FLAG_HIDDEN);}
        else {lv_obj_add_flag(pictures[i],LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(backs[i],LV_OBJ_FLAG_HIDDEN);}
        lv_obj_set_style_bg_color(cards[i],lv_color_hex((flip.matched&(1<<i))?0xD8F3E5:reveal?0xFFFFFF:0xDFE9FA),0);
    }
    lv_label_set_text_fmt(counter,flip.matched==63?"全找到了！你真棒":"翻开两张，找相同的图案");
}
static void new_flip(void)
{
    flip.version=1;flip.matched=0;flip.turns=0;first=second=-1;
    for(int i=0;i<3;i++){do{flip.words[i]=esp_random()%12;}while((i>0&&flip.words[i]==flip.words[0])||(i>1&&flip.words[i]==flip.words[1]));}
    for(int i=0;i<6;i++)flip.deck[i]=i%3;
    for(int i=5;i>0;i--){int j=esp_random()%(i+1);uint8_t t=flip.deck[i];flip.deck[i]=flip.deck[j];flip.deck[j]=t;}
    save("flip",&flip,sizeof(flip));
}
static void fill_images(void)
{
    for(int i=0;i<3;i++)if(flip_pixels[i]){content_picture(flip.words[i],128,flip_pixels[i]);lv_image_cache_drop(&images[i]);}
    for(int i=0;i<6;i++)lv_image_set_src(pictures[i],&images[flip.deck[i]]);
}
static void action(lv_event_t *e)
{
    int a=(int)(intptr_t)lv_event_get_user_data(e);
    if(a==0){go_home();return;}
    if(active==0){
        if(a==1){pet.pats++;pet.asleep=0;if(message)lv_label_set_text_fmt(message,"摸摸头，%s喜欢你！",pet_names[pet_kind]);}
        else if(a==2){pet.meals++;pet.asleep=0;if(message)lv_label_set_text(message,"啊呜，谢谢你的美味点心！");}
        else if(a==3){pet.asleep=!pet.asleep;if(message)lv_label_set_text(message,pet.asleep?"晚安，我要做个甜甜的梦":"早上好，一起玩吧！");}
        else if(a==4){
            pet_dialogue_state_t s=pet_dialogue_state();
            if(s.phase==PET_LISTENING)pet_dialogue_finish();
            else if(s.phase!=PET_THINKING&&pet_dialogue_start()!=ESP_OK&&message)lv_label_set_text(message,"声音正忙，请稍后再说");
            return;
        }
        else if(a==5){
            pet_dialogue_state_t s=pet_dialogue_state();
            if(s.phase==PET_READY){
                if(pet_page==1)pet_dialogue_play();
                else{pet_dialogue_leave();pet_spoken_sequence=s.sequence-1;}
            }
            return;
        }
        else if(a==6){pet_show_page(1);return;}
        else if(a==7){pet_preview_kind=pet_kind;pet_show_page(2);return;}
        else if(a==8){pet_show_page(0);return;}
        else if(a==9){pet_preview_kind=(pet_preview_kind+3)%4;pet_show_page(2);return;}
        else if(a==10){pet_preview_kind=(pet_preview_kind+1)%4;pet_show_page(2);return;}
        else if(a==11){
            pet_dialogue_state_t s=pet_dialogue_state();
            if(s.phase==PET_LISTENING||s.phase==PET_THINKING){pet_show_page(0);return;}
            pet_kind=pet_preview_kind;save("pet_kind",&pet_kind,sizeof(pet_kind));pet_dialogue_set_pet(pet_kind);pet_show_page(0);return;
        }
        smile_until=lv_tick_get()+2000;save("pet",&pet,sizeof(pet));
    }else if(active==1){
        if(a==10){new_flip();fill_images();flip_render();return;}
        int i=a-20;if(i<0||i>=6||second>=0||(flip.matched&(1<<i))||i==first)return;
        if(first<0)first=i;
        else {
            second=i;flip.turns++;
            if(flip.deck[first]==flip.deck[second]){
                flip.matched|=(1<<first)|(1<<second);first=second=-1;
                if(flip.matched==63)flip.wins++;
                save("flip",&flip,sizeof(flip));
            }else hide_at=lv_tick_get()+950;
        }
        flip_render();
    }
}
static void tick(lv_timer_t *timer)
{
    (void)timer;
    pet_apply_voice();
    if(active==0){
        frame++;
        if(pet_picture)lv_obj_set_y(pet_picture,(pet_page==0?43:68)+((pet.asleep?0:frame%20<10?-2:1))+
            ((lv_tick_get()<smile_until&&frame%4<2)?-4:0));
        if(pet_page!=0)return;
        pet_dialogue_state_t s=pet_dialogue_state();
        bool recognizing=s.phase==PET_THINKING&&voice_input_is_processing();
        bool speaking=s.phase==PET_READY&&audio_local_state().playing;
        int visual=(int)s.phase+(recognizing?10:0)+(speaking?20:0);
        const char *caption=s.phase==PET_LISTENING?"结束录音":recognizing?"正在识别…":
            s.phase==PET_THINKING?"等待回答…":speaking?"正在回答…":"和我说话";
        if(strcmp(lv_label_get_text(pet_voice_label),caption))lv_label_set_text(pet_voice_label,caption);
        if(s.phase==PET_THINKING)lv_obj_add_state(pet_voice_button,LV_STATE_DISABLED);else lv_obj_remove_state(pet_voice_button,LV_STATE_DISABLED);
        if(visual!=pet_visual_state){
            uint32_t button_color=s.phase==PET_LISTENING?0xFF7168:recognizing?0xF3B64C:
                s.phase==PET_THINKING?0x73A7F6:speaking?0xA37AF4:0x6ED8B6;
            uint32_t pill_color=s.phase==PET_LISTENING?0xFFE5E2:recognizing?0xFFF0CE:
                s.phase==PET_THINKING?0xE4EEFF:speaking?0xEDE4FF:s.phase==PET_FAILED?0xFFE4E4:0xF1F4F8;
            lv_obj_set_style_bg_color(pet_voice_button,lv_color_hex(button_color),0);
            lv_obj_set_style_bg_color(pet_status_pill,lv_color_hex(pill_color),0);pet_visual_state=visual;
        }
        uint8_t level=s.phase==PET_LISTENING?voice_input_level():0;
        for(int i=0;i<5;i++){int h=s.phase==PET_LISTENING?6+(level*(i%2?18:24))/100:6+(i%2)*3;
            lv_obj_set_height(pet_wave[i],h);lv_obj_set_y(pet_wave[i],30-h/2);}
    }else if(active==1&&second>=0&&(int32_t)(lv_tick_get()-hide_at)>=0){first=second=-1;flip_render();}
}
void kids_apps_create(lv_obj_t *screen,const lv_font_t *body,const lv_font_t *title,void (*home)(void))
{
    body_font=body;title_font=title;go_home=home;load("pet",&pet,sizeof(pet));load("pet_kind",&pet_kind,sizeof(pet_kind));load("flip",&flip,sizeof(flip));
    bool valid=flip.version==1&&flip.matched<=63;int counts[3]={0};
    for(int i=0;i<6;i++){if(flip.deck[i]>2)valid=false;else counts[flip.deck[i]]++;}
    for(int i=0;i<3;i++)if(counts[i]!=2||flip.words[i]>11)valid=false;
    if(!valid)new_flip();
    if(pet.version!=1)pet=(pet_state_t){.version=1};
    if(pet_kind>=4)pet_kind=0;
    for(int i=0;i<2;i++)roots[i]=box(screen,0,0,466,466,i?0xF5F8FF:0xFFFFFF,233);
    lv_timer_create(tick,125,NULL);
}
lv_obj_t *kids_apps_view(unsigned i){return roots[i<2?i:0];}
void kids_apps_activate(int index)
{
    if(index==active)return;
    if(active==0)pet_dialogue_leave();
    if(active>=0)lv_obj_clean(roots[active]);
    for(int i=0;i<3;i++){lv_image_cache_drop(&images[i]);heap_caps_free(flip_pixels[i]);flip_pixels[i]=NULL;}
    lv_image_cache_drop(&pet_image);heap_caps_free(pet_pixels);pet_pixels=NULL;
    first=second=-1;active=index;if(active<0)return;
    lv_obj_t *root=roots[active];
    if(active==0){
        pet_page=0;pet_render_page((void *)(uintptr_t)0);
    }else{
        label(root,"记忆翻牌",83,33,300,title_font);counter=label(root,"",53,72,360,body_font);
        for(int i=0;i<3;i++){
            flip_pixels[i]=heap_caps_calloc(1,32768,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(!flip_pixels[i]){lv_obj_clean(root);label(root,"内存正忙，请返回后重试",55,200,356,body_font);button("桌面",178,400,110,0);return;}
            images[i]=(lv_image_dsc_t){.header.magic=LV_IMAGE_HEADER_MAGIC,.header.cf=LV_COLOR_FORMAT_RGB565,.header.w=128,.header.h=128,.header.stride=256,.data_size=32768,.data=flip_pixels[i]};
        }
        for(int i=0;i<6;i++){
            cards[i]=box(root,117+(i%2)*122,103+(i/2)*99,110,90,0xDFE9FA,20);lv_obj_add_flag(cards[i],LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cards[i],action,LV_EVENT_CLICKED,(void *)(intptr_t)(20+i));
            pictures[i]=lv_image_create(cards[i]);lv_image_set_scale(pictures[i],144);lv_obj_center(pictures[i]);lv_obj_remove_flag(pictures[i],LV_OBJ_FLAG_CLICKABLE);
            backs[i]=label(cards[i],"?",0,15,110,&lv_font_montserrat_48);
        }
        fill_images();for(int i=0;i<6;i++)lv_obj_center(pictures[i]);flip_render();button("桌面",119,407,108,0);button("新一局",239,407,108,10);
    }
}
void kids_apps_debug(cJSON *root)
{
    pet_dialogue_state_t s=pet_dialogue_state();cJSON_AddNumberToObject(root,"pet_phase",s.phase);cJSON_AddNumberToObject(root,"pet_reply_sequence",s.sequence);cJSON_AddNumberToObject(root,"pet_audio_bytes",s.bytes);cJSON_AddStringToObject(root,"pet_reply",s.reply);
    cJSON_AddNumberToObject(root,"kids_active",active);cJSON_AddBoolToObject(root,"pet_asleep",pet.asleep);
    cJSON_AddNumberToObject(root,"pet_pats",pet.pats);cJSON_AddNumberToObject(root,"pet_meals",pet.meals);
    cJSON_AddNumberToObject(root,"pet_kind",pet_kind);cJSON_AddNumberToObject(root,"pet_page",pet_page);
    cJSON_AddNumberToObject(root,"flip_matched",flip.matched);cJSON_AddNumberToObject(root,"flip_first",first);cJSON_AddNumberToObject(root,"flip_second",second);
    cJSON_AddNumberToObject(root,"flip_turns",flip.turns);cJSON_AddItemToObject(root,"flip_deck",cJSON_CreateIntArray((int[]){flip.deck[0],flip.deck[1],flip.deck[2],flip.deck[3],flip.deck[4],flip.deck[5]},6));
}
