/* Diagnostic replay of SATURN_GEOMETRY_CAPTURE snapshots from this build.
 * Args: input-prefix output-prefix [mode: 0 normal, 1 no rotation interpolation,
 * 2 no geometry interpolation, 3 assert held rotation endpoints stay fixed] [first-field].
 * Replays 12 consecutive fields (default 6400..6411). Snapshots contain local game data and
 * must remain outside source control. Compile with core objects excluding
 * window.o, boot.o and vulkan_renderer.o; link SDL2 and Vulkan.
 */
#define SDL_MAIN_HANDLED
#include "../runner/src/vulkan_renderer.c"
static saturn captured;
static uint32_t pixels[704*512];
static saturn_vk_vdp1_op ops[8192],prior[8192];
int main(int argc,char **argv){
 if(argc<3)return 2;SDL_SetMainReady();if(SDL_Init(SDL_INIT_VIDEO))return 2;
 SDL_Window *w=SDL_CreateWindow("Captured interpolation",0,0,352,224,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
 char error[512]={0},path[1024];saturn_vk_renderer *r=saturn_vk_create(w,&captured,error,sizeof error);
 if(!r||!saturn_vk_interpolation_enable(r)){puts(error);return 2;}
 unsigned lastn=0;
 unsigned first=argc>4?(unsigned)strtoul(argv[4],NULL,0):6400;
 for(unsigned field=first;field<first+12;field++){
  snprintf(path,sizeof path,"%s-%u.bin",argv[1],field);FILE *f=fopen(path,"rb");if(!f)return 2;
  if(fread(&captured,sizeof captured,1,f)!=1)return 2;fclose(f);
  snprintf(path,sizeof path,"%s-%u.ops",argv[1],field);f=fopen(path,"rb");if(!f)return 2;
  unsigned n=fread(ops,sizeof *ops,8192,f);fclose(f);
  if(n!=lastn||memcmp(ops,prior,n*sizeof *ops))for(unsigned i=0;i<n;i++)queue_vdp1(r,&ops[i]);
  memcpy(prior,ops,n*sizeof *ops);lastn=n;
  if(getenv("SATURN_REPLAY_LAYERS")){captured.layer_mask=(unsigned)strtoul(getenv("SATURN_REPLAY_LAYERS"),NULL,0);captured.layer_lock=1;}
  int width,height;vdp2_display_size(&captured,&width,&height);
  if(!saturn_vk_interpolation_begin(r,&captured,width,height,error,sizeof error))return 2;
  if(!saturn_vk_readback(r,pixels,width,height,error,sizeof error))return 2;
  snprintf(path,sizeof path,"%s-%u-native.png",argv[2],field);png_write(path,pixels,width,height);
  if(argc>3&&atoi(argv[3])==1)r->rotation_pair=0;
  if(argc>3&&atoi(argv[3])==2)r->matched=0;
  for(unsigned p=0;p<2;p++){
   if(!saturn_vk_interpolation_render(r,&captured,.25f+.5f*p,width,height,error,sizeof error)||!saturn_vk_readback(r,pixels,width,height,error,sizeof error)){puts(error);return 2;}
   snprintf(path,sizeof path,"%s-%u-%u.png",argv[2],field,p);png_write(path,pixels,width,height);
  }
  if(field==first+4 && argc>3 && atoi(argv[3])==3) {
   static uint32_t before[704*512];
   if(!saturn_vk_interpolation_render(r,&captured,.25f,width,height,error,sizeof error)||!saturn_vk_readback(r,before,width,height,error,sizeof error))return 2;
   unsigned base=((((unsigned)captured.vdp2_reg[0xbc/2]&7)<<16)|captured.vdp2_reg[0xbe/2])*2u&0x7ff7c;
   uint8_t *ptr=captured.vdp2_vram+base+0x48;
   uint32_t word=((uint32_t)ptr[0]<<24)|((uint32_t)ptr[1]<<16)|((uint32_t)ptr[2]<<8)|ptr[3];
   word+=32u<<16;ptr[0]=word>>24;ptr[1]=word>>16;ptr[2]=word>>8;ptr[3]=word;
   r->core_only=1;int ok=saturn_vk_render(r,&captured,width,height,error,sizeof error);r->core_only=0;
   if(!ok||!saturn_vk_interpolation_render(r,&captured,.25f,width,height,error,sizeof error)||!saturn_vk_readback(r,pixels,width,height,error,sizeof error))return 2;
   unsigned changes=0;for(int i=0;i<width*height;i++)changes+=pixels[i]!=before[i];
   printf("Held-pair live rotation-table update: %u changed pixels\n",changes);
   saturn_vk_destroy(r);SDL_DestroyWindow(w);SDL_Quit();return changes?1:0;
  }
  printf("frame %u: ops %u matched %u rotation %d span %u age %u\n",field,n,r->matched,r->rotation_pair,r->geometry_span,r->geometry_age);
 }
 saturn_vk_destroy(r);SDL_DestroyWindow(w);SDL_Quit();return 0;
}
