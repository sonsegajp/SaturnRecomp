/* Synthetic rotation coefficient and per-dot color calculation checks. */
#include "saturn.h"
#include <stdio.h>
#include <string.h>
static saturn s;
static uint32_t frame[704*512];
static int fails;
static void reg(unsigned a,unsigned v){s.vdp2_reg[a/2]=(uint16_t)v;}
static void word(unsigned a,unsigned v){s.vdp2_vram[a]=(uint8_t)(v>>8);s.vdp2_vram[a+1]=(uint8_t)v;}
static void longword(unsigned a,uint32_t v){word(a,v>>16);word(a+2,v);}
static void ck(const char *name,uint32_t got,uint32_t want){if(got!=want){printf("FAIL %s: %08X != %08X\n",name,got,want);fails++;}}
static void render(void){vdp2_render(&s,frame,320,224,1);}
int main(void){
    memset(&s,0,sizeof s);
    int w,h;reg(0,0x81c3);vdp2_display_size(&s,&w,&h);
    ck("704 interlace width",w,704);ck("double-density height",h,448);
    reg(0,0x8001);vdp2_display_size(&s,&w,&h);ck("progressive height",h,224);
    reg(0,0x8081);vdp2_display_size(&s,&w,&h);ck("single-density height",h,224);
    reg(0,0x81c3);reg(0xe0,0x20);reg(0xf0,7);s.layer_mask=0x20;
    s.vdp1_fb[1][(200*512+10)*2]=0x80;s.vdp1_fb[1][(200*512+10)*2+1]=0x1f;
    vdp2_render(&s,frame,704,448,1);
    ck("sprite interlace even line",frame[400*704+20],0xffff0000);
    ck("sprite interlace odd line",frame[401*704+21],0xffff0000);
    memset(&s,0,sizeof s);s.layer_mask=0x3F;s.layer_lock=1;
    reg(0,0x8000);reg(0x20,0x10);reg(0x2A,0x1000);reg(0x38,0x8000);reg(0xFC,4);
    reg(0xAC,3);reg(0xAE,0xF800);word(0x7F000,0x7C00); /* blue back */
    s.cram[2]=0;s.cram[3]=31; /* red, CC MSB clear */
    s.cram[4]=0x83;s.cram[5]=0xE0; /* green, CC MSB set */
    for(unsigned a=0;a<0x2000;a+=2)word(a,0x100);
    for(unsigned a=0;a<64;a++)s.vdp2_vram[0x2000+a]=(a&1)?2:1;
    reg(0xBC,3);reg(0xBE,0);
    longword(0x60010,0x10000);longword(0x60014,0x10000);
    longword(0x6001C,0x10000);longword(0x6002C,0x10000);
    longword(0x6004C,0x10000);longword(0x60050,0x10000);
    reg(0xB4,3);reg(0xB6,2);longword(0x60058,0x10000);
    word(0x40000,0x400);word(0x40002,0x800);word(0x40004,0x8000);
    render();
    ck("per-line coefficient unity",frame[1],0xFF00FF00);
    ck("per-line coefficient double",frame[321],0xFFFF0000);
    ck("coefficient transparency",frame[641],0xFF0000FF);
    reg(0xEC,0x10);reg(0xEE,0x300);reg(0x10C,15);
    render();
    ck("grass palette MSB clear remains opaque",frame[0],0xFFFF0000);
    ck("water palette MSB set blends",frame[1],0xFF007F7F);
    reg(0xE8,0x10);reg(0xA8,3);reg(0xAA,0x8000);word(0x70000,1);
    render();
    ck("line color inserted below water",frame[1],0xFF7F7F00);
    reg(0xEC,0x430);render();
    ck("extended line color mixes with reflected back",frame[1],0xFF3F7F3F);
    reg(0xE8,0);
    /* 32-bit coefficients carry a signed 8.16 value and TP in bit 31. */
    reg(0xB4,1);reg(0xB6,1);reg(0xEC,0);
    longword(0x40000,0x10000);longword(0x40004,0x20000);longword(0x40008,0x80000000);
    render();
    ck("long coefficient unity",frame[1],0xFF00FF00);
    ck("long coefficient double",frame[321],0xFFFF0000);
    ck("long coefficient transparency",frame[641],0xFF0000FF);
    printf("VDP2 effects: %d failure(s)\n",fails);return fails!=0;
}
