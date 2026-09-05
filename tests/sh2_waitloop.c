#include "saturn.h"
#include <stdio.h>
#include <string.h>
static saturn a,b;
static int fails;
static int slave_case;
#define CODE 0x06004004u
static void seed(saturn *s,int pattern,int cache){
    saturn_init(s);sh2_reset(&s->master,s,0,CODE,0x06010000);
    s->master.r[2]=0x26008000;s->master.r[1]=1000;
    unsigned short poll[]={0x6121,0x611D,0x2118,0x8DFB,0xE100,0x0009,0xAFFE,0x0009};
    unsigned short count[]={0x4110,0x8BFD,0x0009,0xAFFE,0x0009};
    unsigned short *ops=pattern?poll:count;int n=pattern?8:5;
    for(int i=0;i<n;i++)bus_w16(s,CODE+i*2,ops[i]);
    s->master.onchip[0x92]=cache;
}
static void compare(unsigned clocks){
    sh2 *x=slave_case?&a.slave:&a.master,*y=slave_case?&b.slave:&b.master;
    sh2_run(x,clocks);y->run_target+=clocks;
    while(y->cycles<y->run_target&&!y->halted)sh2_step(y);
    if(x->pc!=y->pc||x->sr!=y->sr||x->cycles!=y->cycles||
       memcmp(x->r,y->r,sizeof x->r)||memcmp(x->cache_data,y->cache_data,sizeof x->cache_data)||
       memcmp(x->cache_lru,y->cache_lru,sizeof x->cache_lru)||
       memcmp(x->cache_tag,y->cache_tag,sizeof x->cache_tag)||
       memcmp(x->cache_valid,y->cache_valid,sizeof x->cache_valid)||
       memcmp(x->onchip,y->onchip,sizeof x->onchip)){
        if(fails++<5)printf("FAIL budget %u: PC %08X/%08X cycles %llu/%llu R1 %u/%u\n",clocks,x->pc,y->pc,(unsigned long long)x->cycles,(unsigned long long)y->cycles,x->r[1],y->r[1]);
    }
}
static void seed_flag(saturn *s, int cache, uint32_t address)
{
    static const uint16_t code[]={0x6090,0xC980,0x2008,0x89FB,0xE301,0xAFFE,0x0009};
    seed(s,0,cache);
    s->master.r[9]=address;
    s->master.r[0]=0xABCD1234;
    for(unsigned i=0;i<sizeof code/sizeof code[0];i++)bus_w16(s,CODE+i*2,code[i]);
    if(slave_case){s->slave=s->master;s->slave.is_slave=1;s->slave.sys=s;}
}
int main(void){
 for(int cache=0;cache<=1;cache++)for(int pattern=0;pattern<=1;pattern++){
    seed(&a,pattern,cache);seed(&b,pattern,cache);
    for(unsigned n=1;n<=257;n++)compare(n);
    if(pattern){bus_w16(&a,0x26008000,1);bus_w16(&b,0x26008000,1);compare(127);}
 }
 /* Counter endpoints include the unsigned wrap case; the fold must leave
  * the terminating iteration for the ordinary interpreter. */
 for(unsigned i=0;i<4;i++){
    static const unsigned counts[]={0,1,2,0xFFFFFFFFu};
    seed(&a,0,1);seed(&b,0,1);a.master.r[1]=b.master.r[1]=counts[i];
    compare(127);compare(129);
 }
 /* A side-effectful device read cannot be replaced by a stable RAM read. */
 seed(&a,1,0);seed(&b,1,0);
 a.master.r[2]=b.master.r[2]=0x25B00404u;compare(127);
 /* An aliased destination destroys the address and cannot be folded. */
 seed(&a,1,0);seed(&b,1,0);
 a.master.r[1]=b.master.r[1]=0x26008000;
 bus_w16(&a,CODE,0x6111);bus_w16(&b,CODE,0x6111);compare(127);
 /* A changed mailbox must exit instead of being treated as an idle loop. */
 seed(&a,1,0);seed(&b,1,0);bus_w16(&a,0x26008000,0x8000);bus_w16(&b,0x26008000,0x8000);compare(13);
 /* A write to code in the cache-through region remains visible. */
 seed(&a,1,0);seed(&b,1,0);compare(100);bus_w16(&a,CODE+2,0x611C);bus_w16(&b,CODE+2,0x611C);compare(131);
 for(slave_case=0;slave_case<=1;slave_case++)for(int cache=0;cache<=1;cache++){
    seed_flag(&a,cache,0xFFFFFE11u);seed_flag(&b,cache,0xFFFFFE11u);
    for(unsigned clocks=1;clocks<=129;clocks++)compare(clocks);
    /* Another CPU's capture becomes visible at the next scheduler slice. */
    frt_capture(slave_case?&a.slave:&a.master);
    frt_capture(slave_case?&b.slave:&b.master);
    compare(128);
    seed_flag(&a,cache,0x26008000u);seed_flag(&b,cache,0x26008000u);
    compare(127);bus_w8(&a,0x26008000u,0x80);bus_w8(&b,0x26008000u,0x80);compare(128);
    /* A live FRC counter is explicitly excluded from the FTCSR shortcut. */
    seed_flag(&a,cache,0xFFFFFE12u);seed_flag(&b,cache,0xFFFFFE12u);compare(129);
 }
 slave_case=0;
 printf("%s scheduler-bounded wait loops: %d divergence(s)\n",fails?"FAIL":"PASS",fails);
 return fails!=0;
}