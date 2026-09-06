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
/* A linear vertex path produces quadratic edge lengths and signed areas.
 * Checking their extrema covers the entire interval, including inversions
 * between the usual quarter-frame samples. No vertices or UV indices move
 * to another corner to make a questionable correspondence fit. */
static inline double geometry_quadratic_min(double a,double b,double c) {
    double value=a,end=a+b+c;
    if(end<value)value=end;
    if(c>0) {
        double t=-b/(2*c);
        if(t>0 && t<1){double at=a+t*(b+t*c);if(at<value)value=at;}
    }
    return value;
}
static inline double geometry_cross_min(const double *p,const double *d,
    unsigned a,unsigned b,unsigned c,double sign) {
    double ax=p[b*2]-p[a*2],ay=p[b*2+1]-p[a*2+1];
    double bx=p[c*2]-p[a*2],by=p[c*2+1]-p[a*2+1];
    double dx=d[b*2]-d[a*2],dy=d[b*2+1]-d[a*2+1];
    double ex=d[c*2]-d[a*2],ey=d[c*2+1]-d[a*2+1];
    return geometry_quadratic_min(sign*(ax*by-ay*bx),
        sign*(dx*by+ax*ey-dy*bx-ay*ex),sign*(dx*ey-dy*ex));
}
static inline int geometry_pair_valid(const saturn_vk_vdp1_op *old,
    const saturn_vk_vdp1_op *cur) {
    double p[8],d[8],q[8],area[3]={0,0,0};
    unsigned vertices[4],count=0;
    for(unsigned k=0;k<8;k++) {
        p[k]=geometry_xy(old,k);q[k]=geometry_xy(cur,k);d[k]=q[k]-p[k];
        if(!isfinite(p[k])||!isfinite(q[k]))return 0;
    }
    /* Saturn triangles repeat a quad corner. Losing or introducing that
     * duplicate is a topology change, not a vertex motion to interpolate. */
    for(unsigned k=0;k<4;k++)for(unsigned j=0;j<k;j++) {
        int before=p[k*2]==p[j*2] && p[k*2+1]==p[j*2+1];
        int after=q[k*2]==q[j*2] && q[k*2+1]==q[j*2+1];
        if(before!=after)return 0;
    }
    for(unsigned k=0;k<4;k++) {
        unsigned j=(k+1)&3;
        if(p[k*2]!=p[j*2]||p[k*2+1]!=p[j*2+1])vertices[count++]=k;
        area[0]+=p[k*2]*p[j*2+1]-p[j*2]*p[k*2+1];
        area[1]+=p[k*2]*d[j*2+1]+d[k*2]*p[j*2+1]-p[j*2]*d[k*2+1]-d[j*2]*p[k*2+1];
        area[2]+=d[k*2]*d[j*2+1]-d[j*2]*d[k*2+1];
    }
    if(count<3||fabs(area[0])<0.000001)return 0;
    double sign=area[0]<0?-1:1;
    if(geometry_quadratic_min(sign*area[0],sign*area[1],sign*area[2])<0.000001)return 0;
    for(unsigned k=0;k<count;k++) {
        unsigned a=vertices[k],b=vertices[(k+1)%count],c=vertices[(k+2)%count];
        double x=p[b*2]-p[a*2],y=p[b*2+1]-p[a*2+1];
        double dx=d[b*2]-d[a*2],dy=d[b*2+1]-d[a*2+1];
        if(geometry_quadratic_min(x*x+y*y,2*(x*dx+y*dy),dx*dx+dy*dy)<0.000001)return 0;
        /* Concave/crossed endpoint quads retain their native presentation;
         * the bilinear presentation surface must stay convex and oriented. */
        if(geometry_cross_min(p,d,a,b,c,sign)<-0.000001)return 0;
    }
    return 1;
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
    if(alpha<0)alpha=0;
    if(alpha>1)alpha=1;
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
        if(!geometry_pair_valid(&prev[j],&cur[i]))continue;
        matched++;
        for(int k=0;k<8;k++) {
            float v=prev[j].xy[k]+((float)cur[i].xy[k]-prev[j].xy[k])*alpha;
            memcpy(&out[i].xy[k],&v,4);
        }
        out[i].flip|=SATURN_GEOMETRY_FLOAT;
    }
    free(head);free(next);free(pc);free(cc);return matched;
}
/* Current and previous shared edges define a connected surface. A missing or
 * contradictory face history holds that entire component at its current
 * geometry. Never borrow only an edge or freeze only a conflicting vertex:
 * either operation can fold an otherwise valid face and tear its neighbors. */
#define SATURN_GEOMETRY_SEAMS (15u<<27)
typedef struct {float xy[4];unsigned a,b,face,edge;} geometry_edge;
static inline int geometry_edge_compare(const void *aa,const void *bb) {
    const geometry_edge *a=aa,*b=bb;
    for(unsigned k=0;k<4;k++)if(a->xy[k]!=b->xy[k])return a->xy[k]<b->xy[k]?-1:1;
    return 0;
}
static inline unsigned geometry_root(unsigned *parent,unsigned n) {
    while(parent[n]!=n){parent[n]=parent[parent[n]];n=parent[n];}return n;
}
static inline void geometry_join(unsigned *parent,unsigned a,unsigned b) {
    parent[geometry_root(parent,b)]=geometry_root(parent,a);
}
static inline unsigned geometry_edges(const saturn_vk_vdp1_op *ops,unsigned n,
    geometry_edge *edges,int history) {
    unsigned count=0;
    for(unsigned i=0;i<n;i++)if(geometry_eligible(&ops[i]) &&
        (!history || (ops[i].flip&SATURN_GEOMETRY_FLOAT)))for(unsigned k=0;k<4;k++) {
        unsigned j=(k+1)&3u,a=i*4+k,b=i*4+j;
        float ax=geometry_xy(&ops[i],k*2),ay=geometry_xy(&ops[i],k*2+1);
        float bx=geometry_xy(&ops[i],j*2),by=geometry_xy(&ops[i],j*2+1);
        if(!isfinite(ax)||!isfinite(ay)||!isfinite(bx)||!isfinite(by))continue;
        if(ax==bx&&ay==by)continue;
        if(ax>bx||(ax==bx&&ay>by)){
            float t=ax;ax=bx;bx=t;t=ay;ay=by;by=t;unsigned u=a;a=b;b=u;
        }
        edges[count++]=(geometry_edge){{ax,ay,bx,by},a,b,i,k};
    }
    qsort(edges,count,sizeof *edges,geometry_edge_compare);
    return count;
}
static inline int geometry_edge_close(const saturn_vk_vdp1_op *ops,
    const geometry_edge *a,const geometry_edge *b,float limit) {
    const unsigned av[2]={a->a,a->b},bv[2]={b->a,b->b};
    for(unsigned k=0;k<2;k++)for(unsigned axis=0;axis<2;axis++) {
        float x=geometry_xy(&ops[av[k]/4],(av[k]&3)*2+axis);
        float y=geometry_xy(&ops[bv[k]/4],(bv[k]&3)*2+axis);
        if(!isfinite(x)||!isfinite(y)||fabsf(x-y)>limit)return 0;
    }
    return 1;
}
static inline unsigned geometry_weld(const saturn_vk_vdp1_op *cur,unsigned n,saturn_vk_vdp1_op *old) {
    if(!n||n>8192)return 0;
    unsigned changed=0;
    geometry_edge *edges=malloc(n*4*sizeof *edges),*history=malloc(n*4*sizeof *history);
    unsigned *parent=malloc(n*sizeof *parent),*seams=calloc(n,sizeof *seams);
    uint8_t *held=calloc(n,1);
    if(!edges||!history||!parent||!seams||!held){
        memcpy(old,cur,n*sizeof *old);goto done;
    }
    for(unsigned i=0;i<n;i++) {
        parent[i]=i;
        held[i]=geometry_eligible(&cur[i]) &&
            (!(old[i].flip&SATURN_GEOMETRY_FLOAT)||!geometry_pair_valid(&old[i],&cur[i]));
    }
    unsigned ne=geometry_edges(cur,n,edges,0),nh=geometry_edges(old,n,history,1);
    for(unsigned i=0;i<ne;) {
        unsigned end=i+1;
        while(end<ne&&!geometry_edge_compare(&edges[i],&edges[end]))end++;
        if(end>i+1) {
            for(unsigned j=i+1;j<end;j++)geometry_join(parent,edges[i].face,edges[j].face);
            if(end!=i+2 || edges[i].face==edges[i+1].face ||
               !geometry_edge_close(old,&edges[i],&edges[i+1],0.01f)) {
                for(unsigned j=i;j<end;j++)held[edges[j].face]=1;
            }
        }
        i=end;
    }
    /* Native pixel-adjacent boundaries can be one coordinate apart. Their
     * histories must retain that adjacency; only verified edges receive the
     * half-pixel coverage flags, never a held component or outer silhouette. */
    for(unsigned i=0;i<ne;i++)for(unsigned j=i+1;j<ne;j++) {
        if(edges[j].xy[0]-edges[i].xy[0]>1)break;
        if(edges[i].face==edges[j].face)continue;
        int close=1,distinct=0;
        for(unsigned k=0;k<4;k++) {
            float distance=edges[i].xy[k]-edges[j].xy[k];
            if(fabsf(distance)>1)close=0;
            distinct|=distance!=0;
        }
        if(!close||!distinct)continue;
        unsigned a=edges[i].face,b=edges[j].face;
        geometry_join(parent,a,b);
        if(!geometry_edge_close(old,&edges[i],&edges[j],1.01f))held[a]=held[b]=1;
        else {
            seams[a]|=1u<<(27+edges[i].edge);
            seams[b]|=1u<<(27+edges[j].edge);
        }
    }
    /* A formerly shared edge cannot silently acquire two unrelated current
     * endpoints. This reverse check detects correspondence/topology changes
     * even when the current faces no longer happen to meet on screen. */
    for(unsigned i=0;i<nh;) {
        unsigned end=i+1;
        while(end<nh&&!geometry_edge_compare(&history[i],&history[end]))end++;
        if(end>i+1) {
            for(unsigned j=i+1;j<end;j++)geometry_join(parent,history[i].face,history[j].face);
            if(end!=i+2 || history[i].face==history[i+1].face ||
               !geometry_edge_close(cur,&history[i],&history[i+1],1.01f)) {
                for(unsigned j=i;j<end;j++)held[history[j].face]=1;
            }
        }
        i=end;
    }
    for(unsigned i=0;i<n;i++)if(held[i])held[geometry_root(parent,i)]=1;
    for(unsigned i=0;i<n;i++)if(geometry_eligible(&cur[i])) {
        if(held[geometry_root(parent,i)]) {
            for(unsigned k=0;k<4;k++)
                changed+=geometry_xy(&old[i],k*2)!=geometry_xy(&cur[i],k*2) ||
                         geometry_xy(&old[i],k*2+1)!=geometry_xy(&cur[i],k*2+1);
            old[i]=cur[i];
        } else old[i].flip=(old[i].flip&~SATURN_GEOMETRY_SEAMS)|seams[i];
    }
done:
    free(edges);free(history);free(parent);free(seams);free(held);return changed;
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
