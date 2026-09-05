#ifndef SATURN_GEOMETRY_INTERP_H
#define SATURN_GEOMETRY_INTERP_H
#include "vulkan_renderer.h"
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
/* Presentation-only vertices. Guest command RAM always retains integer XY. */
#define SATURN_GEOMETRY_FLOAT (1u<<31)
static inline float geometry_xy(const saturn_vk_vdp1_op *o,unsigned k) {
    if(o->flip & SATURN_GEOMETRY_FLOAT){float f;memcpy(&f,&o->xy[k],4);return f;}
    return (float)o->xy[k];
}
static inline int geometry_eligible(const saturn_vk_vdp1_op *o) {
    unsigned type=(o->flip>>8)&15;
    return o->kind==SATURN_VK_VDP1_QUAD && (type==2 || type==4);
}
/* Texture addresses may be reused for different uploads between frames. */
static inline uint32_t geometry_texture_hash(const saturn_vk_vdp1_op *o,const uint8_t *ram) {
    unsigned mode=(o->pmod>>3)&7;
    uint32_t count=o->tw*o->th,hash=2166136261u;
    count=mode<=1 ? (count+1)/2 : mode<=4 ? count : count*2;
    if(count>0x80000u)return 0;
    for(uint32_t i=0;i<count;i++)hash=(hash^ram[(o->chr+i)&0x7FFFFu])*16777619u;
    if(mode==1)for(unsigned i=0;i<32;i++)hash=(hash^ram[(o->colr*8+i)&0x7FFFFu])*16777619u;
    return hash;
}
static inline int64_t geometry_area(const saturn_vk_vdp1_op *o) {
    int64_t area=0;
    for(unsigned k=0;k<4;k++){unsigned j=(k+1)&3;area+=(int64_t)o->xy[k*2]*o->xy[j*2+1]-(int64_t)o->xy[j*2]*o->xy[k*2+1];}
    return area;
}
static inline int geometry_same(const saturn_vk_vdp1_op *a,const saturn_vk_vdp1_op *b) {
    if(!(geometry_eligible(a)&&geometry_eligible(b)&&a->textured==b->textured &&
        a->chr==b->chr&&a->tw==b->tw&&a->th==b->th&&a->colr==b->colr&&
        a->pmod==b->pmod&&a->flat==b->flat&&a->flip==b->flip))return 0;
    int64_t aa=geometry_area(a),ba=geometry_area(b);
    return aa && ba && ((aa<0)==(ba<0));
}
static inline int64_t geometry_distance(const saturn_vk_vdp1_op *a,const saturn_vk_vdp1_op *b) {
    int64_t d=0;
    for(int k=0;k<8;k++){int64_t x=(int64_t)a->xy[k]-b->xy[k]; d+=x*x;}
    return d;
}
/* Identity + nearest position, with a mutual unique match. List position is
 * never an identity and equal/near-equal alternatives are deliberately held. */
static inline int geometry_nearest(const saturn_vk_vdp1_op *a,const saturn_vk_vdp1_op *list,unsigned n) {
    int best=-1;int64_t first=INT64_MAX,second=INT64_MAX;
    for(unsigned i=0;i<n;i++)if(geometry_same(a,&list[i])) {
        int64_t d=geometry_distance(a,&list[i]);
        if(d<first){second=first;first=d;best=(int)i;}else if(d<second)second=d;
    }
    if(first>8*64*64 || second-first<=8 || (second!=INT64_MAX && first*4>=second*3))return -1;
    return best;
}
/* Material buckets only prune impossible candidates. Collisions still use the
 * full identity check; both reciprocal nearest choices share one distance. */
typedef struct {int best;int64_t first,second;} geometry_choice;
static inline uint32_t geometry_material_key(const saturn_vk_vdp1_op *o) {
    const uint32_t v[]={o->textured,o->chr,o->tw,o->th,o->colr,o->pmod,o->flat,o->flip};
    uint32_t h=2166136261u;
    for(unsigned i=0;i<8;i++)h=(h^v[i])*16777619u;
    h^=h>>16;return h;
}
static inline void geometry_consider(geometry_choice *c,int index,int64_t d) {
    if(d<c->first){c->second=c->first;c->first=d;c->best=index;}
    else if(d<c->second)c->second=d;
}
static inline int geometry_choice_index(const geometry_choice *c) {
    if(c->best<0 || c->first>8*64*64 || c->second-c->first<=8 ||
       (c->second!=INT64_MAX && c->first*4>=c->second*3))return -1;
    return c->best;
}
static inline unsigned geometry_interpolate(const saturn_vk_vdp1_op *prev,unsigned pn,
    const saturn_vk_vdp1_op *cur,unsigned cn,float alpha,saturn_vk_vdp1_op *out) {
    unsigned matched=0,buckets=1;
    memcpy(out,cur,cn*sizeof(*out));
    if(!pn || !cn)return 0;
    if(alpha<0)alpha=0;if(alpha>1)alpha=1;
    while(buckets<pn && buckets<16384)buckets<<=1;
    int *head=malloc(buckets*sizeof *head),*next=malloc(pn*sizeof *next);
    geometry_choice *pc=malloc(pn*sizeof *pc),*cc=malloc(cn*sizeof *cc);
    if(head && next && pc && cc) {
        for(unsigned i=0;i<buckets;i++)head[i]=-1;
        for(unsigned j=0;j<pn;j++) {
            pc[j]=(geometry_choice){-1,INT64_MAX,INT64_MAX};
            if(!geometry_eligible(&prev[j]))continue;
            unsigned key=geometry_material_key(&prev[j])&(buckets-1);
            next[j]=head[key];head[key]=(int)j;
        }
        for(unsigned i=0;i<cn;i++) {
            cc[i]=(geometry_choice){-1,INT64_MAX,INT64_MAX};
            if(!geometry_eligible(&cur[i]))continue;
            unsigned key=geometry_material_key(&cur[i])&(buckets-1);
            for(int j=head[key];j>=0;j=next[j])if(geometry_same(&cur[i],&prev[j])) {
                int64_t d=geometry_distance(&cur[i],&prev[j]);
                geometry_consider(&cc[i],j,d);geometry_consider(&pc[j],(int)i,d);
            }
        }
    }
    for(unsigned i=0;i<cn;i++) {
        int j=head&&next&&pc&&cc?geometry_choice_index(&cc[i]):geometry_nearest(&cur[i],prev,pn);
        if(j<0)continue;
        int reverse=head&&next&&pc&&cc?geometry_choice_index(&pc[j]):geometry_nearest(&prev[j],cur,cn);
        if(reverse!=(int)i)continue;
        matched++;
        for(int k=0;k<8;k++) {
            float v=prev[j].xy[k]+((float)cur[i].xy[k]-prev[j].xy[k])*alpha;
            memcpy(&out[i].xy[k],&v,4);
        }
        out[i].flip|=SATURN_GEOMETRY_FLOAT;
    }
    free(head);free(next);free(pc);free(cc);return matched;
}
/* A shared screen-space EDGE supplies topology evidence; a coincident point
 * alone does not (unrelated objects can overlap). Weld its endpoint histories
 * before interpolation. Unmatched neighbors inherit an unambiguous endpoint;
 * conflicting histories hold that endpoint for every connected face. */
typedef struct {int32_t xy[4];unsigned a,b,face,edge;} geometry_edge;
static inline int geometry_edge_compare(const void *aa,const void *bb) {
    const geometry_edge *a=aa,*b=bb;
    for(unsigned k=0;k<4;k++)if(a->xy[k]!=b->xy[k])return a->xy[k]<b->xy[k]?-1:1;
    return 0;
}
static inline unsigned geometry_root(unsigned *parent,unsigned n) {
    while(parent[n]!=n){parent[n]=parent[parent[n]];n=parent[n];}return n;
}
static inline unsigned geometry_weld(const saturn_vk_vdp1_op *cur,unsigned n,saturn_vk_vdp1_op *old) {
    if(!n||n>8192)return 0;
    unsigned count=n*4,ne=0,changed=0;
    geometry_edge *edges=malloc(count*sizeof *edges);
    unsigned *parent=malloc(count*sizeof *parent),*votes=calloc(count,sizeof *votes);
    float *xy=calloc(count*2,sizeof *xy);uint8_t *conflict=calloc(count,1);
    if(!edges||!parent||!votes||!xy||!conflict)goto done;
    for(unsigned a=0;a<count;a++)parent[a]=a;
    for(unsigned i=0;i<n;i++)if(geometry_eligible(&cur[i]))for(unsigned k=0;k<4;k++) {
        unsigned j=(k+1)&3u,a=i*4+k,b=i*4+j;
        int32_t ax=cur[i].xy[k*2],ay=cur[i].xy[k*2+1],bx=cur[i].xy[j*2],by=cur[i].xy[j*2+1];
        if(ax==bx&&ay==by)continue;
        if(ax>bx||(ax==bx&&ay>by)){int32_t t=ax;ax=bx;bx=t;t=ay;ay=by;by=t;unsigned u=a;a=b;b=u;}
        edges[ne++]=(geometry_edge){{ax,ay,bx,by},a,b,i,k};
    }
    for(unsigned i=0;i<n;i++)if(geometry_eligible(&cur[i]))for(unsigned k=0;k<4;k++)for(unsigned j=0;j<k;j++)
        if(cur[i].xy[k*2]==cur[i].xy[j*2]&&cur[i].xy[k*2+1]==cur[i].xy[j*2+1])
            parent[geometry_root(parent,i*4+k)]=geometry_root(parent,i*4+j);
    qsort(edges,ne,sizeof *edges,geometry_edge_compare);
    /* Native pixel-adjacent edges may be one coordinate apart, rather
     * than mathematically coincident. Mark only pairs that are adjacent in
     * both histories for half-pixel seam coverage in the generated picture. */
    for(unsigned i=0;i<ne;i++)for(unsigned j=i+1;j<ne;j++) {
        if(edges[j].xy[0]-edges[i].xy[0]>1)break;
        if(edges[i].face==edges[j].face)continue;
        int close=1,distinct=0;
        for(unsigned k=0;k<4;k++){int64_t d=(int64_t)edges[i].xy[k]-edges[j].xy[k];if(d < -1 || d > 1)close=0;distinct|=d!=0;}
        if(!close||!distinct)continue;
        const unsigned ai[2]={edges[i].a,edges[i].b},bi[2]={edges[j].a,edges[j].b};
        for(unsigned k=0;k<2;k++)for(unsigned xyidx=0;xyidx<2;xyidx++)
            if(fabsf(geometry_xy(&old[ai[k]/4],(ai[k]&3)*2+xyidx)-geometry_xy(&old[bi[k]/4],(bi[k]&3)*2+xyidx))>1.01f)close=0;
        if(close){old[edges[i].face].flip|=1u<<(27+edges[i].edge);old[edges[j].face].flip|=1u<<(27+edges[j].edge);}
    }
    for(unsigned i=0;i<ne;) {
        unsigned j=i+1;while(j<ne&&!geometry_edge_compare(&edges[i],&edges[j]))j++;
        /* More than two incident faces is ambiguous, so do not weld it. */
        if(j==i+2&&edges[i].face!=edges[i+1].face) {
            unsigned a=geometry_root(parent,edges[i].a),b=geometry_root(parent,edges[i+1].a);parent[b]=a;
            a=geometry_root(parent,edges[i].b);b=geometry_root(parent,edges[i+1].b);parent[b]=a;
        }
        i=j;
    }
    for(unsigned a=0;a<count;a++)if(geometry_eligible(&cur[a/4])&&(old[a/4].flip&SATURN_GEOMETRY_FLOAT)) {
        unsigned r=geometry_root(parent,a);float x=geometry_xy(&old[a/4],(a&3)*2),y=geometry_xy(&old[a/4],(a&3)*2+1);
        if(votes[r] && (fabsf(xy[r*2]-x)>0.01f||fabsf(xy[r*2+1]-y)>0.01f))conflict[r]=1;
        else {xy[r*2]=x;xy[r*2+1]=y;}votes[r]++;
    }
    for(unsigned i=0;i<n;i++)if(geometry_eligible(&cur[i])) {
        float values[8];int converted=(old[i].flip&SATURN_GEOMETRY_FLOAT)!=0;
        for(unsigned k=0;k<8;k++)values[k]=geometry_xy(&old[i],k);
        for(unsigned k=0;k<4;k++) {
            unsigned root=geometry_root(parent,i*4+k);
            if(!votes[root])continue;
            float x=conflict[root]?(float)cur[i].xy[k*2]:xy[root*2];
            float y=conflict[root]?(float)cur[i].xy[k*2+1]:xy[root*2+1];
            changed+=values[k*2]!=x||values[k*2+1]!=y;
            values[k*2]=x;values[k*2+1]=y;converted=1;
        }
        if(converted){memcpy(old[i].xy,values,sizeof values);old[i].flip|=SATURN_GEOMETRY_FLOAT;}
    }
done:
    free(edges);free(parent);free(votes);free(xy);free(conflict);return changed;
}
/* Integer presentation budget: rate_num/rate_den logic ticks per second.
 * Alpha is derived from the same remainder; no rounded frames-per-tick. */
typedef struct {uint64_t credit,rate_num,rate_den,hz;} geometry_clock;
static inline unsigned geometry_bank(geometry_clock *c) {
    c->credit+=c->hz*c->rate_den;
    return (unsigned)(c->credit/c->rate_num);
}
static inline double geometry_pay(geometry_clock *c) {
    c->credit-=c->rate_num;
    return 1.0-(double)c->credit/(double)(c->hz*c->rate_den);
}
#endif