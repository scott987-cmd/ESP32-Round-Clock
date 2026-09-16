#include "star_game.h"
#include "esp_random.h"
#include <stdlib.h>

/* Entirely local, no audio/network/flash writes, no always-running timer.
 * Only a small star is repainted during each 360 ms tap celebration. */
static lv_obj_t *view, *star, *heading, *instruction, *progress, *dots[5];
static void (*go_home)(void);
static unsigned found, place;
static bool active, busy;
static int pulse;
static lv_point_t pressed;
static const lv_point_t positions[] = {
    {136, 222}, {330, 222}, {233, 248}, {173, 277}, {293, 277}, {233, 222},
};
static const uint32_t colors[] = {0xFFCC48, 0xFF9778, 0x8AD9C3, 0xA99AF4, 0xFFC75C};
static const lv_point_t points[] = {
    {50,5},{63,34},{95,38},{71,61},{78,94},
    {50,78},{22,94},{29,61},{5,38},{37,34},
};

static void draw_star(lv_event_t *e)
{
    lv_area_t area; lv_obj_get_coords(star, &area);
    lv_layer_t *layer = lv_event_get_layer(e);
    int cx = (area.x1+area.x2)/2, cy = (area.y1+area.y2)/2;
    int size = 124 + (pulse < 50 ? pulse : 100-pulse)/4;
    lv_draw_triangle_dsc_t triangle; lv_draw_triangle_dsc_init(&triangle);
    triangle.color = lv_color_hex(colors[found < 5 ? found : 4]);
    triangle.opa = LV_OPA_COVER;
    for (unsigned i=0; i<10; ++i) {
        unsigned next = (i+1)%10;
        triangle.p[0] = (lv_point_precise_t){cx,cy};
        triangle.p[1] = (lv_point_precise_t){cx+(points[i].x-50)*size/100,cy+(points[i].y-50)*size/100};
        triangle.p[2] = (lv_point_precise_t){cx+(points[next].x-50)*size/100,cy+(points[next].y-50)*size/100};
        lv_draw_triangle(layer,&triangle);
    }
    lv_draw_line_dsc_t line; lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(0x394866); line.width = 5;
    line.round_start = line.round_end = 1;
    for (int x=-14; x<=14; x+=28) {
        line.p1 = (lv_point_precise_t){cx+x,cy-3};
        line.p2 = (lv_point_precise_t){cx+x,cy+1};
        lv_draw_line(layer,&line);
    }
    lv_draw_arc_dsc_t smile; lv_draw_arc_dsc_init(&smile);
    smile.center = (lv_point_t){cx,cy+1}; smile.radius = 16;
    smile.start_angle = 25; smile.end_angle = 155;
    smile.width = 3; smile.rounded = 1; smile.color = line.color;
    lv_draw_arc(layer,&smile);
}

static void render(void)
{
    bool done = found == 5;
    lv_label_set_text(heading, done ? "你是小星星！" : "点点小星星");
    lv_label_set_text(instruction, done ? "五颗星星，都找到啦" : "点一下星星，慢慢来");
    if (done) lv_label_set_text(progress,"休息一下眼睛吧");
    else lv_label_set_text_fmt(progress,"找到 %u / 5 颗星星",found);
    for (unsigned i=0; i<5; ++i)
        lv_obj_set_style_bg_color(dots[i],lv_color_hex(i<found ? colors[i] : 0xE4DFD6),0);
    lv_point_t p = done ? (lv_point_t){233,248} : positions[place];
    lv_obj_set_pos(star,p.x-72,p.y-72);
    lv_obj_invalidate(star);
}

static void pulse_frame(void *object, int32_t value)
{
    pulse = value; lv_obj_invalidate(object);
}

static void settle(void)
{
    pulse = 0; busy = false;
    /* Never present the next star under the same stationary finger. */
    unsigned next = (place + 1 + esp_random()%5)%6;
    while (abs(positions[next].x-positions[place].x)<=80 && abs(positions[next].y-positions[place].y)<=80)
        next=(next+1)%6;
    place=next;
    render();
}

static void completed(lv_anim_t *animation) { (void)animation; settle(); }

static void tap_star(lv_event_t *e)
{
    lv_indev_t *input = lv_indev_active();
    if (!input || !active || busy || found == 5) return;
    lv_point_t at; lv_indev_get_point(input,&at);
    if (lv_event_get_code(e)==LV_EVENT_PRESSED) { pressed=at; return; }
    if (lv_event_get_code(e)!=LV_EVENT_CLICKED || abs(at.x-pressed.x)>20 || abs(at.y-pressed.y)>20) return;
    ++found; busy=true;
    lv_label_set_text(instruction,found==5 ? "真棒，集齐啦！" : "找到啦！");
    lv_obj_set_style_bg_color(dots[found-1],lv_color_hex(colors[found-1]),0);
    lv_anim_t animation; lv_anim_init(&animation);
    lv_anim_set_var(&animation,star); lv_anim_set_exec_cb(&animation,pulse_frame);
    lv_anim_set_values(&animation,0,100); lv_anim_set_duration(&animation,360);
    lv_anim_set_completed_cb(&animation,completed); lv_anim_start(&animation);
}

static void restart(lv_event_t *e)
{
    (void)e;
    if (!active) return;
    lv_anim_delete(star,pulse_frame);
    found=0; busy=false; pulse=0; place=esp_random()%6; render();
}
static void home(lv_event_t *e) { (void)e; if (active) go_home(); }

static lv_obj_t *label(lv_obj_t *parent,const char *text,int y,const lv_font_t *font)
{
    lv_obj_t *obj=lv_label_create(parent);
    lv_label_set_text(obj,text); lv_obj_set_width(obj,330);
    lv_obj_set_style_text_font(obj,font,0);
    lv_obj_set_style_text_color(obj,lv_color_hex(0x31445F),0);
    lv_obj_set_style_text_align(obj,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_align(obj,LV_ALIGN_TOP_MID,0,y);
    return obj;
}

void star_game_create(lv_obj_t *screen,const lv_font_t *body,const lv_font_t *title,void (*callback)(void))
{
    go_home=callback;
    view=lv_obj_create(screen); lv_obj_remove_style_all(view);
    lv_obj_set_size(view,466,466); lv_obj_center(view);
    lv_obj_set_style_bg_color(view,lv_color_hex(0xFFF8E9),0);
    lv_obj_set_style_bg_opa(view,LV_OPA_COVER,0);
    lv_obj_remove_flag(view,LV_OBJ_FLAG_SCROLLABLE);
    heading=label(view,"",44,title); instruction=label(view,"",87,body);
    for (unsigned i=0;i<5;++i) {
        dots[i]=lv_obj_create(view); lv_obj_remove_style_all(dots[i]);
        lv_obj_set_size(dots[i],16,16); lv_obj_set_pos(dots[i],161+i*32,127);
        lv_obj_set_style_radius(dots[i],LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_opa(dots[i],LV_OPA_COVER,0);
        lv_obj_remove_flag(dots[i],LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    }
    star=lv_obj_create(view); lv_obj_remove_style_all(star);
    lv_obj_set_size(star,144,144); lv_obj_add_flag(star,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(star,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(star,draw_star,LV_EVENT_DRAW_MAIN,NULL);
    lv_obj_add_event_cb(star,tap_star,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(star,tap_star,LV_EVENT_CLICKED,NULL);
    progress=label(view,"",355,body);
    const char *names[]={"再玩一轮","回桌面"};
    for (int i=0;i<2;++i) {
        lv_obj_t *button=lv_button_create(view);
        lv_obj_set_size(button,112,50); lv_obj_set_pos(button,111+i*132,394);
        lv_obj_set_style_radius(button,25,0);
        lv_obj_set_style_shadow_width(button,0,0);
        lv_obj_set_style_bg_color(button,lv_color_hex(i?0xE3ECF7:0xFFE1A1),0);
        lv_obj_add_event_cb(button,i?home:restart,LV_EVENT_CLICKED,NULL);
        lv_obj_t *text=label(button,names[i],0,body);
        lv_obj_set_width(text,108); lv_obj_center(text);
    }
    render();
}

lv_obj_t *star_game_view(void) { return view; }
void star_game_set_active(bool enabled)
{
    active=enabled;
    if (!enabled && busy) { lv_anim_delete(star,pulse_frame); settle(); }
}
void star_game_debug(cJSON *root)
{
    cJSON_AddNumberToObject(root,"star_game_found",found);
    cJSON_AddBoolToObject(root,"star_game_busy",busy);
    cJSON_AddBoolToObject(root,"star_game_active",active);
    cJSON_AddNumberToObject(root,"star_game_x",lv_obj_get_x(star)+72);
    cJSON_AddNumberToObject(root,"star_game_y",lv_obj_get_y(star)+72);
}
