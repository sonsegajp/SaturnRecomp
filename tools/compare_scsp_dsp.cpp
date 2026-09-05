/* Differential diagnostic; requires a user-provided local Ymir source tree. */
extern "C" {
#include "saturn.h"
}
#include <ymir/hw/scsp/scsp_dsp.hpp>
#include <cstdio>
#include <cstring>
#include <algorithm>
static saturn s;
static unsigned char ram[SOUND_RAM_SZ];
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    FILE *f=fopen(argv[1],"rb");
    if(!f)return 2;
    if(fread(&s,1,sizeof s,f)!=sizeof s)return 2;
    fclose(f);
    memcpy(ram,s.sound_ram,sizeof ram);
    ymir::scsp::DSP ref(ram);
    ymir::savestate::SCSPDSPSaveState st{};
    auto &d=s.dsp;
#define COPY(to,from) std::copy(std::begin(from),std::end(from),std::begin(to))
    COPY(st.MPRO,d.program); COPY(st.TEMP,d.temp); COPY(st.MEMS,d.mems);
    COPY(st.COEF,d.coef); COPY(st.MADRS,d.madrs); COPY(st.MIXS,d.mixs);
    COPY(st.EFREG,d.efreg); COPY(st.EXTS,d.exts);
    st.MIXSGen=d.mixs_gen;st.MIXSNull=d.mixs_null;st.RBP=d.rbp>>12;
    for(int i=0;i<4;i++)if(((0x2000u<<i)-1)==d.rbl)st.RBL=i;
    st.PC=d.pc;st.INPUTS=d.inputs;st.SFT_REG=d.sft_reg;st.FRC_REG=d.frc_reg;
    st.Y_REG=d.y_reg;st.ADRS_REG=d.adrs_reg;st.MDEC_CT=d.mdec_ct;
    st.readPending=d.read_pending;st.readNOFL=d.read_nofl;st.readValue=d.read_value;
    st.writePending=d.write_pending;st.writeValue=d.write_value;st.readWriteAddr=d.rw_addr;
    ref.LoadState(st);
    for(int n=0;n<128*44100;n++) {
        unsigned pc=d.pc; unsigned wa=d.rw_addr*2u; bool written=d.write_pending && (!d.read_pending || pc>=d.prog_len) && wa<SOUND_RAM_SZ;
        scsp_dsp_step(&s); ref.Step();
        if(n%4==0) {
            unsigned slot=(n/4+26)&31,isel=slot&15;
            int v=n<128 ? 65536 : 0; // one impulse, then its delay tail
            scsp_dsp_mixs_write(&s,isel,v);ref.MIXSSlotWrite(isel,v);
        }
        if(memcmp(d.efreg,ref.effectOut.data(),sizeof d.efreg)||
           memcmp(d.temp,ref.tempMem.data(),sizeof d.temp)||
           memcmp(d.mems,ref.soundMem.data(),sizeof d.mems)||
           (written && memcmp(s.sound_ram+wa,ram+wa,2))) {
            printf("DSP divergence step=%d PC=%u temp=%d mems=%d effects=%d RAM=%d\n",n,pc,
                memcmp(d.temp,ref.tempMem.data(),sizeof d.temp),
                memcmp(d.mems,ref.soundMem.data(),sizeof d.mems),
                memcmp(d.efreg,ref.effectOut.data(),sizeof d.efreg),memcmp(s.sound_ram,ram,sizeof ram));
            return 1;
        }
    }
    puts("PASS: 44100 DSP samples, all TEMP/MEMS/EFREG and sound RAM match Ymir");
}
