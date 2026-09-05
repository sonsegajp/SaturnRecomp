#include "geometry_interp.h"
#include <stdio.h>
static saturn_vk_vdp1_op quad(int x,unsigned chr) {
    saturn_vk_vdp1_op o={0};o.kind=1;o.textured=1;o.chr=chr;o.tw=o.th=16;o.flip=2<<8;
    for(int k=0;k<4;k++){o.xy[k*2]=x+(k==1||k==2?10:0);o.xy[k*2+1]=k>=2?10:0;}
    return o;
}
#define CHECK(c) do{if(!(c)){printf("FAIL line %d\n",__LINE__);return 1;}}while(0)
static inline unsigned geometry_interpolate_reference(const saturn_vk_vdp1_op *prev,unsigned pn,
    const saturn_vk_vdp1_op *cur,unsigned cn,float alpha,saturn_vk_vdp1_op *out) {
    unsigned matched=0;
    memcpy(out,cur,cn*sizeof(*out));
    if(alpha<0)alpha=0;if(alpha>1)alpha=1;
    for(unsigned i=0;i<cn;i++) {
        int j=geometry_nearest(&cur[i],prev,pn);
        if(j<0 || geometry_nearest(&prev[j],cur,cn)!=(int)i)continue;
        matched++;
        for(int k=0;k<8;k++) {
            float v=prev[j].xy[k]+((float)cur[i].xy[k]-prev[j].xy[k])*alpha;
            memcpy(&out[i].xy[k],&v,4);
        }
        out[i].flip|=SATURN_GEOMETRY_FLOAT;
    }
    return matched;
}

static unsigned rng_state=17;
static unsigned random_word(void){rng_state=rng_state*1664525u+1013904223u;return rng_state;}
static int matching_equivalence(void) {
    saturn_vk_vdp1_op p[128],c[128],a[128],b[128];
    for(unsigned trial=0;trial<1000;trial++) {
        unsigned pn=random_word()%128+1,cn=random_word()%128+1;
        for(unsigned i=0;i<pn;i++) {
            p[i]=quad((int)(random_word()%600)-300,random_word()%12);
            if(!(trial%7))p[i].flip=0;
        }
        for(unsigned i=0;i<cn;i++) {
            c[i]=p[random_word()%pn];
            int delta=(int)(random_word()%11)-5;
            for(unsigned k=0;k<8;k+=2)c[i].xy[k]+=delta;
            if(!(i%9))c[i].colr^=1;
        }
        float alpha=(trial%5)*0.25f;
        unsigned na=geometry_interpolate_reference(p,pn,c,cn,alpha,a);
        unsigned nb=geometry_interpolate(p,pn,c,cn,alpha,b);
        if(na!=nb||memcmp(a,b,cn*sizeof *a)){printf("matcher mismatch trial %u\n",trial);return 0;}
    }
    return 1;
}

int main(void){
    CHECK(matching_equivalence());
    saturn_vk_vdp1_op p[2]={quad(0,1),quad(100,2)},c[2]={quad(101,2),quad(1,1)},o[2];
    CHECK(geometry_interpolate(p,2,c,2,0.25f,o)==2);
    CHECK(geometry_xy(&o[1],0)==0.25f);CHECK(c[1].xy[0]==1);
    p[0]=quad(0,1);p[1]=quad(2,1);c[0]=quad(1,1);
    CHECK(geometry_interpolate(p,2,c,1,0.5f,o)==0);
    c[0]=quad(500,1);CHECK(geometry_interpolate(p,2,c,1,0.5f,o)==0);
    c[0]=quad(1,1);c[0].flip=0;CHECK(!geometry_eligible(&c[0]));
    /* Two adjoining faces must have identical endpoint histories even if
     * only one face could be matched. Point-only overlaps stay independent. */
    c[0]=quad(0,1);c[1]=quad(10,2);p[0]=quad(-2,1);p[1]=quad(8,99);
    CHECK(geometry_interpolate(p,2,c,2,0,o)==1);
    CHECK(geometry_weld(c,2,o)>0);
    CHECK(geometry_xy(&o[0],2)==geometry_xy(&o[1],0));
    CHECK(geometry_xy(&o[0],4)==geometry_xy(&o[1],6));
    CHECK(geometry_xy(&o[1],2)==20); /* unmatched outside edge stays put */
    p[1]=quad(9,2);CHECK(geometry_interpolate(p,2,c,2,0,o)==2);
    geometry_weld(c,2,o);
    CHECK(geometry_xy(&o[0],2)==10 && geometry_xy(&o[1],0)==10);
    c[1]=quad(10,2);for(int k=0;k<4;k++)c[1].xy[k*2+1]+=10;
    p[1]=c[1];p[0]=quad(-2,1);geometry_interpolate(p,2,c,2,0,o);geometry_weld(c,2,o);
    CHECK(geometry_xy(&o[0],4)==8); /* only a coincident corner: no weld */
    c[0]=quad(0,1);c[1]=quad(11,2);p[0]=quad(-1,1);p[1]=quad(10,2);
    geometry_interpolate(p,2,c,2,0,o);geometry_weld(c,2,o);
    CHECK(o[0].flip&(1u<<28));CHECK(o[1].flip&(1u<<30));
    CHECK(!(o[0].flip&(1u<<30))); /* outer silhouette is not expanded */
    geometry_clock clock={0,30,1,144};unsigned frames=0;
    for(int i=0;i<30000;i++){unsigned n=geometry_bank(&clock);for(unsigned k=0;k<n;k++){double a=geometry_pay(&clock);CHECK(a>0&&a<=1);frames++;}}
    CHECK(frames==144000 && clock.credit==0);
    clock=(geometry_clock){0,28636360,1820*263,120};frames=0;
    for(int i=0;i<100000;i++){unsigned n=geometry_bank(&clock);for(unsigned k=0;k<n;k++){double a=geometry_pay(&clock);CHECK(a>0&&a<=1);frames++;}}
    CHECK(frames==(uint64_t)100000*120*1820*263/28636360);
    puts("PASS geometry matching, ambiguity rejection, fractional XY, exact presentation budget");return 0;
}