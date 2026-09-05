#include "saturn.h"
#include "scsp_modulation.h"
#include <stdio.h>
#include <string.h>
static saturn s;
static int failures;
#define CHECK(x) do {if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);failures++;}}while(0)
void m68k_set_irq(m68k *m,int l,int v){(void)m;(void)l;(void)v;}
static void tick(void){int16_t l,r;scsp_render(&s,&l,&r);}
static void reset(void){memset(&s,0,sizeof s);scsp_reset(&s);}
static void voice(void){
    reset();scsp_write(&s,6,100);scsp_write(&s,0x0c,0x100);
    scsp_write(&s,0x10,0x400);scsp_write(&s,0x16,0xe000);
    s.sound_ram[0]=0x10;s.sound_ram[2]=0x20;s.sound_ram[4]=0x30;
    scsp_write(&s,0,0x1800);
}
int main(void){
    /* Independent full-period tables, including signed wrap at 128. */
    for(unsigned x=0;x<256;x++){
        CHECK(scsp_plfo_wave(0,x,0)==(x<128?(int)(x&254):(int)(x&254)-256));
        CHECK(scsp_alfo_wave(0,x,0)==(x&254));
        CHECK(scsp_plfo_wave(1,x,0)==(x<128?126:-128));
        CHECK(scsp_alfo_wave(1,x,0)==(x<128?0:254));
        CHECK(scsp_alfo_wave(2,x,0)==(x<128?2*x:510-2*x));
    }
    for(unsigned i=0;i<128;i++){
        CHECK(scsp_plfo_wave(2,(i-64)&255,0)==(int)i*2-128);
        CHECK(scsp_plfo_wave(2,(191-i)&255,0)==(int)i*2-128);
    }
    for(unsigned depth=0;depth<5;depth++)CHECK(scsp_fm_displacement(32767,32767,depth)==0);
    CHECK(scsp_fm_displacement(1024,1024,5)==32);
    CHECK(scsp_fm_displacement(-1024,-1024,5)==-32);
    CHECK(scsp_fm_displacement(32767,32767,15)==-32);
    CHECK(scsp_pitch_lfo(0,100,1,1024)==0);
    CHECK(scsp_amp_lfo(0,100,1)==0);
    reset();scsp_write(&s,0x12,31u<<10);tick();CHECK(s.scsp_slot[0].lfo_phase==1);
    scsp_write(&s,0x12,(31u<<10)|0x8000);tick();CHECK(s.scsp_slot[0].lfo_phase==0);
    scsp_write(&s,0x12,31u<<10);tick();CHECK(s.scsp_slot[0].lfo_phase==1);
    reset();for(int i=0;i<1019;i++)tick();CHECK(s.scsp_slot[0].lfo_phase==0);tick();CHECK(s.scsp_slot[0].lfo_phase==1);
    voice();scsp_write(&s,0x600,1024);scsp_write(&s,0x0e,0x5000);tick();CHECK(s.scsp_slot[0].output==0x2000);
    voice();scsp_write(&s,0x600,1024);scsp_write(&s,0x0e,0x4000);tick();CHECK(s.scsp_slot[0].output==0x1000);
    voice();scsp_write(&s,0x600,1234);scsp_write(&s,0x0c,0x300);tick();CHECK(scsp_read(&s,0x600)==1234);
    reset();scsp_write(&s,0x600,0xabcd);scsp_write8(&s,0x601,0x12);CHECK(scsp_read(&s,0x600)==0xab12);
    voice();tick();CHECK(scsp_read(&s,0x600)==0x1000);CHECK(s.scsp_stack_index==32);tick();CHECK(s.scsp_stack_index==0);
    voice();scsp_write(&s,0x0a,0x8000);scsp_write(&s,0x0c,0);scsp_write(&s,0x12,7|(1<<3));s.scsp_slot[0].lfo_phase=128;tick();CHECK(s.scsp_slot[0].output<1024);
    voice();scsp_write(&s,0x12,7|(1<<3));s.scsp_slot[0].lfo_phase=128;tick();CHECK(s.scsp_slot[0].output==4096);
    printf("SCSP modulation: %d failure(s)\n",failures);return failures!=0;
}
