/* Vulkan integration stress: native/interpolated presentation and resize.
 * Link core objects excluding window.o, boot.o, vulkan_renderer.o, plus SDL2
 * and Vulkan. Run from the repository root with built runner/shaders. */
#define SDL_MAIN_HANDLED
#include "../runner/src/vulkan_renderer.c"
static saturn s;
static uint32_t pixels[32*32];
int main(void){
 SDL_SetMainReady();if(SDL_Init(SDL_INIT_VIDEO))return 2;
 SDL_Window *w=SDL_CreateWindow("Presentation stress",0,0,128,128,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE);
 char error[512]={0};s.vdp2_reg[0]=0x8000;s.vdp2_reg[0xe0>>1]=0x20;s.vdp2_reg[0xf0>>1]=7;s.layer_mask=0x20;
 saturn_vk_renderer *r=saturn_vk_create(w,&s,error,sizeof error);if(!r){puts(error);return 2;}
 for(unsigned frame=1;frame<=1200;frame++) {
  s.frames=frame;
  if(frame%60==1)SDL_SetWindowSize(w,(frame/60)%2?160:128,(frame/60)%2?160:128);
  if(frame%120==1){if(r->interpolate)saturn_vk_interpolation_disable(r);else if(!saturn_vk_interpolation_enable(r))return 2;}
  saturn_vk_vdp1_op o[2]={0};o[0].kind=3;o[0].target=1;o[0].xy[2]=32;o[0].xy[3]=31;
  o[1].kind=1;o[1].target=1;o[1].flat=0x801f;o[1].pmod=0xc0;o[1].flip=4u<<8;o[1].sys_x1=o[1].sys_y1=31;
  const int xy[]={2,2,29,2,29,29,2,29};memcpy(o[1].xy,xy,sizeof xy);
  for(unsigned i=0;i<2;i++)if(!queue_vdp1(r,&o[i]))return 2;
  if(r->interpolate){if(!saturn_vk_interpolation_begin(r,&s,32,32,error,sizeof error))goto fail;for(unsigned k=0;k<2;k++)if(!saturn_vk_interpolation_render(r,&s,.25f+.5f*k,32,32,error,sizeof error)||!saturn_vk_present(r,error,sizeof error))goto fail;}
  else if(!saturn_vk_render(r,&s,32,32,error,sizeof error)||!saturn_vk_present(r,error,sizeof error))goto fail;
  if(frame%120==0){if(!saturn_vk_readback(r,pixels,32,32,error,sizeof error))goto fail;if((pixels[16*32+16]&0xffffff)!=0xff0000){puts("wrong center pixel");goto fail;}}
 }
 puts("PASS 1200 fields, 1800 presentations, 20 resize requests, 10 interpolation switches, stable sampled pixels");
 saturn_vk_destroy(r);SDL_DestroyWindow(w);SDL_Quit();return 0;
fail: puts(error);saturn_vk_destroy(r);SDL_DestroyWindow(w);SDL_Quit();return 1;
}
