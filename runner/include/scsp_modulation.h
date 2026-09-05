#ifndef SATURN_SCSP_MODULATION_H
#define SATURN_SCSP_MODULATION_H
#include <stdint.h>
/* SCSP fixed-point modulation, following Ymir SlotProcessStep1/2/3/4.
 * PLFO is signed pitch displacement; ALFO is logarithmic attenuation. */
static const uint16_t scsp_lfo_interval[32]={1020,892,764,636,508,444,380,316,252,220,188,156,124,108,92,76,60,52,44,36,28,24,20,16,12,10,8,6,4,3,2,1};
static inline uint32_t scsp_noise_next(uint32_t n) {return (n>>1)|(((n>>5)^n)&1u)<<16;}
static inline int scsp_plfo_wave(unsigned wave,unsigned phase,uint32_t noise) {
    switch(wave&3u) {
    case 0:return (int8_t)(phase&254u);
    case 1:return phase<128?126:-128;
    case 2:{unsigned x=(phase+64u)&255u;return (int)(x<128?x:255-x)*2-128;}
    default:return (int8_t)((noise^128u)&254u);
    }
}
static inline unsigned scsp_alfo_wave(unsigned wave,unsigned phase,uint32_t noise) {
    switch(wave&3u) {
    case 0:return phase&254u;
    case 1:return phase<128?0:254;
    case 2:return (phase<128?phase:255-phase)*2;
    default:return noise&254u;
    }
}
static inline int scsp_pitch_lfo(uint16_t reg,unsigned phase,uint32_t noise,unsigned fns) {
    unsigned depth=(reg>>5)&7u;
    return depth ? ((scsp_plfo_wave(reg>>8,phase,noise)>>(7-depth))*(int)(fns>>4))>>6 : 0;
}
static inline unsigned scsp_amp_lfo(uint16_t reg,unsigned phase,uint32_t noise) {
    unsigned depth=reg&7u;
    return depth ? scsp_alfo_wave(reg>>3,phase,noise)>>(7-depth) : 0;
}
static inline int32_t scsp_fm_displacement(int16_t x,int16_t y,unsigned depth) {
    if(depth<5)return 0;
    uint32_t sum=(uint32_t)((int32_t)x+y)&0x3ffffeu;
    return (int16_t)((sum<<5)>>(16-depth));
}
#endif
