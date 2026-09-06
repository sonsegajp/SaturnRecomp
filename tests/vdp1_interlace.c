/* FBCR double-interlace regression: real command walking and CPU raster,
 * optionally the actual Vulkan raster/compositor. No game data required.
 * Build the GPU variant with tools/test_vdp1_interlace.ps1. */
#define SDL_MAIN_HANDLED
#ifdef SATURN_TEST_VULKAN
#include "../runner/src/vulkan_renderer.c"
#else
#include "saturn.h"
#include "geometry_interp.h"
#endif
#include <stdio.h>
#include <string.h>

static saturn machine;
static unsigned checks;
static uint16_t expected[512*256];
static char error_text[512];
static saturn_vk_vdp1_op captured[16];
static unsigned captured_count;
#define REQUIRE(c, m) do { checks++; if(!(c)) { \
    fprintf(stderr,"FAIL %s: %s\n",m,error_text); return 0; } } while(0)

typedef struct {
    const char *name;
    unsigned fbcr,tvmd,pmod,system_y;
    int top;
} fixture;
static const fixture cases[]={
    {"even rows above 255",8,0xc0,0,447,300},
    {"odd rows above 255",12,0xc0,0,447,300},
    {"even mesh uses draw parity",8,0xc0,0x100,447,300},
    {"odd mesh uses draw parity",12,0xc0,0x100,447,300},
    {"odd system clip boundary",8,0xc0,0,305,300},
    {"odd field clipped boundary",12,0xc0,0,305,300},
    {"inside user clip in draw coordinates",8,0xc0,0x400,447,300},
    {"outside user clip in draw coordinates",12,0xc0,0x600,447,300},
    {"field-selected half transparency",12,0xc0,3,447,300},
    {"DIE outside double density aliases both rows",8,0,3,447,300},
    {"DIL ignored outside double density",12,0,3,447,300},
    {"DIE off clips high coordinates",0,0xc0,0,447,300},
    {"DIE off preserves ordinary coordinates",4,0xc0,0,447,20}
};
static void word(unsigned a,unsigned value) {
    machine.vdp1_vram[a]=(uint8_t)(value>>8);
    machine.vdp1_vram[a+1]=(uint8_t)value;
}
static void basic_command(unsigned a,unsigned type) { word(a,type);word(a+4,0); }
static unsigned texel_row(unsigned row) { return 0x8001u|(row<<5); }
static uint16_t blend(unsigned src,unsigned dst) {
    return (uint16_t)(0x8000u|(((src&31)+(dst&31))>>1)|
        (((((src>>5)&31)+((dst>>5)&31))>>1)<<5)|
        (((((src>>10)&31)+((dst>>10)&31))>>1)<<10));
}
static int capture(void *unused,const saturn_vk_vdp1_op *op) {
    (void)unused;if(captured_count==16)return 0;
    captured[captured_count++]=*op;return 1;
}
static void prepare(const fixture *f) {
    memset(machine.vdp1_vram,0,VDP1_VRAM_SZ);
    machine.vdp1_reg[1]=(uint16_t)f->fbcr;
    machine.vdp2_reg[0]=(uint16_t)f->tvmd;
    machine.vdp1_ew_val=0xc631;machine.vdp1_ew_x1=machine.vdp1_ew_y1=0;
    machine.vdp1_ew_x3=512;machine.vdp1_ew_y3=255;
    machine.fb_draw=1;vdp1_erase(&machine);machine.fb_draw=0;
    basic_command(0,9);word(0x14,511);word(0x16,f->system_y);
    basic_command(0x20,8);word(0x2c,21);word(0x2e,f->top+1);
    word(0x34,26);word(0x36,f->top+5);
    basic_command(0x40,10);word(0x4c,0);word(0x4e,224);
    basic_command(0x60,0);word(0x64,(5u<<3)|f->pmod);
    word(0x68,0x10000/8);word(0x6a,0x108);
    word(0x6c,20);word(0x6e,(unsigned)(f->top-224));
    /* Horizontal one-dot lines cover the separate line operation path. */
    for(unsigned i=0;i<2;i++) {
        unsigned a=0x80+i*0x20;basic_command(a,6);word(a+6,0xfc00u|(i<<5));
        word(a+0xc,40);word(a+0xe,(unsigned)(f->top-224+1+i));
        word(a+0x10,47);word(a+0x12,(unsigned)(f->top-224+1+i));
    }
    word(0xc0,0x8000);
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++)
        word(0x10000+(y*8+x)*2,texel_row(y));
}
static void reference(const fixture *f) {
    for(unsigned i=0;i<512*256;i++)expected[i]=0xc631;
    for(unsigned row=0;row<8;row++)for(int x=20;x<=27;x++) {
        int draw_y=f->top+(int)row;
        if(draw_y<0||draw_y>(int)f->system_y)continue;
        int outside=x<21||x>26||row<1||row>5;
        if((f->pmod&0x400)&&outside!=((f->pmod>>9)&1))continue;
        if((f->pmod&0x100)&&((x+draw_y)&1))continue;
        if((f->fbcr&8)&&(f->tvmd&0xc0)==0xc0&&(draw_y&1)!=((f->fbcr>>2)&1))continue;
        int y=(f->fbcr&8)?draw_y/2:draw_y;
        if(y>=256)continue;
        unsigned at=(unsigned)y*512+x,colour=texel_row(row);
        expected[at]=(f->pmod&3)==3?blend(colour,expected[at]):(uint16_t)colour;
    }
    for(unsigned i=0;i<2;i++)for(unsigned x=40;x<=47;x++) {
        int draw_y=f->top+1+(int)i;
        if(draw_y<0||draw_y>(int)f->system_y)continue;
        if((f->fbcr&8)&&(f->tvmd&0xc0)==0xc0&&(draw_y&1)!=((f->fbcr>>2)&1))continue;
        int y=(f->fbcr&8)?draw_y/2:draw_y;
        if(y<256)expected[y*512+x]=(uint16_t)(0xfc00u|(i<<5));
    }
}
static int compare_cpu(const fixture *f) {
    for(unsigned i=0;i<512*256;i++) {
        unsigned actual=(machine.vdp1_fb[0][i*2]<<8)|machine.vdp1_fb[0][i*2+1];
        if(actual!=expected[i]) {
            snprintf(error_text,sizeof error_text,"%s at %u,%u: got %04X expected %04X",f->name,i%512,i/512,actual,expected[i]);
            REQUIRE(0,"CPU draw-space/field mapping");
        }
    }
    REQUIRE(1,"complete CPU framebuffer matches independent row reference");return 1;
}
static int cpu_tests(void) {
    for(unsigned i=0;i<sizeof cases/sizeof *cases;i++) {
        prepare(&cases[i]);reference(&cases[i]);vdp1_execute(&machine);
        REQUIRE(compare_cpu(&cases[i]),cases[i].name);
    }
    /* Capture actual commands, then verify modes are immutable metadata. */
    saturn_vdp1_gpu_sink sink={NULL,capture,NULL};vdp1_gpu_bind(&machine,&sink);
    captured_count=0;prepare(&cases[0]);vdp1_execute(&machine);
    REQUIRE(captured_count==4,"erase, sprite and both lines captured");
    REQUIRE(captured[0].flip==0,"erase has no draw-coordinate mode");
    for(unsigned i=1;i<4;i++)REQUIRE((captured[i].flip&0x70000u)==
        (SATURN_VDP1_DIE|SATURN_VDP1_FIELD_SELECT),"commands latch DIE and parity selection");
    machine.vdp1_reg[1]=0;
    REQUIRE((captured[1].flip&SATURN_VDP1_DIE)!=0,"later register write cannot reinterpret queued draw");
    saturn_vk_vdp1_op a=captured[1],b=a;a.flip|=2u<<8;b=a;
    b.flip^=SATURN_VDP1_DIL;
    REQUIRE(!geometry_same(&a,&b),"opposite fields cannot become interpolation endpoints");
    b=a;b.flip&=~SATURN_VDP1_DIE;
    REQUIRE(!geometry_same(&a,&b),"different draw modes cannot become interpolation endpoints");
    vdp1_gpu_bind(&machine,NULL);return 1;
}

#ifdef SATURN_TEST_VULKAN
static uint32_t output[704*448];
static uint32_t saved_gpu[512*256*2];
static saturn saved_guest;
static int gpu_tests(saturn_vk_renderer *r) {
    for(unsigned i=0;i<sizeof cases/sizeof *cases;i++) {
        const fixture *f=&cases[i];prepare(f);reference(f);vdp1_execute(&machine);
        machine.vdp1_reg[1]=0; /* Deliberately change before queued GPU work executes. */
        machine.fb_draw=1;machine.frames++;
        REQUIRE(saturn_vk_render(r,&machine,704,448,error_text,sizeof error_text),"render queued fixture");
        REQUIRE(saturn_vk_readback(r,output,704,448,error_text,sizeof error_text),"wait/read completed GPU fixture");
        const uint32_t *pixels=r->fb.map;
        for(unsigned p=0;p<512*256;p++)if(pixels[p]!=expected[p]) {
            snprintf(error_text,sizeof error_text,"%s at %u,%u: got %04X expected %04X",f->name,p%512,p/512,pixels[p],expected[p]);
            REQUIRE(0,"GPU draw-space/field mapping");
        }
        REQUIRE(1,"complete GPU framebuffer matches row reference");
    }
    /* Bus writes already carry physical framebuffer addresses; neither the
     * active double-interlace mode nor drawing clips apply to them. */
    machine.vdp1_reg[1]=12;machine.vdp2_reg[0]=0x81c3;
    unsigned off=(190u*512u+54u)*2u;
    vdp1_gpu_fb_write(&machine,0,off,4,0x81238456u);
    vdp1_gpu_fb_write(&machine,0,off+1,1,0xabu);
    REQUIRE(saturn_vk_render(r,&machine,704,448,error_text,sizeof error_text),"render framebuffer bus writes in DIE mode");
    REQUIRE(saturn_vk_readback(r,output,704,448,error_text,sizeof error_text),"wait for framebuffer bus writes");
    const uint32_t *pixels=r->fb.map;
    REQUIRE(pixels[190u*512u+54u]==0x81abu&&pixels[190u*512u+55u]==0x8456u,
        "framebuffer byte/long writes bypass draw-mode mapping and clipping");

    saturn_runtime_settings settings;saturn_settings_defaults(&settings);settings.internal_scale=2;
    REQUIRE(saturn_vk_set_quality(r,&settings,error_text,sizeof error_text),"enable 2x internal rasterization");
    prepare(&cases[0]);vdp1_execute(&machine);machine.fb_draw=1;machine.frames++;
    machine.vdp2_reg[0]=0x81c3;machine.vdp2_reg[0xe0/2]=0x20;machine.vdp2_reg[0xf0/2]=1;
    REQUIRE(saturn_vk_interpolation_begin(r,&machine,704,448,error_text,sizeof error_text),"build high-resolution interlaced display");
    REQUIRE(saturn_vk_readback(r,output,704,448,error_text,sizeof error_text),"read canonical interlaced composite");
    REQUIRE(r->history_valid,"physical framebuffer erase covers interlaced replay viewport");
    unsigned native_sample=output[300u*704u+44u];
    REQUIRE((native_sample&0xffffffu)==0x080000u,"selected draw row appears at original 448-line screen position");
    memcpy(saved_gpu,r->fb.map,sizeof saved_gpu);saved_guest=machine;
    uint32_t *large=malloc(1408u*896u*sizeof *large);
    REQUIRE(large!=NULL,"allocate enhanced image readback");
    int ok=saturn_vk_interpolation_render(r,&machine,1,704,448,error_text,sizeof error_text)&&
        saturn_vk_readback_presented(r,large,1408,896,error_text,sizeof error_text);
    unsigned enhanced_sample=ok?large[600u*1408u+88u]:0;free(large);
    REQUIRE(ok,"rerasterize DIE scene at 1408x896");
    REQUIRE(enhanced_sample==native_sample,"2x render preserves original draw-space texture row");
    REQUIRE(!memcmp(saved_gpu,r->fb.map,sizeof saved_gpu),"enhancement preserves persistent native framebuffer pair");
    REQUIRE(!memcmp(&saved_guest,&machine,sizeof machine),"enhancement preserves guest RAM and registers");
    return 1;
}
#endif
int main(void) {
    if(!cpu_tests())return 1;
#ifdef SATURN_TEST_VULKAN
    SDL_SetMainReady();if(SDL_Init(SDL_INIT_VIDEO)){fprintf(stderr,"SDL: %s\n",SDL_GetError());return 77;}
    SDL_Window *window=SDL_CreateWindow("VDP1 interlace regression",0,0,704,448,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
    if(!window){fprintf(stderr,"SDL Vulkan: %s\n",SDL_GetError());SDL_Quit();return 77;}
    saturn_vk_renderer *r=saturn_vk_create(window,&machine,error_text,sizeof error_text);
    int ok=r&&gpu_tests(r);if(!r)fprintf(stderr,"Vulkan: %s\n",error_text);
    saturn_vk_destroy(r);SDL_DestroyWindow(window);SDL_Quit();if(!ok)return 1;
#endif
    printf("VDP1 interlace: %u checks passed%s\n",checks,
#ifdef SATURN_TEST_VULKAN
        " (CPU + actual Vulkan)"
#else
        " (CPU and queued-command metadata)"
#endif
    );return 0;
}
