/* Read-only Windows sampler. Arguments: PID g_sys_RVA seconds output-prefix.
 * Match the executable's g_sys RVA with nm; never attach using an old layout.
 * Sampling is approximate, not an architectural trace. No target writes. */
#include "saturn.h"
#include "sh2_isa.h"
#include <windows.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#define CAP 32768u
static struct {uint32_t pc;unsigned core,hits;unsigned char code[32];} bins[CAP];
static unsigned nbin;
static unsigned pending[4096],npending;
static HANDLE process;
static uintptr_t state;
static uint64_t timer_freq;
static ULONGLONG stamp(void){LARGE_INTEGER t;QueryPerformanceCounter(&t);return (uint64_t)t.QuadPart*1000u/timer_freq;}
static int read_target(uintptr_t address,void *dst,size_t size){SIZE_T got;return ReadProcessMemory(process,(void*)address,dst,size,&got)&&got==size;}
static uintptr_t code_address(uint32_t pc){
 uint32_t a=pc&0x07ffffffu;
 if(a>=0x06000000u)return state+offsetof(saturn,wram_h)+((a-0x06000000u)&(WRAM_H_SIZE-1));
 if(a>=0x00200000u&&a<0x00300000u)return state+offsetof(saturn,wram_l)+(a-0x00200000u);
 if(a<0x00100000u)return state+offsetof(saturn,bios)+(a&(BIOS_SIZE-1));
 return 0;
}
static unsigned slot(unsigned core,uint32_t pc){
 pc&=~31u;unsigned idx=((pc>>5)*2654435761u+core)&(CAP-1);
 for(unsigned n=0;n<CAP;n++,idx=(idx+1)&(CAP-1)){
  if(!bins[idx].pc){bins[idx].pc=pc;bins[idx].core=core;uintptr_t a=code_address(pc);if(a)read_target(a,bins[idx].code,32);nbin++;return idx;}
  if(bins[idx].pc==pc&&bins[idx].core==core)return idx;
 }
 return 0;
}
int main(int argc,char **argv){
 if(argc!=5){fprintf(stderr,"usage: sample_saturn PID g_sys_RVA seconds output-prefix\n");return 2;}
 DWORD pid=(DWORD)strtoul(argv[1],0,0);uintptr_t rva=(uintptr_t)strtoull(argv[2],0,0);
 process=OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process){fprintf(stderr,"OpenProcess failed: %lu\n",GetLastError());return 2;}
 HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,pid);MODULEENTRY32 mod={0};mod.dwSize=sizeof mod;
 if(!Module32First(snap,&mod)){fprintf(stderr,"Module lookup failed: %lu\n",GetLastError());return 2;}CloseHandle(snap);state=(uintptr_t)mod.modBaseAddr+rva;
 char path[1024];snprintf(path,sizeof path,"%s-frames.csv",argv[4]);FILE *f=fopen(path,"w");if(!f)return 2;
 fputs("field,wall_ms,master_pc,slave_pc,samples\n",f);
 uint64_t frame=0,oldframe=0,lastcycles[2]={0,0};uint32_t pc[2]={0,0};
 LARGE_INTEGER freq;QueryPerformanceFrequency(&freq);timer_freq=freq.QuadPart;timeBeginPeriod(1);
 ULONGLONG begin=stamp(),fieldtime=begin,end=begin+strtoul(argv[3],0,0)*1000u;
 unsigned slow=0,samples=0;
 while(stamp()<end){
  ULONGLONG now=stamp();
  if(!read_target(state+offsetof(saturn,frames),&frame,sizeof frame))break;
  if(frame!=oldframe){
   if(oldframe&&frame==oldframe+1&&now-fieldtime>=25){
    for(unsigned j=0;j<npending;j++)bins[pending[j]].hits++;
    fprintf(f,"%llu,%llu,%08X,%08X,%u\n",(unsigned long long)oldframe,(unsigned long long)(now-fieldtime),pc[0],pc[1],npending);slow++;
   }
   oldframe=frame;fieldtime=now;npending=0;
  }
  for(unsigned core=0;core<2;core++){
   uintptr_t cpu=state+(core?offsetof(saturn,slave):offsetof(saturn,master));uint64_t cycles;
   if(!read_target(cpu+offsetof(sh2,pc),&pc[core],4)||!read_target(cpu+offsetof(sh2,cycles),&cycles,8))continue;
   if(cycles!=lastcycles[core]&&pc[core]&&npending<4096)pending[npending++]=slot(core,pc[core]);
   lastcycles[core]=cycles;
  }
  samples++;Sleep(1);
 }
 fclose(f);snprintf(path,sizeof path,"%s-hot.txt",argv[4]);f=fopen(path,"w");if(!f)return 2;
 fprintf(f,"%u samples, %u observed slow fields, %u distinct code blocks. Sampling is approximate.\n",samples,slow,nbin);
 for(unsigned rank=0;rank<40;rank++){
  unsigned best=0;for(unsigned i=1;i<CAP;i++)if(bins[i].hits>bins[best].hits)best=i;
  if(!bins[best].hits)break;
  fprintf(f,"\n%s %08X hits=%u\n",bins[best].core?"slave":"master",bins[best].pc,bins[best].hits);
  for(unsigned i=0;i<32;i+=2){uint16_t op=(bins[best].code[i]<<8)|bins[best].code[i+1];char text[64]="?";sh2_format(op,bins[best].pc+i,text);fprintf(f," %08X %04X %s\n",bins[best].pc+i,op,text);}
  bins[best].hits=0;
 }
 fclose(f);CloseHandle(process);timeEndPeriod(1);printf("Sampled %u times; %u slow fields.\n",samples,slow);return 0;
}
