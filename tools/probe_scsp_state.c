/* Local diagnostic snapshots contain game data; keep their output ignored. */
#include "saturn.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <tlhelp32.h>
static saturn s;
void m68k_set_irq(m68k *m, int l, int v) { (void)m; (void)l; (void)v; }
int main(int argc, char **argv) {
    if(argc==3) {
        DWORD pid=(DWORD)strtoul(argv[1],0,0);
        HANDLE process=OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,pid);
        MODULEENTRY32 mod={0}; mod.dwSize=sizeof mod;
        if(!process || !Module32First(snap,&mod))return 2;
        SIZE_T got=0;
        uintptr_t address=(uintptr_t)mod.modBaseAddr+(uintptr_t)strtoull(argv[2],0,0);
        int ok=ReadProcessMemory(process,(void*)address,&s,sizeof s,&got)&&got==sizeof s;
        CloseHandle(snap);CloseHandle(process);
        if(!ok)return 2;
        /* Live read is approximate; no writes or suspension of the game. */
    } else {
    if(argc!=2)return 2;
    FILE *f=fopen(argv[1],"rb");
    if(!f)return 2;
    if(fread(&s,1,sizeof s,f)!=sizeof s)return 2;
    fclose(f);
    }
    printf("DSP length=%u RBP=%X RBL=%X PC=%u\n",s.dsp.prog_len,s.dsp.rbp,s.dsp.rbl,s.dsp.pc);
    for(int i=0;i<32;i++) {
        printf("slot %02d active=%d phase=%d env=%X pos=%X",i,s.scsp_slot[i].active,s.scsp_slot[i].phase,s.scsp_slot[i].env,s.scsp_slot[i].pos);
        for(int j=0;j<12;j++)printf(" %04X",s.scsp_reg[i*16+j]);
        puts("");
    }
    for(int i=0;i<16;i++)printf("EFREG %d %d\n",i,s.dsp.efreg[i]);
    for(unsigned i=0;i<s.dsp.prog_len;i++)printf("MPRO %02u %016llX\n",i,(unsigned long long)s.dsp.program[i]);
    return 0;
}
