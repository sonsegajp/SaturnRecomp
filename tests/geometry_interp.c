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
    if(alpha<0)alpha=0;
    if(alpha>1)alpha=1;
    for(unsigned i=0;i<cn;i++) {
        int j=geometry_nearest(&cur[i],prev,pn);
        if(j<0 || geometry_nearest(&prev[j],cur,cn)!=(int)i)continue;
        if(!geometry_pair_valid(&prev[j],&cur[i]))continue;
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

static saturn_vk_vdp1_op history(saturn_vk_vdp1_op value) {
    for(unsigned k=0;k<8;k++){float xy=(float)value.xy[k];memcpy(&value.xy[k],&xy,4);}
    value.flip|=SATURN_GEOMETRY_FLOAT;return value;
}

static int shape_safety(void) {
    saturn_vk_vdp1_op p=quad(0,1),c=p,o;
    const int32_t bowtie[8]={0,0,10,10,0,10,8,0};
    memcpy(c.xy,bowtie,sizeof bowtie);
    CHECK(geometry_area(&p)>0 && geometry_area(&c)>0);
    CHECK(!geometry_pair_valid(&p,&c));
    CHECK(geometry_interpolate(&p,1,&c,1,.5f,&o)==0);
    CHECK(!memcmp(&o,&c,sizeof o));

    /* Both endpoints and all quarter samples have positive area, but the
     * triangle inverts between t=.31 and t=.32. Sampling is insufficient. */
    const int32_t triangle_before[8]={0,0,31,0,0,32,0,0};
    const int32_t triangle_after[8]={0,0,-69,0,0,-68,0,0};
    memcpy(p.xy,triangle_before,sizeof p.xy);memcpy(c.xy,triangle_after,sizeof c.xy);
    for(unsigned i=0;i<=4;i++){float t=i*.25f;CHECK((31-100*t)*(32-100*t)>0);}
    CHECK(!geometry_pair_valid(&p,&c));
    CHECK(geometry_interpolate(&p,1,&c,1,.5f,&o)==0);

    /* A valid quarter turn keeps the original vertex/UV indexing. */
    p=quad(0,1);c=p;
    for(unsigned k=0;k<4;k++)for(unsigned axis=0;axis<2;axis++)c.xy[k*2+axis]=p.xy[((k+1)&3)*2+axis];
    CHECK(geometry_pair_valid(&p,&c));
    CHECK(geometry_interpolate(&p,1,&c,1,.25f,&o)==1);
    CHECK(geometry_xy(&o,0)==2.5f && geometry_xy(&o,1)==0);
    CHECK((o.flip&~SATURN_GEOMETRY_FLOAT)==c.flip && o.chr==c.chr);
    return 0;
}

static int component_safety(void) {
    saturn_vk_vdp1_op c[4]={quad(0,1),quad(10,2),quad(20,3),quad(100,4)},p[4],o[4];
    for(unsigned i=0;i<4;i++){p[i]=c[i];for(unsigned k=0;k<8;k+=2)p[i].xy[k]-=2;}
    p[2].chr=99;
    CHECK(geometry_interpolate(p,4,c,4,0,o)==3);
    geometry_weld(c,4,o);
    for(unsigned i=0;i<3;i++)CHECK(!memcmp(&o[i],&c[i],sizeof o[i]));
    CHECK(o[3].flip&SATURN_GEOMETRY_FLOAT);
    CHECK(geometry_xy(&o[3],0)==98 && geometry_xy(&o[3],2)==108);

    /* Shared histories that split apart now must also be rejected when the
     * current screen-space graph alone would no longer connect the faces. */
    p[0]=quad(0,1);p[1]=quad(10,2);c[0]=quad(0,1);c[1]=quad(14,2);
    CHECK(geometry_interpolate(p,2,c,2,0,o)==2);geometry_weld(c,2,o);
    CHECK(!memcmp(o,c,2*sizeof *o));

    /* Cyclic index order on the neighboring face is preserved; adjacency
     * compares corresponding endpoints without reordering texture corners. */
    c[0]=quad(0,1);c[1]=quad(10,2);p[0]=quad(-2,1);p[1]=quad(8,2);
    saturn_vk_vdp1_op pc=p[1],cc=c[1];
    for(unsigned k=0;k<4;k++)for(unsigned axis=0;axis<2;axis++){
        p[1].xy[k*2+axis]=pc.xy[((k+2)&3)*2+axis];
        c[1].xy[k*2+axis]=cc.xy[((k+2)&3)*2+axis];
    }
    CHECK(geometry_interpolate(p,2,c,2,0,o)==2);geometry_weld(c,2,o);
    CHECK((o[0].flip&SATURN_GEOMETRY_FLOAT)&&(o[1].flip&SATURN_GEOMETRY_FLOAT));
    for(unsigned k=0;k<8;k++)CHECK(geometry_xy(&o[1],k)==p[1].xy[k]);

    /* Three incident faces make a shared edge ambiguous even when their
     * endpoint histories happen to agree. Unrelated motion still survives. */
    c[0]=quad(0,1);c[1]=quad(10,2);c[2]=quad(10,3);c[3]=quad(100,4);
    for(unsigned i=0;i<4;i++){p[i]=c[i];for(unsigned k=0;k<8;k+=2)p[i].xy[k]-=2;}
    CHECK(geometry_interpolate(p,4,c,4,0,o)==4);geometry_weld(c,4,o);
    for(unsigned i=0;i<3;i++)CHECK(!memcmp(&o[i],&c[i],sizeof o[i]));
    CHECK((o[3].flip&SATURN_GEOMETRY_FLOAT)&&geometry_xy(&o[3],0)==98);
    return 0;
}

static int sonic_r_capture_safety(void) {
    /* Minimized from moving-edge-audit, faces 4/5. The previous weld moved
     * only the shared endpoints of the unmatched face, distorting its shape. */
    saturn_vk_vdp1_op c[2]={quad(0,390880),quad(0,390880)},o[2];
    const int32_t current_a[8]={179,176,186,175,184,172,179,176};
    const int32_t current_b[8]={186,175,189,179,184,172,186,175};
    memcpy(c[0].xy,current_a,sizeof current_a);memcpy(c[1].xy,current_b,sizeof current_b);
    o[0]=c[0];o[0].xy[5]=173;o[0]=history(o[0]);o[1]=c[1];
    CHECK(geometry_pair_valid(&o[0],&c[0]));geometry_weld(c,2,o);
    CHECK(!memcmp(c,o,sizeof c));

    /* Face 25's current A/D corner is duplicated but its matched history
     * contains two different positions. Never repair just one occurrence. */
    const int32_t current_triangle[8]={159,170,164,166,164,164,159,170};
    const int32_t mismatched_triangle[8]={159,171,164,166,164,165,159,170};
    memcpy(c[0].xy,current_triangle,sizeof current_triangle);
    o[0]=c[0];memcpy(o[0].xy,mismatched_triangle,sizeof mismatched_triangle);
    CHECK(!geometry_pair_valid(&o[0],&c[0]));o[0]=history(o[0]);
    geometry_weld(c,1,o);CHECK(!memcmp(c,o,sizeof *c));
    return 0;
}

int main(void){
    CHECK(matching_equivalence());
    CHECK(!shape_safety());CHECK(!component_safety());CHECK(!sonic_r_capture_safety());
    saturn_vk_vdp1_op p[2]={quad(0,1),quad(100,2)},c[2]={quad(101,2),quad(1,1)},o[2];
    CHECK(geometry_interpolate(p,2,c,2,0.25f,o)==2);
    CHECK(geometry_xy(&o[1],0)==0.25f);CHECK(c[1].xy[0]==1);
    p[0]=quad(0,1);p[1]=quad(2,1);c[0]=quad(1,1);
    CHECK(geometry_interpolate(p,2,c,1,0.5f,o)==0);
    c[0]=quad(500,1);CHECK(geometry_interpolate(p,2,c,1,0.5f,o)==0);
    c[0]=quad(1,1);c[0].flip=0;CHECK(!geometry_eligible(&c[0]));
    /* Incomplete or conflicting shared history holds whole faces, while
     * point-only overlaps remain independent. */
    c[0]=quad(0,1);c[1]=quad(10,2);p[0]=quad(-2,1);p[1]=quad(8,99);
    CHECK(geometry_interpolate(p,2,c,2,0,o)==1);
    CHECK(geometry_weld(c,2,o)>0);
    CHECK(geometry_xy(&o[0],2)==geometry_xy(&o[1],0));
    CHECK(geometry_xy(&o[0],4)==geometry_xy(&o[1],6));
    CHECK(!memcmp(o,c,sizeof o));
    p[1]=quad(9,2);CHECK(geometry_interpolate(p,2,c,2,0,o)==2);
    geometry_weld(c,2,o);
    CHECK(!memcmp(o,c,sizeof o));
    c[1]=quad(10,2);for(int k=0;k<4;k++)c[1].xy[k*2+1]+=10;
    p[1]=c[1];p[0]=quad(-2,1);geometry_interpolate(p,2,c,2,0,o);geometry_weld(c,2,o);
    CHECK(geometry_xy(&o[0],4)==8); /* only a coincident corner: no weld */
    c[0]=quad(0,1);c[1]=quad(11,2);p[0]=quad(-1,1);p[1]=quad(10,2);
    geometry_interpolate(p,2,c,2,0,o);geometry_weld(c,2,o);
    CHECK(o[0].flip&(1u<<28));CHECK(o[1].flip&(1u<<30));
    CHECK(!(o[0].flip&(1u<<30))); /* outer silhouette is not expanded */
    CHECK((o[0].flip&SATURN_GEOMETRY_FLOAT)&&(o[1].flip&SATURN_GEOMETRY_FLOAT));
    geometry_clock clock={0,30,1,144};unsigned frames=0;
    for(int i=0;i<30000;i++){unsigned n=geometry_bank(&clock);for(unsigned k=0;k<n;k++){double a=geometry_pay(&clock);CHECK(a>0&&a<=1);frames++;}}
    CHECK(frames==144000 && clock.credit==0);
    clock=(geometry_clock){0,28636360,1820*263,120};frames=0;
    for(int i=0;i<100000;i++){unsigned n=geometry_bank(&clock);for(unsigned k=0;k<n;k++){double a=geometry_pay(&clock);CHECK(a>0&&a<=1);frames++;}}
    CHECK(frames==(uint64_t)100000*120*1820*263/28636360);
    puts("PASS geometry matching, continuous shape safety, component fallback, Sonic R captures, exact presentation budget");return 0;
}
