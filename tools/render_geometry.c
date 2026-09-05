#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include "saturn.h"
#include "geometry_interp.h"
#include <stdlib.h>
static saturn s,before;
static saturn_vk_vdp1_op prev[8192],cur[8192],mid[8192];
static unsigned loadops(const char *prefix,int frame,saturn_vk_vdp1_op *ops){
    char path[1024];snprintf(path,sizeof path,"%s-%d.ops",prefix,frame);
    FILE *f=fopen(path,"rb");if(!f)return 0;
    unsigned n=(unsigned)fread(ops,sizeof(*ops),8192,f);fclose(f);return n;
}
int main(int argc,char **argv){
    if(argc!=5){fprintf(stderr,"usage: render_geometry prefix previous-field current-field output-prefix\n");return 2;}
    int a=atoi(argv[2]),b=atoi(argv[3]);
    unsigned pn=loadops(argv[1],a,prev),cn=loadops(argv[1],b,cur);
    if(!pn||!cn)return 2;
    char path[1024];snprintf(path,sizeof path,"%s-%d.bin",argv[1],b);
    FILE *f=fopen(path,"rb");if(!f||fread(&s,sizeof s,1,f)!=1)return 2;fclose(f);
    snprintf(path,sizeof path,"%s-%d.bin",argv[1],a);
    f=fopen(path,"rb");if(!f||fread(&before,sizeof before,1,f)!=1)return 2;fclose(f);
    for(unsigned i=0;i<pn;i++)if(prev[i].textured)prev[i].flat=geometry_texture_hash(&prev[i],before.vdp1_vram);
    for(unsigned i=0;i<cn;i++)if(cur[i].textured)cur[i].flat=geometry_texture_hash(&cur[i],s.vdp1_vram);
    s.layer_mask=0x3F;s.layer_lock=1;
    int w,h;vdp2_display_size(&s,&w,&h);
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
    SDL_Window *window=SDL_CreateWindow("Geometry interpolation lab",0,0,w,h,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
    char error[512]={0};saturn_vk_renderer *r=saturn_vk_create(window,&s,error,sizeof error);
    uint32_t *pixels=malloc((size_t)w*h*4);
    if(!r||!pixels)return 2;
    memcpy(&before,&s,sizeof s);
    for(int i=0;i<=4;i++){
        unsigned matched=geometry_interpolate(prev,pn,cur,cn,i/4.0f,mid);
        if(!saturn_vk_replay_geometry(r,&s,mid,cn,w,h,error,sizeof error)||
           !saturn_vk_readback(r,pixels,w,h,error,sizeof error)){fprintf(stderr,"%s\n",error);return 1;}
        snprintf(path,sizeof path,"%s-%d.png",argv[4],i);
        if(png_write(path,pixels,w,h))return 1;
        printf("alpha=%.2f matched=%u/%u -> %s\n",i/4.0f,matched,cn,path);
    }
    int changed=memcmp(&s,&before,sizeof s)!=0;
    printf("Guest state unchanged: %s\n",changed?"FAIL":"PASS");
    saturn_vk_destroy(r);SDL_DestroyWindow(window);SDL_Quit();free(pixels);
    return changed;
}