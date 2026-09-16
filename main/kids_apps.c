#include "kids_apps.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "content_assets.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "nvs.h"
#include <string.h>

/* Progress is small, independent of LVGL, and survives scene destruction/reboot. */
typedef struct {uint32_t version;uint16_t pats,meals;uint8_t asleep;} pet_state_t;
typedef struct {uint32_t version;uint8_t deck[6],words[3],matched;uint16_t turns,wins;} flip_state_t;
static pet_state_t pet={.version=1};
static flip_state_t flip;
static lv_obj_t *roots[2],*message,*eyes[2],*face,*cards[6],*pictures[6],*backs[6],*counter;
static const lv_font_t *body_font,*title_font;
static void (*go_home)(void);
static int active=-1,first=-1,second=-1;
static uint32_t hide_at,smile_until,frame;
static uint8_t *pixels[3];
static lv_image_dsc_t images[3];
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
    for(int i=0;i<3;i++)if(pixels[i]){content_picture(flip.words[i],128,pixels[i]);lv_image_cache_drop(&images[i]);}
    for(int i=0;i<6;i++)lv_image_set_src(pictures[i],&images[flip.deck[i]]);
}
static void action(lv_event_t *e)
{
    int a=(int)(intptr_t)lv_event_get_user_data(e);
    if(a==0){go_home();return;}
    if(active==0){
        if(a==1){pet.pats++;pet.asleep=0;lv_label_set_text(message,"摸摸头，团团喜欢你！");}
        else if(a==2){pet.meals++;pet.asleep=0;lv_label_set_text(message,"啊呜，谢谢你的美味点心！");}
        else if(a==3){pet.asleep=!pet.asleep;lv_label_set_text(message,pet.asleep?"晚安，点一下就能叫醒我":"早上好，一起玩吧！");}
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
    if(active==0){
        frame++;bool blink=pet.asleep||(frame%32==0);int height=blink?4:16;
        for(int i=0;i<2;i++){lv_obj_set_height(eyes[i],height);lv_obj_set_y(eyes[i],68+(16-height)/2);}
        lv_obj_set_y(face,140+((lv_tick_get()<smile_until)?(frame%4<2?-4:0):0));
    }else if(active==1&&second>=0&&(int32_t)(lv_tick_get()-hide_at)>=0){first=second=-1;flip_render();}
}
void kids_apps_create(lv_obj_t *screen,const lv_font_t *body,const lv_font_t *title,void (*home)(void))
{
    body_font=body;title_font=title;go_home=home;load("pet",&pet,sizeof(pet));load("flip",&flip,sizeof(flip));
    bool valid=flip.version==1&&flip.matched<=63;int counts[3]={0};
    for(int i=0;i<6;i++){if(flip.deck[i]>2)valid=false;else counts[flip.deck[i]]++;}
    for(int i=0;i<3;i++)if(counts[i]!=2||flip.words[i]>11)valid=false;
    if(!valid)new_flip();
    if(pet.version!=1)pet=(pet_state_t){.version=1};
    for(int i=0;i<2;i++)roots[i]=box(screen,0,0,466,466,i?0xF5F8FF:0xFFF6EC,233);
    lv_timer_create(tick,125,NULL);
}
lv_obj_t *kids_apps_view(unsigned i){return roots[i<2?i:0];}
void kids_apps_activate(int index)
{
    if(index==active)return;
    if(active>=0)lv_obj_clean(roots[active]);
    for(int i=0;i<3;i++){lv_image_cache_drop(&images[i]);heap_caps_free(pixels[i]);pixels[i]=NULL;}
    first=second=-1;active=index;if(active<0)return;
    lv_obj_t *root=roots[active];
    if(active==0){
        label(root,"团团的小窝",83,36,300,title_font);label(root,"轻轻摸头，陪它玩一会儿",65,79,336,body_font);
        box(root,146,124,53,67,0xEDB6A0,25);box(root,267,124,53,67,0xEDB6A0,25);
        face=box(root,133,140,200,165,0xFFD5B5,75);lv_obj_add_flag(face,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(face,action,LV_EVENT_CLICKED,(void *)1);
        eyes[0]=box(face,55,68,12,16,0x54403C,8);eyes[1]=box(face,133,68,12,16,0x54403C,8);
        box(face,30,96,30,15,0xF0A4A2,8);box(face,140,96,30,15,0xF0A4A2,8);
        for(int i=0;i<2;i++){
            lv_obj_t *mouth=lv_arc_create(face);lv_obj_remove_style_all(mouth);lv_obj_set_pos(mouth,85+i*14,96);lv_obj_set_size(mouth,18,18);
            lv_arc_set_bg_angles(mouth,0,180);lv_obj_set_style_arc_width(mouth,3,LV_PART_MAIN);lv_obj_set_style_arc_color(mouth,lv_color_hex(0x54403C),LV_PART_MAIN);lv_obj_set_style_arc_opa(mouth,255,LV_PART_MAIN);
            lv_obj_set_style_arc_opa(mouth,0,LV_PART_INDICATOR);lv_obj_remove_flag(mouth,LV_OBJ_FLAG_CLICKABLE);
        }
        message=label(root,pet.asleep?"团团睡着了，轻触可以叫醒":"你好呀，我是团团！",53,315,360,body_font);
        button("喂点心",105,355,122,2);button("睡觉 / 醒来",239,355,132,3);button("桌面",178,409,110,0);
    }else{
        label(root,"记忆翻牌",83,33,300,title_font);counter=label(root,"",53,72,360,body_font);
        for(int i=0;i<3;i++){
            pixels[i]=heap_caps_calloc(1,32768,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(!pixels[i]){lv_obj_clean(root);label(root,"内存正忙，请返回后重试",55,200,356,body_font);button("桌面",178,400,110,0);return;}
            images[i]=(lv_image_dsc_t){.header.magic=LV_IMAGE_HEADER_MAGIC,.header.cf=LV_COLOR_FORMAT_RGB565,.header.w=128,.header.h=128,.header.stride=256,.data_size=32768,.data=pixels[i]};
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
    cJSON_AddNumberToObject(root,"kids_active",active);cJSON_AddBoolToObject(root,"pet_asleep",pet.asleep);
    cJSON_AddNumberToObject(root,"pet_pats",pet.pats);cJSON_AddNumberToObject(root,"pet_meals",pet.meals);
    cJSON_AddNumberToObject(root,"flip_matched",flip.matched);cJSON_AddNumberToObject(root,"flip_first",first);cJSON_AddNumberToObject(root,"flip_second",second);
    cJSON_AddNumberToObject(root,"flip_turns",flip.turns);cJSON_AddItemToObject(root,"flip_deck",cJSON_CreateIntArray((int[]){flip.deck[0],flip.deck[1],flip.deck[2],flip.deck[3],flip.deck[4],flip.deck[5]},6));
}
