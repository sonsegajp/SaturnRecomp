#include "game_overlay.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#endif
#define NK_IMPLEMENTATION
#include "game_overlay_internal.h"

#define OVERLAY_VERTICES (2u * 1024u * 1024u)
#define OVERLAY_INDICES  (512u * 1024u)
#define OVERLAY_COMMANDS 2048u

struct game_overlay {
    SDL_Window *window;
    Uint32 window_id;
    struct nk_context context;
    struct nk_font_atlas atlas;
    struct nk_draw_null_texture null_texture;
    struct nk_font *body, *heading, *caption;
    unsigned char *atlas_pixels;
    int atlas_width, atlas_height;
    struct nk_buffer converted;
    void *vertices, *indices;
    game_overlay_draw_command commands[OVERLAY_COMMANDS];
    game_overlay_draw_data draw;
    int open, tab, focus, focus_count, widget_id;
    int navigate, activate, adjust, tab_change;
    int width, height, resume_pending;
    float scale, input_x, input_y;
};

static const struct nk_color BG={16,21,27,255}, PANEL={25,35,45,255};
static const struct nk_color RAISED={37,51,63,255}, BORDER={52,71,83,255};
static const struct nk_color TEXT={238,243,245,255}, SECONDARY={173,189,199,255};
static const struct nk_color MUTED={131,154,168,255}, ACCENT={141,229,237,255};
static const struct nk_color SELECTED={35,68,80,255};

static void overlay_error(char *error, size_t size, const char *message) {
    if(error && size)snprintf(error,size,"%s",message);
}

static int readable_file(const char *path) {
    FILE *f=fopen(path,"rb");if(!f)return 0;fclose(f);return 1;
}

static const char *find_font(char *path, size_t size) {
#ifdef _WIN32
    char windows[MAX_PATH];
    if(GetWindowsDirectoryA(windows,sizeof windows)) {
        snprintf(path,size,"%s/Fonts/segoeui.ttf",windows);
        if(readable_file(path))return path;
    }
#else
    const char *fonts[]={"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf"};
    for(unsigned i=0;i<sizeof fonts/sizeof fonts[0];i++)
        if(readable_file(fonts[i]))return fonts[i];
#endif
    return NULL;
}

static void apply_style(game_overlay *o) {
    struct nk_context *c=&o->context;
    struct nk_color colors[NK_COLOR_COUNT];
    for(unsigned i=0;i<NK_COLOR_COUNT;i++)colors[i]=RAISED;
    colors[NK_COLOR_TEXT]=TEXT;
    colors[NK_COLOR_WINDOW]=PANEL;
    colors[NK_COLOR_HEADER]=BG;
    colors[NK_COLOR_BORDER]=BORDER;
    colors[NK_COLOR_BUTTON]=RAISED;
    colors[NK_COLOR_BUTTON_HOVER]=SELECTED;
    colors[NK_COLOR_BUTTON_ACTIVE]=ACCENT;
    colors[NK_COLOR_TOGGLE]=RAISED;
    colors[NK_COLOR_TOGGLE_HOVER]=SELECTED;
    colors[NK_COLOR_TOGGLE_CURSOR]=ACCENT;
    colors[NK_COLOR_SELECT]=RAISED;
    colors[NK_COLOR_SELECT_ACTIVE]=SELECTED;
    colors[NK_COLOR_SLIDER]=BG;
    colors[NK_COLOR_SLIDER_CURSOR]=ACCENT;
    colors[NK_COLOR_SLIDER_CURSOR_HOVER]=nk_rgb(193,248,250);
    colors[NK_COLOR_SLIDER_CURSOR_ACTIVE]=nk_rgb(91,191,201);
    colors[NK_COLOR_PROPERTY]=RAISED;
    colors[NK_COLOR_EDIT]=BG;
    colors[NK_COLOR_EDIT_CURSOR]=ACCENT;
    colors[NK_COLOR_COMBO]=RAISED;
    colors[NK_COLOR_CHART]=BG;
    colors[NK_COLOR_SCROLLBAR]=PANEL;
    colors[NK_COLOR_SCROLLBAR_CURSOR]=BORDER;
    colors[NK_COLOR_SCROLLBAR_CURSOR_HOVER]=MUTED;
    colors[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE]=ACCENT;
    colors[NK_COLOR_TAB_HEADER]=BG;
    nk_style_from_table(c,colors);
    c->style.window.rounding=8*o->scale;
    c->style.window.border=1;
    c->style.window.padding=nk_vec2(24*o->scale,18*o->scale);
    c->style.window.group_padding=nk_vec2(4*o->scale,4*o->scale);
    c->style.window.spacing=nk_vec2(12*o->scale,9*o->scale);
    c->style.window.scrollbar_size=nk_vec2(7*o->scale,7*o->scale);
    c->style.button.rounding=5*o->scale;
    c->style.button.border=1;
    c->style.button.padding=nk_vec2(10*o->scale,7*o->scale);
    c->style.button.text_active=BG;
    c->style.combo.rounding=5*o->scale;
    c->style.combo.border=1;
    c->style.combo.content_padding=nk_vec2(12*o->scale,7*o->scale);
    c->style.combo.button.padding=nk_vec2(9*o->scale,9*o->scale);
    c->style.combo.sym_normal=NK_SYMBOL_CHEVRON_DOWN;
    c->style.combo.sym_hover=NK_SYMBOL_CHEVRON_DOWN;
    c->style.combo.sym_active=NK_SYMBOL_CHEVRON_DOWN;
    c->style.checkbox.padding=nk_vec2(6*o->scale,4*o->scale);
    c->style.checkbox.spacing=10*o->scale;
    c->style.checkbox.border=1;
    c->style.slider.bar_height=4*o->scale;
    c->style.slider.rounding=3*o->scale;
    c->style.slider.cursor_size=nk_vec2(15*o->scale,15*o->scale);
    c->style.slider.padding=nk_vec2(8*o->scale,7*o->scale);
    c->style.property.rounding=5*o->scale;
    c->style.property.inc_button.padding=nk_vec2(5*o->scale,5*o->scale);
    c->style.property.dec_button.padding=nk_vec2(5*o->scale,5*o->scale);
}

game_overlay *game_overlay_create(SDL_Window *window,char *error,size_t error_size) {
    game_overlay *o=calloc(1,sizeof *o);
    if(!o){overlay_error(error,error_size,"Could not allocate game overlay");return NULL;}
    o->window=window;o->window_id=SDL_GetWindowID(window);o->scale=1;o->input_x=o->input_y=1;
    o->vertices=malloc(OVERLAY_VERTICES);o->indices=malloc(OVERLAY_INDICES);
    if(!o->vertices||!o->indices){overlay_error(error,error_size,"Could not allocate overlay draw buffers");game_overlay_destroy(o);return NULL;}
    nk_font_atlas_init_default(&o->atlas);nk_font_atlas_begin(&o->atlas);
    char font_path[1024];const char *font=find_font(font_path,sizeof font_path);
    /* Bake at 2x so normal/high-DPI windows share a sharp immutable atlas. */
    struct nk_font_config cfg=nk_font_config(36);
    cfg.oversample_h=2;cfg.oversample_v=2;
    o->body=font?nk_font_atlas_add_from_file(&o->atlas,font,36,&cfg):nk_font_atlas_add_default(&o->atlas,36,&cfg);
    cfg.size=48;
    o->heading=font?nk_font_atlas_add_from_file(&o->atlas,font,48,&cfg):nk_font_atlas_add_default(&o->atlas,48,&cfg);
    cfg.size=28;
    o->caption=font?nk_font_atlas_add_from_file(&o->atlas,font,28,&cfg):nk_font_atlas_add_default(&o->atlas,28,&cfg);
    const void *pixels=nk_font_atlas_bake(&o->atlas,&o->atlas_width,&o->atlas_height,NK_FONT_ATLAS_RGBA32);
    if(!pixels||!o->body||!o->heading||!o->caption){overlay_error(error,error_size,"Could not load overlay fonts");game_overlay_destroy(o);return NULL;}
    size_t bytes=(size_t)o->atlas_width*o->atlas_height*4;
    o->atlas_pixels=malloc(bytes);
    if(!o->atlas_pixels){overlay_error(error,error_size,"Could not allocate overlay font atlas");game_overlay_destroy(o);return NULL;}
    memcpy(o->atlas_pixels,pixels,bytes);
    nk_font_atlas_end(&o->atlas,nk_handle_id(1),&o->null_texture);
    nk_init_default(&o->context,&o->body->handle);
    nk_buffer_init_default(&o->converted);
    apply_style(o);
    return o;
}

void game_overlay_destroy(game_overlay *o) {
    if(!o)return;
    nk_free(&o->context);nk_buffer_free(&o->converted);nk_font_atlas_clear(&o->atlas);
    free(o->atlas_pixels);free(o->vertices);free(o->indices);free(o);
}
static void clear_interaction(game_overlay *o) {
    /* Releases can arrive while the panel is closed or another window has
     * focus. Do not carry a pressed button, drag or text editor into the next
     * interaction. Keep the pointer position until SDL sends another motion. */
    struct nk_vec2 position=o->context.input.mouse.pos;
    memset(&o->context.input,0,sizeof o->context.input);
    o->context.input.mouse.pos=o->context.input.mouse.prev=position;
    o->context.text_edit.active=0;
    for(struct nk_window *window=o->context.begin;window;window=window->next) {
        window->property.active=0;
        window->edit.active=0;
    }
    o->navigate=o->activate=o->adjust=o->tab_change=0;
}
void game_overlay_set_open(game_overlay *o,int open) {
    if(!o)return;
    if(o->open!=(open!=0))clear_interaction(o);
    o->open=open!=0;
    if(o->open){o->focus=0;SDL_StartTextInput();SDL_ShowCursor(SDL_ENABLE);}
    else SDL_StopTextInput();
}
void game_overlay_toggle(game_overlay *o){if(o)game_overlay_set_open(o,!o->open);}
int game_overlay_is_open(const game_overlay *o){return o&&o->open;}
const unsigned char *game_overlay_font_atlas(const game_overlay *o,int *width,int *height){
    if(!o)return NULL;
    if(width)*width=o->atlas_width;
    if(height)*height=o->atlas_height;
    return o->atlas_pixels;
}

void game_overlay_input_begin(game_overlay *o) {
    if(!o)return;
    o->navigate=o->activate=o->adjust=o->tab_change=0;
    nk_input_begin(&o->context);
    int w=0,h=0,dw=0,dh=0;
    SDL_GetWindowSize(o->window,&w,&h);SDL_Vulkan_GetDrawableSize(o->window,&dw,&dh);
    o->input_x=w>0&&dw>0?(float)dw/w:1;o->input_y=h>0&&dh>0?(float)dh/h:1;
}
int game_overlay_event(game_overlay *o,const SDL_Event *e) {
    if(!o||!e)return 0;
    Uint32 event_window=0;
    switch(e->type) {
    case SDL_KEYDOWN:case SDL_KEYUP:event_window=e->key.windowID;break;
    case SDL_TEXTINPUT:event_window=e->text.windowID;break;
    case SDL_TEXTEDITING:event_window=e->edit.windowID;break;
    case SDL_MOUSEMOTION:event_window=e->motion.windowID;break;
    case SDL_MOUSEBUTTONDOWN:case SDL_MOUSEBUTTONUP:event_window=e->button.windowID;break;
    case SDL_MOUSEWHEEL:event_window=e->wheel.windowID;break;
    case SDL_WINDOWEVENT:event_window=e->window.windowID;break;
    default:break;
    }
    /* A zero ID is permitted for synthetic, directly supplied events. Real
     * SDL window events must belong to the game, never its Diagnostics view. */
    if(event_window && event_window!=o->window_id)return 0;
    if(e->type==SDL_WINDOWEVENT && e->window.event==SDL_WINDOWEVENT_FOCUS_LOST) {
        clear_interaction(o);return o->open;
    }
    if(e->type==SDL_KEYDOWN && !e->key.repeat && e->key.keysym.sym==SDLK_F1){game_overlay_toggle(o);return 1;}
    if((e->type==SDL_KEYDOWN||e->type==SDL_KEYUP) && e->key.keysym.sym==SDLK_F2)return 0;
    if(!o->open)return 0;
    struct nk_context *c=&o->context;
    switch(e->type){
    case SDL_KEYDOWN:
    case SDL_KEYUP:{
        int down=e->type==SDL_KEYDOWN;SDL_Keycode key=e->key.keysym.sym;
        if(down && key==SDLK_ESCAPE){game_overlay_set_open(o,0);o->resume_pending=1;return 1;}
        if(down&&!c->text_edit.active){
            if(key==SDLK_TAB)o->navigate+=(e->key.keysym.mod&KMOD_SHIFT)?-1:1;
            else if(key==SDLK_DOWN)o->navigate++;
            else if(key==SDLK_UP)o->navigate--;
            else if(key==SDLK_LEFT)o->adjust--;
            else if(key==SDLK_RIGHT)o->adjust++;
            else if(key==SDLK_RETURN||key==SDLK_KP_ENTER||key==SDLK_SPACE)o->activate=1;
            else if(key==SDLK_PAGEUP)o->tab_change--;
            else if(key==SDLK_PAGEDOWN)o->tab_change++;
        }
        nk_input_key(c,NK_KEY_SHIFT,(e->key.keysym.mod&KMOD_SHIFT)!=0);
        if(key==SDLK_RETURN||key==SDLK_KP_ENTER)nk_input_key(c,NK_KEY_ENTER,down);
        if(key==SDLK_BACKSPACE)nk_input_key(c,NK_KEY_BACKSPACE,down);
        if(key==SDLK_DELETE)nk_input_key(c,NK_KEY_DEL,down);
        if(key==SDLK_LEFT)nk_input_key(c,NK_KEY_LEFT,down);
        if(key==SDLK_RIGHT)nk_input_key(c,NK_KEY_RIGHT,down);
        if(key==SDLK_HOME)nk_input_key(c,NK_KEY_TEXT_START,down);
        if(key==SDLK_END)nk_input_key(c,NK_KEY_TEXT_END,down);
        if(key==SDLK_a&&(e->key.keysym.mod&KMOD_CTRL))nk_input_key(c,NK_KEY_TEXT_SELECT_ALL,down);
        return 1;
    }
    case SDL_TEXTINPUT:{
        const char *text=e->text.text;int remaining=(int)strlen(text);
        while(remaining>0){nk_rune rune;int used=nk_utf_decode(text,&rune,remaining);if(used<=0)break;nk_input_unicode(c,rune);text+=used;remaining-=used;}
        return 1;
    }
    case SDL_MOUSEMOTION:nk_input_motion(c,(int)(e->motion.x*o->input_x),(int)(e->motion.y*o->input_y));return 1;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:{
        enum nk_buttons b=e->button.button==SDL_BUTTON_LEFT?NK_BUTTON_LEFT:e->button.button==SDL_BUTTON_RIGHT?NK_BUTTON_RIGHT:NK_BUTTON_MIDDLE;
        nk_input_button(c,b,(int)(e->button.x*o->input_x),(int)(e->button.y*o->input_y),e->type==SDL_MOUSEBUTTONDOWN);return 1;
    }
    case SDL_MOUSEWHEEL:nk_input_scroll(c,nk_vec2((float)e->wheel.x,(float)e->wheel.y));return 1;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    case SDL_CONTROLLERAXISMOTION:return 1;
    default:return 0;
    }
}
void game_overlay_input_end(game_overlay *o){if(o)nk_input_end(&o->context);}

static void body_font(game_overlay *o){nk_style_set_font(&o->context,&o->body->handle);}
static void caption(game_overlay *o,const char *text) {
    nk_style_set_font(&o->context,&o->caption->handle);
    nk_layout_row_dynamic(&o->context,20*o->scale,1);
    nk_label_colored(&o->context,text,NK_TEXT_LEFT,SECONDARY);body_font(o);
}
static void section(game_overlay *o,const char *text,const char *description) {
    nk_layout_row_dynamic(&o->context,29*o->scale,1);
    nk_label_colored(&o->context,text,NK_TEXT_LEFT,TEXT);
    if(description)caption(o,description);
}
static void label_row(game_overlay *o,const char *label) {
    static const float widths[2]={.43f,.57f};
    nk_layout_row(&o->context,NK_DYNAMIC,38*o->scale,2,widths);
    nk_label_colored(&o->context,label,NK_TEXT_LEFT,SECONDARY);
}
static int focus_begin(game_overlay *o,struct nk_rect *bounds) {
    int id=o->widget_id++;
    *bounds=nk_widget_bounds(&o->context);
    if(nk_input_is_mouse_hovering_rect(&o->context.input,*bounds) && nk_input_is_mouse_pressed(&o->context.input,NK_BUTTON_LEFT))o->focus=id;
    return o->focus==id;
}
static void focus_end(game_overlay *o,struct nk_rect bounds,int focused) {
    if(focused)nk_stroke_rect(nk_window_get_canvas(&o->context),bounds,5*o->scale,1.5f*o->scale,ACCENT);
}
static int choice(game_overlay *o,const char *label,const char *const *items,int count,int value,int enabled) {
    label_row(o,label);struct nk_rect rect;int focused=focus_begin(o,&rect);
    if(!enabled)nk_widget_disable_begin(&o->context);
    int next=nk_combo(&o->context,items,count,value,(int)(32*o->scale),nk_vec2(rect.w,32*o->scale*(count+1)));
    if(enabled&&focused&&(o->adjust||o->activate))next=(value+(o->adjust?o->adjust:1)+count)%count;
    if(!enabled)nk_widget_disable_end(&o->context);
    focus_end(o,rect,focused&&enabled);return enabled?next:value;
}
static int toggle(game_overlay *o,const char *label,int value,int enabled) {
    label_row(o,label);struct nk_rect rect;int focused=focus_begin(o,&rect);nk_bool state=value!=0;
    if(!enabled)nk_widget_disable_begin(&o->context);
    nk_checkbox_label(&o->context,state?"Enabled":"Disabled",&state);
    if(enabled&&focused&&(o->activate||o->adjust))state=!value;
    if(!enabled)nk_widget_disable_end(&o->context);
    focus_end(o,rect,focused&&enabled);return enabled?state:value;
}
static int number(game_overlay *o,const char *label,int value,int low,int high,int step,int enabled) {
    label_row(o,label);struct nk_rect rect;int focused=focus_begin(o,&rect);
    if(!enabled)nk_widget_disable_begin(&o->context);
    int result=value;
    nk_property_int(&o->context,"",low,&result,high,step,(float)step);
    if(enabled&&focused&&o->adjust)result=value+o->adjust*step;
    if(result<low)result=low;
    if(result>high)result=high;
    if(!enabled)nk_widget_disable_end(&o->context);
    focus_end(o,rect,focused&&enabled);return enabled?result:value;
}
static int button(game_overlay *o,const char *text) {
    struct nk_rect rect;int focused=focus_begin(o,&rect);
    int pressed=nk_button_label(&o->context,text)||(focused&&o->activate);
    focus_end(o,rect,focused);return pressed;
}
static void unavailable(game_overlay *o,unsigned caps,unsigned bit) {
    if(!(caps&bit))caption(o,"This option is unavailable with the active renderer.");
}
static void mark_changes(game_overlay_actions *a,const saturn_runtime_settings *old) {
#define CHANGED(field,bit) if(a->settings.field!=old->field)a->changed|=bit
    CHANGED(window_width,GAME_OVERLAY_WINDOW);CHANGED(window_height,GAME_OVERLAY_WINDOW);
    CHANGED(fullscreen,GAME_OVERLAY_FULLSCREEN);CHANGED(internal_scale,GAME_OVERLAY_INTERNAL_SCALE);
    CHANGED(texture_filter,GAME_OVERLAY_TEXTURE_FILTER);CHANGED(antialiasing,GAME_OVERLAY_ANTIALIASING);
    CHANGED(model_smoothing,GAME_OVERLAY_MODEL_SMOOTHING);CHANGED(interpolation,GAME_OVERLAY_INTERPOLATION);
    CHANGED(target_hz,GAME_OVERLAY_TARGET_HZ);CHANGED(volume,GAME_OVERLAY_VOLUME);CHANGED(muted,GAME_OVERLAY_MUTED);
#undef CHANGED
}

const game_overlay_draw_data *game_overlay_build(game_overlay *o,const game_overlay_view *v,
    game_overlay_actions *a,int width,int height,float dpi) {
    if(!o||!v||!a)return NULL;
    memset(a,0,sizeof *a);a->settings=v->settings;a->resume=o->resume_pending;o->resume_pending=0;
    memset(&o->draw,0,sizeof o->draw);
    if(!o->open){nk_clear(&o->context);return &o->draw;}
    o->width=width;o->height=height;
    if(dpi<.8f)dpi=.8f;
    if(dpi>2.5f)dpi=2.5f;
    float fit=fminf((width-24.f)/780.f,(height-24.f)/620.f);
    o->scale=fminf(dpi,fit);if(o->scale<.65f)o->scale=.65f;
    o->body->handle.height=20*o->scale;o->heading->handle.height=28*o->scale;o->caption->handle.height=16*o->scale;
    apply_style(o);body_font(o);
    if(o->focus_count>0)o->focus=(o->focus+o->navigate%o->focus_count+o->focus_count)%o->focus_count;
    if(o->tab_change){o->tab=(o->tab+o->tab_change%4+4)%4;o->focus=0;}
    o->widget_id=0;
    const float panel_w=780*o->scale,panel_h=620*o->scale;
    struct nk_rect panel=nk_rect((width-panel_w)/2,(height-panel_h)/2,panel_w,panel_h);
    struct nk_style_item old_background=o->context.style.window.fixed_background;
    struct nk_vec2 old_padding=o->context.style.window.padding;
    o->context.style.window.fixed_background=nk_style_item_color(nk_rgba(4,8,12,175));
    o->context.style.window.padding=nk_vec2(0,0);
    if(nk_begin(&o->context,"Overlay shade",nk_rect(0,0,(float)width,(float)height),NK_WINDOW_NO_SCROLLBAR|NK_WINDOW_BACKGROUND)){}
    nk_end(&o->context);
    o->context.style.window.fixed_background=old_background;o->context.style.window.padding=old_padding;
    if(nk_begin(&o->context,"Game settings",panel,NK_WINDOW_BORDER|NK_WINDOW_NO_SCROLLBAR)) {
        nk_style_set_font(&o->context,&o->caption->handle);
        nk_layout_row_dynamic(&o->context,18*o->scale,1);
        nk_label_colored(&o->context,"SATURNRECOMP  /  IN-GAME SETTINGS",NK_TEXT_LEFT,ACCENT);
        nk_style_set_font(&o->context,&o->heading->handle);
        nk_layout_row_dynamic(&o->context,37*o->scale,1);
        nk_label(&o->context,"Game settings",NK_TEXT_LEFT);body_font(o);
        caption(o,v->game_title&&*v->game_title?v->game_title:"Settings apply to every game in your library.");
        nk_layout_row_dynamic(&o->context,36*o->scale,4);
        const char *tabs[]={"Display","Graphics","Motion","Audio"};
        for(int i=0;i<4;i++) {
            struct nk_style_item normal=o->context.style.button.normal;
            if(i==o->tab)o->context.style.button.normal=nk_style_item_color(SELECTED);
            if(button(o,tabs[i])){o->tab=i;o->focus=i;}
            o->context.style.button.normal=normal;
        }
        nk_layout_row_dynamic(&o->context,4*o->scale,1);nk_spacing(&o->context,1);
        nk_layout_row_dynamic(&o->context,320*o->scale,1);
        if(nk_group_begin(&o->context,"Settings content",NK_WINDOW_NO_SCROLLBAR)) {
            saturn_runtime_settings *s=&a->settings;unsigned caps=v->capabilities;
            char text[256];
            if(o->tab==0) {
                section(o,"Display","Choose how the game fits your screen.");
                const char *modes[]={"Windowed","Borderless fullscreen"};
                s->fullscreen=choice(o,"Window mode",modes,2,s->fullscreen!=0,caps&GAME_OVERLAY_FULLSCREEN);
                char labels[32][48];const char *items[32];int count=0,selected=0;
                if(v->resolutions)for(unsigned i=0;i<v->resolution_count&&count<31;i++) {
                    int w=v->resolutions[i].width,h=v->resolutions[i].height;
                    if(w<320||h<240)continue;
                    snprintf(labels[count],sizeof labels[count],"%d x %d",w,h);items[count]=labels[count];
                    if(w==s->window_width&&h==s->window_height)selected=count;
                    count++;
                }
                int found=0;
                for(int i=0;i<count;i++){int w=0,h=0;sscanf(items[i],"%d x %d",&w,&h);if(w==s->window_width&&h==s->window_height)found=1;}
                if(!found||!count){snprintf(labels[count],sizeof labels[count],"%d x %d",s->window_width,s->window_height);items[count]=labels[count];selected=count++;}
                int next=choice(o,"Window resolution",items,count,selected,!s->fullscreen&&(caps&GAME_OVERLAY_WINDOW));
                if(next!=selected)sscanf(items[next],"%d x %d",&s->window_width,&s->window_height);
                caption(o,s->fullscreen?"Fullscreen uses your desktop resolution.":"The game keeps its original 4:3 display proportions.");
                snprintf(text,sizeof text,"Game output  %d x %d     Display  %d x %d",v->native_width,v->native_height,width,height);caption(o,text);
                unavailable(o,caps,GAME_OVERLAY_FULLSCREEN);
            } else if(o->tab==1) {
                section(o,"Graphics","Sharper rendering, with the original game intact.");
                const char *scales[]={"Native (1x)","2x","3x","4x"};
                int scale=s->internal_scale;if(scale<1)scale=1;if(scale>4)scale=4;
                s->internal_scale=choice(o,"Internal resolution",scales,4,scale-1,caps&GAME_OVERLAY_INTERNAL_SCALE)+1;
                snprintf(text,sizeof text,"Render size  %d x %d  |  Higher scales use more GPU time.",v->native_width*s->internal_scale,v->native_height*s->internal_scale);caption(o,text);
                const char *filters[]={"Nearest / original pixels","Bilinear / smooth textures"};
                s->texture_filter=choice(o,"Texture filtering",filters,2,s->texture_filter!=0,caps&GAME_OVERLAY_TEXTURE_FILTER);
                const char *aa[]={"Off","FXAA"};
                s->antialiasing=choice(o,"Anti-aliasing",aa,2,s->antialiasing!=0,caps&GAME_OVERLAY_ANTIALIASING);
                s->model_smoothing=toggle(o,"Smooth model edges",s->model_smoothing,caps&GAME_OVERLAY_MODEL_SMOOTHING);
                caption(o,"Supersampled polygon edges. Original models and textures are preserved.");
            } else if(o->tab==2) {
                section(o,"Frame interpolation","Smoother motion between the game's original frames.");
                s->interpolation=toggle(o,"Frame interpolation",s->interpolation,caps&GAME_OVERLAY_INTERPOLATION);
                s->target_hz=number(o,"Target refresh rate",s->target_hz,60,240,1,caps&GAME_OVERLAY_TARGET_HZ);
                caption(o,"60-240 Hz. F2 switches interpolation on or off at any time.");
                snprintf(text,sizeof text,"Game  %.1f FPS     Presentation  %.1f FPS",v->game_fps,v->present_fps);caption(o,text);
                caption(o,"Original frames are kept when an in-between picture cannot be rebuilt reliably.");
                unavailable(o,caps,GAME_OVERLAY_INTERPOLATION);
            } else {
                section(o,"Audio","Control playback volume without changing game audio timing.");
                label_row(o,"Master volume");struct nk_rect rect;int focused=focus_begin(o,&rect);
                int enabled=(caps&GAME_OVERLAY_VOLUME)!=0;
                if(!enabled)nk_widget_disable_begin(&o->context);
                nk_slider_int(&o->context,0,&s->volume,100,1);
                if(enabled&&nk_input_is_mouse_pressed(&o->context.input,NK_BUTTON_LEFT)&&
                    nk_input_is_mouse_hovering_rect(&o->context.input,rect)) {
                    float inset=o->context.style.slider.padding.x+o->context.style.slider.cursor_size.x*.5f;
                    float track=rect.w-2*inset;
                    if(track>0)s->volume=(int)roundf(100*(o->context.input.mouse.pos.x-rect.x-inset)/track);
                }
                if(enabled&&focused&&o->adjust)s->volume+=o->adjust*5;
                if(s->volume<0)s->volume=0;
                if(s->volume>100)s->volume=100;
                if(!enabled)nk_widget_disable_end(&o->context);
                focus_end(o,rect,focused&&enabled);
                snprintf(text,sizeof text,"%d%%",s->volume);caption(o,text);
                s->muted=toggle(o,"Mute",s->muted,caps&GAME_OVERLAY_MUTED);
                caption(o,"Volume and mute apply to every game's host playback.");
            }
            nk_group_end(&o->context);
        }
        nk_layout_row_dynamic(&o->context,38*o->scale,3);
        if(button(o,"Resume game")){game_overlay_set_open(o,0);a->resume=1;}
        if(button(o,"Diagnostics")){a->diagnostics=1;}
        if(button(o,"Reset this page")) {
            saturn_runtime_settings defaults;
            saturn_settings_defaults(&defaults);
            if(o->tab==0){a->settings.window_width=defaults.window_width;a->settings.window_height=defaults.window_height;a->settings.fullscreen=defaults.fullscreen;}
            if(o->tab==1){a->settings.internal_scale=defaults.internal_scale;a->settings.texture_filter=defaults.texture_filter;a->settings.antialiasing=defaults.antialiasing;a->settings.model_smoothing=defaults.model_smoothing;}
            if(o->tab==2){a->settings.interpolation=defaults.interpolation;a->settings.target_hz=defaults.target_hz;}
            if(o->tab==3){a->settings.volume=defaults.volume;a->settings.muted=defaults.muted;}
        }
        nk_style_set_font(&o->context,&o->caption->handle);
        nk_layout_row_dynamic(&o->context,20*o->scale,1);
        nk_label_colored(&o->context,"F1 / Esc  Close     Tab  Move focus     Arrow keys  Adjust     PgUp / PgDn  Pages",NK_TEXT_LEFT,MUTED);
        body_font(o);
    }
    nk_end(&o->context);
    o->focus_count=o->widget_id;if(o->focus_count&&o->focus>=o->focus_count)o->focus=o->focus_count-1;
    mark_changes(a,&v->settings);a->changed&=v->capabilities;
    /* The action snapshot is safe to apply in full, including page resets when
     * only a subset of controls is supported by the active renderer. */
#define KEEP_UNAVAILABLE(field,bit) if(!(v->capabilities&(bit)))a->settings.field=v->settings.field
    KEEP_UNAVAILABLE(window_width,GAME_OVERLAY_WINDOW);KEEP_UNAVAILABLE(window_height,GAME_OVERLAY_WINDOW);
    KEEP_UNAVAILABLE(fullscreen,GAME_OVERLAY_FULLSCREEN);KEEP_UNAVAILABLE(internal_scale,GAME_OVERLAY_INTERNAL_SCALE);
    KEEP_UNAVAILABLE(texture_filter,GAME_OVERLAY_TEXTURE_FILTER);KEEP_UNAVAILABLE(antialiasing,GAME_OVERLAY_ANTIALIASING);
    KEEP_UNAVAILABLE(model_smoothing,GAME_OVERLAY_MODEL_SMOOTHING);KEEP_UNAVAILABLE(interpolation,GAME_OVERLAY_INTERPOLATION);
    KEEP_UNAVAILABLE(target_hz,GAME_OVERLAY_TARGET_HZ);KEEP_UNAVAILABLE(volume,GAME_OVERLAY_VOLUME);KEEP_UNAVAILABLE(muted,GAME_OVERLAY_MUTED);
#undef KEEP_UNAVAILABLE
    nk_buffer_clear(&o->converted);
    struct nk_buffer vertices,indices;
    nk_buffer_init_fixed(&vertices,o->vertices,OVERLAY_VERTICES);
    nk_buffer_init_fixed(&indices,o->indices,OVERLAY_INDICES);
    static const struct nk_draw_vertex_layout_element layout[]={
        {NK_VERTEX_POSITION,NK_FORMAT_FLOAT,offsetof(game_overlay_vertex,position)},
        {NK_VERTEX_TEXCOORD,NK_FORMAT_FLOAT,offsetof(game_overlay_vertex,uv)},
        {NK_VERTEX_COLOR,NK_FORMAT_R8G8B8A8,offsetof(game_overlay_vertex,color)},
        {NK_VERTEX_LAYOUT_END}
    };
    struct nk_convert_config config;memset(&config,0,sizeof config);
    config.vertex_layout=layout;config.vertex_size=sizeof(game_overlay_vertex);config.vertex_alignment=NK_ALIGNOF(game_overlay_vertex);
    config.tex_null=o->null_texture;config.circle_segment_count=22;config.curve_segment_count=22;config.arc_segment_count=22;
    config.global_alpha=1;config.shape_AA=NK_ANTI_ALIASING_ON;config.line_AA=NK_ANTI_ALIASING_ON;
    enum nk_convert_result result=nk_convert(&o->context,&o->converted,&vertices,&indices,&config);
    if(result==NK_CONVERT_SUCCESS) {
        unsigned count=0,offset=0;const struct nk_draw_command *command;
        nk_draw_foreach(command,&o->context,&o->converted) {
            if(!command->elem_count)continue;
            if(count>=OVERLAY_COMMANDS)break;
            o->commands[count++]=(game_overlay_draw_command){command->elem_count,offset,command->clip_rect.x,command->clip_rect.y,command->clip_rect.w,command->clip_rect.h};
            offset+=command->elem_count;
        }
        o->draw.vertices=o->vertices;o->draw.indices=o->indices;o->draw.commands=o->commands;
        o->draw.vertex_bytes=vertices.allocated;o->draw.index_bytes=indices.allocated;o->draw.command_count=count;
        o->draw.width=width;o->draw.height=height;
    }
    nk_clear(&o->context);return &o->draw;
}
