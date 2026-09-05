/* Render the same diagnostic snapshot as render_state.c through Vulkan. */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "saturn.h"
#include "vulkan_renderer.h"
#include <stdlib.h>
int main(int argc, char **argv) {
    if(argc!=3){fprintf(stderr,"usage: render_state_vk state.bin frame.png\n");return 2;}
    saturn *s=malloc(sizeof *s);FILE *f=fopen(argv[1],"rb");
    if(!s||!f||fread(s,sizeof *s,1,f)!=1)return 1;
    fclose(f);
    int w,h;vdp2_display_size(s,&w,&h);
    s->layer_mask=0x3F;s->layer_lock=1;
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 1;
    SDL_Window *window=SDL_CreateWindow("VDP2 snapshot check",0,0,w,h,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
    char error[512]={0};
    saturn_vk_renderer *r=saturn_vk_create(window,s,error,sizeof error);
    uint32_t *pixels=malloc((size_t)w*h*4);
    int ok=r&&pixels&&saturn_vk_render(r,s,w,h,error,sizeof error)&&
        saturn_vk_readback(r,pixels,w,h,error,sizeof error)&&png_write(argv[2],pixels,w,h)==0;
    if(!ok)fprintf(stderr,"Vulkan snapshot failed: %s\n",error);
    else printf("rendered %s on %s\n",argv[2],saturn_vk_device_name(r));
    saturn_vk_destroy(r);SDL_DestroyWindow(window);SDL_Quit();free(pixels);free(s);
    return !ok;
}
