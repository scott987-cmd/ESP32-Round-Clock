#include "launcher.h"
#include <stdlib.h>

/* A single input surface owns the drag and tap. Icons never consume clicks. */
static lv_obj_t *tiles[5], *caption, *page_number;
static const char *const *app_names;
static unsigned selected, app_count;
static int offset, destination;
static bool dragging, moved, settling;
static lv_point_t press;
static launcher_open_cb_t open_app;
static void (*open_all)(void);
static const uint32_t fills[] = {
    0x147DF5, 0x16A6C4, 0xF3538C, 0xFF9743, 0x788498,
    0x8961E9, 0xF26A64, 0xC65AD9, 0x596DEB, 0x22B6A4,
    0x347BD1, 0xF1A52C, 0xEEA525, 0x329D9C, 0xDA7395, 0xEDAB73, 0x9B83D7, 0x57B899,
};
static const uint32_t gradients[] = {
    0x5AA9FF, 0x64D7DE, 0xFF8EB9, 0xFFC36D, 0xA2AFBF,
    0xB798FF, 0xFF9E98, 0xEC92F4, 0x91A6FF, 0x65D3B5,
    0x69A8F4, 0xFFD26B, 0xFFD86B, 0x73C8B8, 0xF2A8C1,
    0xFFC69C, 0xC9B0F6, 0x95D3AF,
};

typedef struct { lv_layer_t *layer; lv_area_t area; int size; } glyph_t;
static void stroke(glyph_t *g, int x1, int y1, int x2, int y2, int width)
{
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_white(); d.width = LV_MAX(2, width * g->size / 100);
    d.round_start = d.round_end = 1;
    d.p1.x = g->area.x1 + x1 * g->size / 100;
    d.p1.y = g->area.y1 + y1 * g->size / 100;
    d.p2.x = g->area.x1 + x2 * g->size / 100;
    d.p2.y = g->area.y1 + y2 * g->size / 100;
    lv_draw_line(g->layer, &d);
}
static void arc(glyph_t *g, int x, int y, int radius, int start, int end, int width)
{
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.color = lv_color_white(); d.width = LV_MAX(2, width * g->size / 100);
    d.center.x = g->area.x1 + x * g->size / 100;
    d.center.y = g->area.y1 + y * g->size / 100;
    d.radius = radius * g->size / 100;
    d.start_angle = start; d.end_angle = end; d.rounded = 1;
    lv_draw_arc(g->layer, &d);
}
static void icon_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    glyph_t g = {.layer = lv_event_get_layer(e), .size = lv_obj_get_width(obj)};
    lv_obj_get_coords(obj, &g.area);
    unsigned i = (unsigned)(uintptr_t)lv_obj_get_user_data(obj);
    switch (i) {
    case 0: /* Clock, with hands rather than a refresh arrow. */
        arc(&g,50,50,34,0,360,5); stroke(&g,50,50,50,29,5); stroke(&g,50,50,67,59,5); break;
    case 1:
        arc(&g,38,39,17,0,360,6);
        stroke(&g,38,12,38,16,4); stroke(&g,11,39,15,39,4);
        stroke(&g,18,19,22,23,4); stroke(&g,58,18,54,22,4);
        arc(&g,55,62,19,180,360,8); arc(&g,72,65,12,260,90,8);
        stroke(&g,30,76,73,76,8); arc(&g,32,65,12,90,270,8); break;
    case 2:
        arc(&g,50,50,34,0,360,5); stroke(&g,38,42,38,44,5); stroke(&g,62,42,62,44,5);
        arc(&g,50,52,18,20,160,5); break;
    case 3:
        stroke(&g,50,28,50,51,18); arc(&g,50,50,27,0,180,5);
        stroke(&g,50,77,50,85,5); stroke(&g,37,85,63,85,5); break;
    case 4:
        arc(&g,50,50,24,0,360,9); arc(&g,50,50,9,0,360,5);
        stroke(&g,50,15,50,23,9); stroke(&g,50,77,50,85,9);
        stroke(&g,15,50,23,50,9); stroke(&g,77,50,85,50,9);
        stroke(&g,25,25,31,31,9); stroke(&g,69,69,75,75,9);
        stroke(&g,25,75,31,69,9); stroke(&g,69,31,75,25,9); break;
    case 5:
        stroke(&g,26,74,26,51,12); stroke(&g,50,74,50,25,12);
        stroke(&g,74,74,74,39,12); break;
    case 6:
        stroke(&g,50,19,17,77,5); stroke(&g,17,77,83,77,5);
        stroke(&g,83,77,50,19,5); stroke(&g,50,40,50,54,5); stroke(&g,50,65,50,65,6); break;
    case 7:
        stroke(&g,40,27,75,19,6); stroke(&g,40,27,40,73,5); stroke(&g,75,19,75,65,5);
        stroke(&g,31,75,37,73,14); stroke(&g,66,67,72,65,14); break;
    case 8:
        stroke(&g,50,42,50,59,14); arc(&g,50,50,23,140,220,5);
        arc(&g,50,50,23,320,400,5); arc(&g,50,50,37,140,220,5);
        arc(&g,50,50,37,320,400,5); break;
    case 9: /* Wallpaper: little landscape and a live sparkle. */
        arc(&g,68,30,10,0,360,5);stroke(&g,20,74,42,47,5);stroke(&g,42,47,55,62,5);
        stroke(&g,55,62,70,42,5);stroke(&g,70,42,82,74,5);stroke(&g,20,75,82,75,5);
        stroke(&g,25,22,25,34,4);stroke(&g,19,28,31,28,4);break;
    case 10:
        stroke(&g,22,26,78,26,5); stroke(&g,22,26,22,66,5); stroke(&g,78,26,78,66,5);
        stroke(&g,22,66,78,66,5); stroke(&g,50,67,50,81,5); stroke(&g,36,82,64,82,5);
        stroke(&g,33,53,43,43,5); stroke(&g,43,43,53,54,5); stroke(&g,53,54,68,36,5); break;
    case 11:
        stroke(&g,25,21,25,80,5); stroke(&g,25,80,74,80,5); stroke(&g,74,80,74,21,5);
        stroke(&g,25,21,74,21,5); stroke(&g,37,37,63,37,5); stroke(&g,37,51,63,51,5);
        stroke(&g,37,65,53,65,5); break;
    case 12: /* A large friendly star, matching the game. */
        stroke(&g,50,13,62,37,5); stroke(&g,62,37,88,41,5);
        stroke(&g,88,41,69,60,5); stroke(&g,69,60,74,87,5);
        stroke(&g,74,87,50,74,5); stroke(&g,50,74,26,87,5);
        stroke(&g,26,87,31,60,5); stroke(&g,31,60,12,41,5);
        stroke(&g,12,41,38,37,5); stroke(&g,38,37,50,13,5);
        stroke(&g,40,49,40,51,4); stroke(&g,60,49,60,51,4);
        arc(&g,50,54,10,20,160,3); break;
    case 15: /* Pet face with ears. */
        arc(&g,27,29,11,0,360,5);arc(&g,73,29,11,0,360,5);arc(&g,50,55,32,0,360,5);
        stroke(&g,38,49,38,53,5);stroke(&g,62,49,62,53,5);arc(&g,50,57,12,15,165,4);break;
    case 16: /* Story book with a shooting-star bookmark. */
        stroke(&g,50,24,20,20,5);stroke(&g,20,20,20,75,5);stroke(&g,20,75,50,82,5);
        stroke(&g,50,24,80,20,5);stroke(&g,80,20,80,75,5);stroke(&g,80,75,50,82,5);
        stroke(&g,50,24,50,82,5);stroke(&g,60,34,65,45,4);stroke(&g,65,45,76,46,4);
        stroke(&g,76,46,68,53,4);stroke(&g,68,53,70,64,4);stroke(&g,70,64,60,58,4);break;
    case 17:
        stroke(&g,21,23,43,23,5);stroke(&g,43,23,43,77,5);stroke(&g,43,77,21,77,5);stroke(&g,21,77,21,23,5);
        stroke(&g,57,23,79,23,5);stroke(&g,79,23,79,77,5);stroke(&g,79,77,57,77,5);stroke(&g,57,77,57,23,5);
        arc(&g,32,50,6,0,360,4);arc(&g,68,50,6,0,360,4);break;
    case 13: /* Picture word card: a letter plus a picture dot. */
        stroke(&g,24,76,37,23,5);stroke(&g,37,23,50,76,5);stroke(&g,29,55,45,55,4);
        arc(&g,66,42,12,0,360,5);stroke(&g,54,72,78,72,5);break;
    case 18: /* Memory cards. */
        stroke(&g,22,25,44,25,5);stroke(&g,44,25,44,68,5);stroke(&g,44,68,22,68,5);stroke(&g,22,68,22,25,5);
        stroke(&g,56,33,78,33,5);stroke(&g,78,33,78,76,5);stroke(&g,78,76,56,76,5);stroke(&g,56,76,56,33,5);
        arc(&g,33,46,5,0,360,4);arc(&g,67,55,5,0,360,4);break;
    case 14: /* Stacked photo frames. */
        stroke(&g,22,30,22,80,5);stroke(&g,22,80,73,80,5);
        stroke(&g,30,20,82,20,5);stroke(&g,82,20,82,70,5);stroke(&g,82,70,30,70,5);stroke(&g,30,70,30,20,5);
        arc(&g,64,36,5,0,360,5);stroke(&g,35,62,48,47,5);stroke(&g,48,47,72,62,5);break;
    case 19: /* Open picture book. */
        stroke(&g,50,26,19,20,5); stroke(&g,19,20,19,73,5);
        stroke(&g,19,73,50,81,5); stroke(&g,50,81,81,73,5);
        stroke(&g,81,73,81,20,5); stroke(&g,81,20,50,26,5);
        stroke(&g,50,26,50,81,5);
        stroke(&g,27,59,34,38,4); stroke(&g,34,38,42,62,4); stroke(&g,29,53,40,55,3);
        arc(&g,65,42,7,0,360,5); stroke(&g,58,63,63,56,4); stroke(&g,63,56,73,64,4); break;
    default:
        stroke(&g,19,72,39,46,5); stroke(&g,39,46,54,63,5);
        stroke(&g,54,63,66,50,5); stroke(&g,66,50,83,73,5);
        stroke(&g,20,76,82,76,5); arc(&g,66,28,9,0,360,8); break;
    }
}
static void layout(int value)
{
    offset = value;
    for (int slot=0; slot<5; ++slot) {
        int position = (slot-2)*200 + offset;
        int size = LV_MAX(100, 224 - abs(position)*124/200);
        lv_obj_set_size(tiles[slot], size, size);
        lv_obj_set_pos(tiles[slot], 233 + position - size/2, 230-size/2);
    }
}
void launcher_decorate_icon(lv_obj_t *object, unsigned index)
{
    lv_obj_set_user_data(object, (void *)(uintptr_t)index);
    lv_obj_set_style_bg_color(object, lv_color_hex(fills[index % (sizeof(fills)/sizeof(fills[0]))]), 0);
    lv_obj_set_style_bg_grad_color(object, lv_color_hex(gradients[index % (sizeof(gradients)/sizeof(gradients[0]))]), 0);
    lv_obj_set_style_bg_grad_dir(object, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 2, 0);
    lv_obj_set_style_border_color(object, lv_color_white(), 0);
    lv_obj_set_style_border_opa(object, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(object, 14, 0);
    lv_obj_set_style_shadow_color(object, lv_color_hex(fills[index % (sizeof(fills)/sizeof(fills[0]))]), 0);
    lv_obj_set_style_shadow_opa(object, LV_OPA_30, 0);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(object, icon_draw, LV_EVENT_DRAW_MAIN, NULL);
}

static void refresh(void)
{
    for (int slot=0; slot<5; ++slot) {
        unsigned i=(selected+app_count+slot-2)%app_count;
        lv_obj_set_user_data(tiles[slot], (void *)(uintptr_t)i);
        lv_obj_set_style_bg_color(tiles[slot], lv_color_hex(fills[i%(sizeof(fills)/sizeof(fills[0]))]), 0);
        lv_obj_set_style_bg_grad_color(tiles[slot], lv_color_hex(gradients[i%(sizeof(gradients)/sizeof(gradients[0]))]), 0);
        lv_obj_set_style_bg_grad_dir(tiles[slot], LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_shadow_color(tiles[slot], lv_color_hex(fills[i%(sizeof(fills)/sizeof(fills[0]))]), 0);
        lv_obj_invalidate(tiles[slot]);
    }
    lv_label_set_text(caption, app_names[selected]);
    lv_label_set_text_fmt(page_number,"%u / %u",selected+1,app_count);
    layout(0);
}
static void animate(void *var, int32_t value) { (void)var; layout(value); }
static void settled(lv_anim_t *a)
{
    (void)a;
    if(destination) selected=(selected+app_count+(destination<0?1:-1))%app_count;
    settling=false; refresh();
}
static void settle(int target)
{
    destination=target; settling=true;
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a,tiles);
    lv_anim_set_values(&a,offset,target); lv_anim_set_duration(&a,160);
    lv_anim_set_exec_cb(&a,animate); lv_anim_set_path_cb(&a,lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a,settled); lv_anim_start(&a);
}
static void input(lv_event_t *e)
{
    lv_indev_t *indev=lv_indev_active();
    if(!indev) return;
    lv_point_t p; lv_indev_get_point(indev,&p);
    lv_event_code_t code=lv_event_get_code(e);
    if(code==LV_EVENT_PRESSED) {
        if(settling) return;
        press=p; dragging=true; moved=false;
    } else if(code==LV_EVENT_PRESSING && dragging) {
        int dx=p.x-press.x, dy=p.y-press.y;
        if(abs(dx)>12 || abs(dy)>12) moved=true;
        if(abs(dx)>abs(dy) && offset != LV_CLAMP(-200,dx,200)) layout(LV_CLAMP(-200,dx,200));
    } else if(code==LV_EVENT_RELEASED && dragging) {
        dragging=false;
        int dx=p.x-press.x, dy=p.y-press.y;
        if(dy < -60 && abs(dy)>abs(dx)) { layout(0); open_all(); }
        else if(moved) settle(abs(dx)>48 && abs(dx)>abs(dy)?(dx<0?-200:200):0);
        else if(p.y>100 && p.y<350) {
            if(p.x<110) settle(200);
            else if(p.x>356) settle(-200);
            else open_app(selected);
        }
    }
}
void launcher_create(lv_obj_t *parent,const lv_font_t *font,const char *const *names,
                     unsigned count,launcher_open_cb_t cb,void (*overview)(void))
{
    app_names=names; app_count=count; open_app=cb; open_all=overview;
    for(int slot=0;slot<5;++slot) {
        tiles[slot]=lv_obj_create(parent);
        lv_obj_remove_style_all(tiles[slot]);
        lv_obj_set_style_radius(tiles[slot],LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_opa(tiles[slot],LV_OPA_COVER,0);
        lv_obj_set_style_border_width(tiles[slot],2,0);
        lv_obj_set_style_border_color(tiles[slot],lv_color_white(),0);
        lv_obj_set_style_border_opa(tiles[slot],LV_OPA_50,0);
        /* Changing radius every drag frame defeats the software shadow cache.
         * Keep the gradient and crisp rim, without recomputing blurred masks. */
        lv_obj_set_style_shadow_width(tiles[slot],0,0);
        lv_obj_add_event_cb(tiles[slot],icon_draw,LV_EVENT_DRAW_MAIN,NULL);
        lv_obj_remove_flag(tiles[slot],LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    }
    caption=lv_label_create(parent); lv_obj_set_width(caption,320);
    lv_obj_set_style_text_font(caption,font,0); lv_obj_set_style_text_align(caption,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_align(caption,LV_ALIGN_TOP_MID,0,354);
    page_number=lv_label_create(parent);
    lv_obj_set_style_text_font(page_number,&lv_font_montserrat_16,0);
    lv_obj_align(page_number,LV_ALIGN_TOP_MID,0,402);
    lv_obj_t *surface=lv_obj_create(parent); lv_obj_remove_style_all(surface);
    lv_obj_set_size(surface,466,466); lv_obj_remove_flag(surface,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(surface,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(surface,input,LV_EVENT_ALL,NULL);
    refresh();
}
unsigned launcher_selected(void) { return selected; }
bool launcher_animating(void) { return settling || dragging; }
