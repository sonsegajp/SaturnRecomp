/* Actual Vulkan regression for draw-time VDP1 material ownership.
 * No BIOS, disc or window interaction is needed. See
 * tools/test_interpolation_material.ps1 for the focused build/run command.
 * The renderer is included to inspect its persistent GPU buffers and force
 * integer endpoint replay, separating material fidelity from coverage rules.
 */
#define SDL_MAIN_HANDLED
#define _POSIX_C_SOURCE 200809L
#include "../runner/src/vulkan_renderer.c"

enum { TEST_W=320, TEST_H=224, TEST_PIXELS=TEST_W*TEST_H };
static saturn machine;
static uint32_t canonical[TEST_PIXELS],actual[TEST_PIXELS],first_picture[TEST_PIXELS];
static uint32_t persistent_fb[FB_PIXELS*2],persistent_mesh[FB_PIXELS*2];
static uint8_t live_material[VDP1_VRAM_SZ];
static char failure[512];
static unsigned checks;

#define REQUIRE(condition, message) do { \
    checks++; if(!(condition)) { \
        fprintf(stderr,"FAIL: %s (%s)\n",message,failure); return 0; \
    } \
} while(0)

static void word(unsigned address,unsigned value) {
    machine.vdp1_vram[address]=(uint8_t)(value>>8);
    machine.vdp1_vram[address+1]=(uint8_t)value;
}
static void materials(unsigned variant) {
    static const unsigned shades[]={0x4210,0x421f,0x7e10,0x43f0};
    static const unsigned colours[]={0x801f,0x83e0,0xfc00,0xffff};
    for(unsigned i=0;i<4;i++)word(0x3000+i*2,shades[variant&3]);
    for(unsigned i=0;i<8*8;i++)word(0x1000+i*2,colours[variant&3]);
    memset(machine.vdp1_vram+0x2000,0x11,8*8/2);
    word(0x2800+2,colours[(variant+1)&3]);
}
static saturn_vk_vdp1_op square(unsigned target,unsigned which) {
    saturn_vk_vdp1_op o={0};
    int x=20+(int)which*90;
    o.kind=SATURN_VK_VDP1_QUAD;o.target=target;
    o.xy[0]=o.xy[6]=x;o.xy[2]=o.xy[4]=x+60;
    o.xy[1]=o.xy[3]=30;o.xy[5]=o.xy[7]=100;
    o.sys_x1=511;o.sys_y1=255;o.flat=0xc210;
    o.flip=(which?2u:4u)<<8;
    if(which==0) {o.pmod=4;o.grda=0x3000/8;}
    else {
        o.textured=1;o.tw=o.th=8;
        o.chr=which==1?0x1000:0x2000;
        o.pmod=which==1?5u<<3:1u<<3;
        o.colr=which==2?0x2800/8:0;
    }
    return o;
}
static int picture(saturn_vk_renderer *r,unsigned target) {
    saturn_vk_vdp1_op erase={0};
    erase.kind=SATURN_VK_VDP1_ERASE;erase.target=target;
    erase.xy[2]=TEST_W;erase.xy[3]=TEST_H-1;
    REQUIRE(queue_vdp1(r,&erase),"queue full clear");
    for(unsigned i=0;i<3;i++) {
        saturn_vk_vdp1_op o=square(target,i);
        REQUIRE(queue_vdp1(r,&o),"queue material square");
    }
    return 1;
}
static int begin(saturn_vk_renderer *r,unsigned field,unsigned display) {
    machine.frames=field;machine.fb_draw=(int)(display^1u);
    REQUIRE(saturn_vk_interpolation_begin(r,&machine,TEST_W,TEST_H,failure,sizeof failure),"canonical draw");
    REQUIRE(saturn_vk_readback(r,canonical,TEST_W,TEST_H,failure,sizeof failure),"canonical readback");
    r->rotation_pair=0;
    return 1;
}
static int endpoint_replays(saturn_vk_renderer *r,int expect_replay) {
    /* Preserve the real match decision, but use its current integer vertices:
     * every pixel must match, regardless of subpixel edge rasterization. */
    for(unsigned i=0;i<r->blend_count;i++)r->blend[i]=r->history[i];
    memcpy(persistent_fb,r->fb.map,sizeof persistent_fb);
    memcpy(persistent_mesh,r->mesh.map,sizeof persistent_mesh);
    memcpy(live_material,machine.vdp1_vram,sizeof live_material);
    static const float alphas[]={0.125f,0.5f,0.875f};
    for(unsigned i=0;i<sizeof alphas/sizeof *alphas;i++) {
        REQUIRE(saturn_vk_interpolation_render(r,&machine,alphas[i],TEST_W,TEST_H,failure,sizeof failure),"intermediate render");
        REQUIRE(saturn_vk_readback(r,actual,TEST_W,TEST_H,failure,sizeof failure),"intermediate readback");
        REQUIRE((r->picture_count!=0)==expect_replay,"correct replay/fallback path");
        REQUIRE(!memcmp(canonical,actual,sizeof canonical),"replayed material matches canonical pixels");
        REQUIRE(!memcmp(persistent_fb,r->fb.map,sizeof persistent_fb),"both persistent framebuffers unchanged");
        REQUIRE(!memcmp(persistent_mesh,r->mesh.map,sizeof persistent_mesh),"both persistent mesh buffers unchanged");
        REQUIRE(!memcmp(live_material,machine.vdp1_vram,sizeof live_material),"guest material RAM unchanged");
    }
    REQUIRE(saturn_vk_interpolation_render(r,&machine,1,TEST_W,TEST_H,failure,sizeof failure),"native endpoint render");
    REQUIRE(saturn_vk_readback(r,actual,TEST_W,TEST_H,failure,sizeof failure),"native endpoint readback");
    REQUIRE(!memcmp(canonical,actual,sizeof canonical),"native endpoint unchanged after replay");
    return 1;
}
static int regression(saturn_vk_renderer *r) {
    materials(0);
    REQUIRE(picture(r,0)&&begin(r,1,1),"draw first buffer");
    REQUIRE(picture(r,1)&&begin(r,2,0),"draw second buffer with same materials");
    memcpy(first_picture,canonical,sizeof canonical);
    REQUIRE((canonical[60*TEST_W+40]&0xffffffu)!=0,"Gouraud square is visible");
    REQUIRE((canonical[60*TEST_W+130]&0xffffffu)!=0,"direct texture is visible");
    REQUIRE((canonical[60*TEST_W+220]&0xffffffu)!=0,"CLUT texture is visible");

    /* A future draw overwrites all three resource kinds in shared VRAM while
     * the opposite framebuffer still displays its original material. */
    materials(1);
    REQUIRE(picture(r,0)&&begin(r,3,1),"overwrite future buffer materials");
    REQUIRE(r->matched==3,"all stable faces still interpolate");
    REQUIRE(!memcmp(first_picture,canonical,sizeof canonical),"other buffer upload does not change display");
    REQUIRE(endpoint_replays(r,1),"Gouraud, CLUT and texture epoch preservation");

    materials(2);
    REQUIRE(picture(r,1)&&begin(r,4,0),"swap to the next material epoch");
    REQUIRE(memcmp(first_picture,canonical,sizeof canonical)!=0,"newly displayed materials take effect");
    REQUIRE(r->matched>0,"Gouraud face remains eligible after buffer swap");
    REQUIRE(endpoint_replays(r,1),"buffer swap selects its own material image");

    /* Accumulating a new draw without a clear mixes material epochs within
     * one framebuffer; it must retain native pixels until a full redraw. */
    materials(3);
    saturn_vk_vdp1_op extra=square(1,0);extra.xy[0]+=10;extra.xy[6]+=10;
    REQUIRE(queue_vdp1(r,&extra)&&begin(r,5,0),"partial next-buffer draw");
    REQUIRE(begin(r,6,1),"display a buffer with multiple material epochs");
    REQUIRE(!r->history_valid&&!r->matched,"partial material history is held");
    REQUIRE(endpoint_replays(r,0),"partial draw preserves native pixels");

    REQUIRE(picture(r,1)&&begin(r,7,0),"complete clear and redraw restores safety");
    REQUIRE(begin(r,8,1),"display fully redrawn buffer");
    REQUIRE(r->history_valid&&r->matched>0,"full redraw restores interpolation");
    REQUIRE(endpoint_replays(r,1),"recovered material history");
    return 1;
}
int main(void) {
    SDL_SetMainReady();
    /* The renderer reads the C runtime environment. SDL's Windows DLL may
     * use a different CRT, so SDL_setenv alone does not update getenv here. */
#ifdef _WIN32
    _putenv("SATURN_VK_VDP1_ONLY=1");
#else
    setenv("SATURN_VK_VDP1_ONLY","1",1);
#endif
    if(SDL_Init(SDL_INIT_VIDEO)!=0) {fprintf(stderr,"SDL: %s\n",SDL_GetError());return 77;}
    SDL_Window *window=SDL_CreateWindow("Material regression",0,0,TEST_W,TEST_H,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
    if(!window) {fprintf(stderr,"Vulkan window: %s\n",SDL_GetError());SDL_Quit();return 77;}
    saturn_vk_renderer *renderer=saturn_vk_create(window,&machine,failure,sizeof failure);
    if(!renderer) {fprintf(stderr,"Vulkan: %s\n",failure);SDL_DestroyWindow(window);SDL_Quit();return 77;}
    int ok=saturn_vk_interpolation_enable(renderer)&&regression(renderer);
    saturn_vk_destroy(renderer);SDL_DestroyWindow(window);SDL_Quit();
    if(ok)printf("interpolation material: %u checks passed (actual Vulkan)\n",checks);
    return ok?0:1;
}
