#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "saturn.h"
#include "geometry_interp.h"
#include <stdio.h>
static saturn s;
static uint32_t pixels[32*32];
static saturn_vk_vdp1_op quad(float x,unsigned seam) {
 saturn_vk_vdp1_op o={0};o.kind=1;o.target=1;o.flat=0x801f;o.pmod=0xc0;
 o.sys_x1=o.sys_y1=31;o.flip=(4u<<8)|SATURN_GEOMETRY_FLOAT|seam;
 float xy[8]={x,2,x+9,2,x+9,29,x,29};memcpy(o.xy,xy,sizeof xy);return o;
}
int main(void){
 SDL_SetMainReady();if(SDL_Init(SDL_INIT_VIDEO))return 2;
 SDL_Window *w=SDL_CreateWindow("Interpolation seam regression",0,0,128,128,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
 char error[512]={0};s.vdp2_reg[0]=0x8000;s.vdp2_reg[0xe0>>1]=0x20;s.vdp2_reg[0xf0>>1]=7;s.layer_mask=0x20;
 saturn_vk_renderer *r=saturn_vk_create(w,&s,error,sizeof error);if(!r){puts(error);return 2;}
 unsigned holes[2]={0,0};
 for(int pass=0;pass<2;pass++) {
  saturn_vk_vdp1_op ops[3]={0};ops[0].kind=3;ops[0].target=1;ops[0].xy[2]=32;ops[0].xy[3]=31;
  ops[1]=quad(.25f,pass?1u<<28:0);ops[2]=quad(10.25f,pass?1u<<30:0);
  if(!saturn_vk_replay_geometry(r,&s,ops,3,32,32,error,sizeof error)||!saturn_vk_readback(r,pixels,32,32,error,sizeof error)){puts(error);return 2;}
  for(unsigned y=4;y<28;y++)holes[pass]+=(pixels[y*32+10]&0xffffffu)==0;
  char path[128];snprintf(path,sizeof path,"out/modulation-interp/seam-test-%d.png",pass);png_write(path,pixels,32,32);
 }
 printf("Pixel-adjacent seam: baseline %u holes, corrected %u holes (24 sample rows)\n",holes[0],holes[1]);
 saturn_vk_destroy(r);SDL_DestroyWindow(w);SDL_Quit();return holes[0]!=24||holes[1]!=0;
}
