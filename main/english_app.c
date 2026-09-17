#include "english_app.h"
#include "english_assets.h"
#include "audio_bus.h"
#include "content_assets.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include "nvs.h"
#include <string.h>

static lv_obj_t *view,*heading,*counter,*picture,*word,*meaning,*hint,*surface;
static lv_obj_t *previous,*following,*choice_buttons[2],*choice_images[2],*next_button,*next_text,*mode_text;
static lv_image_dsc_t pictures[ENGLISH_WORD_COUNT],thumbnails[ENGLISH_WORD_COUNT];
static lv_timer_t *audio_timer;
static void (*go_home)(void);
static bool active,quiz,heard,answered,assets_valid,auto_read_pending;
static unsigned card,target,choices[2],correct;
static uint32_t waiting_sequence;
static lv_point_t press;
static uint8_t *image_pixels,*choice_pixels[2];
static int loaded_card=-1,loaded_choices[2]={-1,-1};
static const lv_font_t *body_font,*title_font;
static void save_progress(void)
{
    uint32_t values[]={1,card,target,choices[0],choices[1],correct,quiz,answered};
    nvs_handle_t h;if(nvs_open("english_app",NVS_READWRITE,&h)==ESP_OK){nvs_set_blob(h,"progress",values,sizeof(values));nvs_commit(h);nvs_close(h);}
}

static void visible(lv_obj_t *object,bool enabled)
{
    if(enabled) lv_obj_remove_flag(object,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object,LV_OBJ_FLAG_HIDDEN);
}
static void cancel_reading(void) { audio_local_stop(waiting_sequence); waiting_sequence=0; auto_read_pending=false; }

static void render(void)
{
    lv_label_set_text(heading,quiz ? (correct==5?"你完成啦！":"听音选图") : "看图英语");
    if(quiz) lv_label_set_text_fmt(counter,"找到 %u / 5 个单词",correct);
    else lv_label_set_text_fmt(counter,"%s · %u / %u",english_assets[card].category,card+1,ENGLISH_WORD_COUNT);
    visible(picture,!quiz); visible(surface,!quiz);
    visible(word,!quiz); visible(meaning,!quiz); visible(previous,!quiz); visible(following,!quiz);
    lv_label_set_text(mode_text,quiz?"学习":"练习");
    if(!quiz) {
        if(assets_valid && loaded_card!=(int)card) {
            assets_valid=content_picture(card,176,image_pixels);
            loaded_card=card;
        }
        lv_image_set_src(picture,&pictures[card]);
        lv_label_set_text(word,english_assets[card].word);
        lv_label_set_text(meaning,english_assets[card].chinese);
        lv_obj_set_y(hint,365);
        lv_label_set_text(hint,"点图片听发音 · 左右滑动");
    } else {
        lv_obj_set_y(hint,316);
        lv_label_set_text(hint,correct==5?"真棒！休息一下眼睛吧":answered?"找对啦！":"听单词，再选图片");
    }
    for(unsigned i=0;i<2;++i) {
        visible(choice_buttons[i],quiz);
        if(quiz && assets_valid && loaded_choices[i]!=(int)choices[i]) {
            assets_valid=content_picture(choices[i],128,choice_pixels[i]);
            loaded_choices[i]=choices[i];
        }
        thumbnails[i].data=choice_pixels[i];
        lv_image_set_src(choice_images[i],&thumbnails[i]);
        lv_obj_set_style_border_color(choice_buttons[i],lv_color_hex(answered&&choices[i]==target?0x40AD87:0xDCE7F3),0);
        lv_obj_set_style_border_width(choice_buttons[i],answered&&choices[i]==target?3:1,0);
    }
    visible(next_button,quiz&&answered);
    lv_label_set_text(next_text,correct==5?"再练一轮":"下一题");
    if(!assets_valid) lv_label_set_text(hint,"素材校验失败，请重新烧录");
}

static void new_question(void)
{
    cancel_reading(); heard=false; answered=false;
    target=(card+correct*5)%ENGLISH_WORD_COUNT;
    unsigned side=esp_random()%2;
    choices[side]=target; choices[1-side]=(target+1+esp_random()%(ENGLISH_WORD_COUNT-1))%ENGLISH_WORD_COUNT;
    render();
    auto_read_pending=true;
    save_progress();
}

static void read_word(void)
{
    if(!active || !assets_valid || (quiz&&correct==5)) return;
    auto_read_pending=false;
    if(audio_bus_volume()==0) { lv_label_set_text(hint,"音量为零，请到设置调节"); return; }
    audio_local_state_t current=audio_local_state();
    if(waiting_sequence==current.sequence && current.playing) return;
    unsigned index=quiz?target:card;
    esp_err_t result=content_word_play(index);
    if(result!=ESP_OK) { lv_label_set_text(hint,result==ESP_ERR_INVALID_STATE?"声音正忙，请稍后点读":"播放失败，请再试一次"); return; }
    waiting_sequence=audio_local_state().sequence;
    lv_label_set_text(hint,"正在读，请仔细听");
}

static void audio_tick(lv_timer_t *timer)
{
    (void)timer;
    if(!active) return;
    /* A new question is a single playback intent, not a retry loop for focus. */
    if(auto_read_pending) read_word();
    if(!waiting_sequence) return;
    audio_local_state_t state=audio_local_state();
    if(state.sequence!=waiting_sequence) {waiting_sequence=0;return;}
    if(state.playing) return;
    waiting_sequence=0;
    if(state.result==ESP_OK) {
        heard=true;
        lv_label_set_text(hint,quiz?(answered?"找对啦！":"选一张图片吧"):"跟着读一遍 · 点图可重听");
    } else lv_label_set_text(hint,"发音未播放完成，请重试");
}
static void reading_event(lv_event_t *event) { (void)event; read_word(); }
static void home_event(lv_event_t *event) { (void)event; if(active) go_home(); }
static void mode_event(lv_event_t *event)
{
    (void)event;
    if(!active) return;
    cancel_reading(); quiz=!quiz;
    if(quiz) { correct=0; new_question(); }
    else {render();save_progress();}
}
static void move_card(int direction)
{
    if(!active || quiz) return;
    cancel_reading(); card=(card+ENGLISH_WORD_COUNT+direction)%ENGLISH_WORD_COUNT; render();save_progress();
}
static void navigation_event(lv_event_t *event) { move_card((int)(intptr_t)lv_event_get_user_data(event)); }
static void surface_event(lv_event_t *event)
{
    if(!active || quiz || !lv_indev_active()) return;
    lv_point_t at; lv_indev_get_point(lv_indev_active(),&at);
    if(lv_event_get_code(event)==LV_EVENT_PRESSED) { press=at; return; }
    int dx=at.x-press.x,dy=at.y-press.y;
    if(abs(dx)>55 && abs(dx)>abs(dy)) move_card(dx<0?1:-1);
    else if(abs(dx)<20 && abs(dy)<20) read_word();
}
static void choose_event(lv_event_t *event)
{
    if(!active || !quiz || answered || !assets_valid) return;
    if(!heard) { lv_label_set_text(hint,waiting_sequence?"先听完，再选图片":"点读可重新播放"); return; }
    unsigned side=(unsigned)(uintptr_t)lv_event_get_user_data(event);
    if(choices[side]!=target) { lv_label_set_text(hint,"再听一遍，慢慢找"); return; }
    cancel_reading(); answered=true; ++correct; render();
    save_progress();
    if(correct<5) lv_label_set_text_fmt(hint,"找对啦！%s · %s",english_assets[target].word,english_assets[target].chinese);
}
static void next_event(lv_event_t *event)
{
    (void)event;
    if(!active || !quiz || !answered) return;
    if(correct==5) correct=0;
    new_question();
}
static lv_obj_t *label(lv_obj_t *parent,const char *text,int y,int width,const lv_font_t *font)
{
    lv_obj_t *object=lv_label_create(parent);
    lv_label_set_text(object,text); lv_obj_set_width(object,width);
    lv_obj_set_style_text_font(object,font,0);
    lv_obj_set_style_text_color(object,lv_color_hex(0x263D58),0);
    lv_obj_set_style_text_align(object,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_align(object,LV_ALIGN_TOP_MID,0,y);
    return object;
}
static lv_obj_t *button(lv_obj_t *parent,int x,int y,int width,int height,lv_event_cb_t event,void *data)
{
    lv_obj_t *object=lv_button_create(parent);
    lv_obj_set_size(object,width,height); lv_obj_set_pos(object,x,y);
    lv_obj_set_style_radius(object,22,0); lv_obj_set_style_shadow_width(object,0,0);
    lv_obj_set_style_bg_color(object,lv_color_hex(0xEBF3FC),0);
    lv_obj_set_style_pad_all(object,0,0);
    lv_obj_add_event_cb(object,event,LV_EVENT_CLICKED,data);
    return object;
}

static void create_content(void)
{
    const lv_font_t *body=body_font,*title=title_font;
    assets_valid=content_assets_ready();loaded_card=-1;loaded_choices[0]=loaded_choices[1]=-1;
    image_pixels=heap_caps_calloc(1,176*176*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    for(unsigned i=0;i<2;++i) choice_pixels[i]=heap_caps_calloc(1,128*128*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!image_pixels||!choice_pixels[0]||!choice_pixels[1]) {
        assets_valid=false;
        label(view,"内存正忙，请返回后重试",200,356,body);
        lv_obj_t *home=button(view,173,400,120,44,home_event,NULL);
        lv_obj_t *text=lv_label_create(home);lv_obj_set_style_text_font(text,body,0);lv_label_set_text(text,"桌面");lv_obj_center(text);
        return;
    }
    for(unsigned i=0;i<ENGLISH_WORD_COUNT;++i) {
        pictures[i]=(lv_image_dsc_t){.header.magic=LV_IMAGE_HEADER_MAGIC,.header.cf=LV_COLOR_FORMAT_RGB565,
            .header.w=176,.header.h=176,.header.stride=352,.data_size=176*176*2,.data=image_pixels};
        thumbnails[i]=(lv_image_dsc_t){.header.magic=LV_IMAGE_HEADER_MAGIC,.header.cf=LV_COLOR_FORMAT_RGB565,
            .header.w=128,.header.h=128,.header.stride=256,.data_size=128*128*2,.data=choice_pixels[0]};
    }
    heading=label(view,"看图英语",36,320,title); counter=label(view,"",73,320,body);
    picture=lv_image_create(view); lv_obj_set_pos(picture,145,99);
    word=label(view,"",275,344,&lv_font_montserrat_48); meaning=label(view,"",336,330,body);
    hint=label(view,"",365,346,body);
    surface=lv_obj_create(view); lv_obj_remove_style_all(surface);
    lv_obj_set_pos(surface,0,98); lv_obj_set_size(surface,466,263);
    lv_obj_add_flag(surface,LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(surface,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(surface,surface_event,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(surface,surface_event,LV_EVENT_RELEASED,NULL);
    previous=button(view,52,192,44,52,navigation_event,(void *)(intptr_t)-1);
    following=button(view,370,192,44,52,navigation_event,(void *)(intptr_t)1);
    lv_obj_t *text=label(previous,LV_SYMBOL_LEFT,0,40,&lv_font_montserrat_20); lv_obj_center(text);
    text=label(following,LV_SYMBOL_RIGHT,0,40,&lv_font_montserrat_20); lv_obj_center(text);
    for(unsigned i=0;i<2;++i) {
        choice_buttons[i]=button(view,73+i*172,151,148,148,choose_event,(void *)(uintptr_t)i);
        lv_obj_set_style_bg_color(choice_buttons[i],lv_color_white(),0);
        choice_images[i]=lv_image_create(choice_buttons[i]); lv_obj_set_pos(choice_images[i],10,10);
    }
    next_button=button(view,108,353,250,36,next_event,NULL);
    next_text=label(next_button,"下一题",0,240,body); lv_obj_center(next_text);
    const char *names[]={"桌面","点读","练习"};
    for(unsigned i=0;i<3;++i) {
        lv_obj_t *control=button(view,i==0?125:i==1?193:277,400,i==1?80:64,44,
                                  i==0?home_event:i==1?reading_event:mode_event,NULL);
        text=label(control,names[i],0,i==1?76:60,body); lv_obj_center(text);
        if(i==2) mode_text=text;
    }
    render();
}
void english_app_create(lv_obj_t *screen,const lv_font_t *body,const lv_font_t *title,void (*home)(void))
{
    body_font=body;title_font=title;go_home=home;
    uint32_t values[8]={0};size_t size=sizeof(values);nvs_handle_t h;
    if(nvs_open("english_app",NVS_READONLY,&h)==ESP_OK){esp_err_t result=nvs_get_blob(h,"progress",values,&size);nvs_close(h);
        if(result==ESP_OK&&size==sizeof(values)&&values[0]==1&&values[1]<12&&values[2]<12&&values[3]<12&&values[4]<12&&values[5]<=5){card=values[1];target=values[2];choices[0]=values[3];choices[1]=values[4];correct=values[5];quiz=values[6]!=0;answered=values[7]!=0;heard=answered;}}
    view=lv_obj_create(screen);lv_obj_remove_style_all(view);lv_obj_set_size(view,466,466);
    lv_obj_set_style_bg_color(view,lv_color_white(),0);lv_obj_set_style_bg_opa(view,255,0);lv_obj_remove_flag(view,LV_OBJ_FLAG_SCROLLABLE);
    audio_timer=lv_timer_create(audio_tick,100,NULL);lv_timer_pause(audio_timer);
}
lv_obj_t *english_app_view(void) { return view; }
void english_app_set_active(bool enabled)
{
    if(active==enabled)return;
    active=enabled;
    if(!enabled) {
        cancel_reading(); lv_timer_pause(audio_timer);
        lv_obj_clean(view);heap_caps_free(image_pixels);image_pixels=NULL;
        for(unsigned i=0;i<2;++i){heap_caps_free(choice_pixels[i]);choice_pixels[i]=NULL;}
    }
    else {
        create_content();
        if(quiz && !heard && !answered) auto_read_pending=true;
        lv_timer_resume(audio_timer);
    }
}
void english_app_debug(cJSON *root)
{
    audio_local_state_t state=audio_local_state();
    cJSON_AddNumberToObject(root,"english_card",card);
    cJSON_AddBoolToObject(root,"english_quiz",quiz);
    cJSON_AddBoolToObject(root,"english_heard",heard);
    cJSON_AddBoolToObject(root,"english_answered",answered);
    cJSON_AddNumberToObject(root,"english_correct",correct);
    cJSON_AddNumberToObject(root,"english_target",target);
    cJSON_AddNumberToObject(root,"english_choice0",choices[0]);
    cJSON_AddNumberToObject(root,"english_choice1",choices[1]);
    cJSON_AddBoolToObject(root,"english_assets_valid",assets_valid);
    cJSON_AddBoolToObject(root,"english_playing",state.playing);
    cJSON_AddNumberToObject(root,"english_audio_result",state.result);
    cJSON_AddNumberToObject(root,"english_audio_written",state.written);
    cJSON_AddNumberToObject(root,"english_audio_sequence",state.sequence);
}
