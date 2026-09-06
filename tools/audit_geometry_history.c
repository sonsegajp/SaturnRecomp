/* Audit an existing SATURN_INTERP_AUDIT before/current pair without running
 * guest code or a GPU. Captured operations remain local game data.
 * Build: gcc -O2 -std=c11 -Irunner/include tools/audit_geometry_history.c -o out/audit_geometry_history.exe
 * Run: out/audit_geometry_history.exe <capture-prefix> [output.ops]
 */
#include "geometry_interp.h"
#include <stdio.h>

static saturn_vk_vdp1_op current[8192],before[8192],after[8192];
static geometry_edge edges[8192*4];

static unsigned load(const char *prefix,const char *suffix,saturn_vk_vdp1_op *ops) {
    char path[1024];
    if(snprintf(path,sizeof path,"%s-%s.ops",prefix,suffix)>=(int)sizeof path)return 0;
    FILE *file=fopen(path,"rb");
    if(!file){perror(path);return 0;}
    unsigned count=(unsigned)fread(ops,sizeof *ops,8192,file);
    if(fgetc(file)!=EOF)count=0;
    fclose(file);return count;
}

static double area(const saturn_vk_vdp1_op *op) {
    double value=0;
    for(unsigned k=0;k<4;k++) {
        unsigned j=(k+1)&3;
        value+=(double)geometry_xy(op,k*2)*geometry_xy(op,j*2+1)-
               (double)geometry_xy(op,j*2)*geometry_xy(op,k*2+1);
    }
    return value;
}

static int same_positions(const saturn_vk_vdp1_op *a,const saturn_vk_vdp1_op *b) {
    for(unsigned k=0;k<8;k++)if(geometry_xy(a,k)!=geometry_xy(b,k))return 0;
    return 1;
}

static unsigned cracks(unsigned count,const saturn_vk_vdp1_op *ops) {
    unsigned result=0;
    for(unsigned i=0;i<count;) {
        unsigned end=i+1;
        while(end<count&&!geometry_edge_compare(&edges[i],&edges[end]))end++;
        for(unsigned j=i+1;j<end;j++)if(edges[i].face!=edges[j].face &&
            !geometry_edge_close(ops,&edges[i],&edges[j],0.01f))result++;
        i=end;
    }
    return result;
}

int main(int argc,char **argv) {
    if(argc<2||argc>3){fputs("usage: audit_geometry_history <capture-prefix> [output.ops]\n",stderr);return 2;}
    unsigned count=load(argv[1],"current",current),old_count=load(argv[1],"before",before);
    if(!count||count!=old_count)return 2;
    memcpy(after,before,count*sizeof *after);
    unsigned changed=geometry_weld(current,count,after);
    unsigned eligible=0,matched=0,retained=0,moving=0,held=0,deformed=0,inverted=0,unsafe=0;
    for(unsigned i=0;i<count;i++)if(geometry_eligible(&current[i])) {
        eligible++;
        matched+=(before[i].flip&SATURN_GEOMETRY_FLOAT)!=0;
        retained+=(after[i].flip&SATURN_GEOMETRY_FLOAT)!=0;
        moving+=(after[i].flip&SATURN_GEOMETRY_FLOAT)!=0 && !same_positions(&after[i],&current[i]);
        held+=!(after[i].flip&SATURN_GEOMETRY_FLOAT);
        deformed+=!same_positions(&after[i],&before[i]) && !same_positions(&after[i],&current[i]);
        inverted+=area(&after[i])*area(&current[i])<0;
        unsafe+=(after[i].flip&SATURN_GEOMETRY_FLOAT)!=0 && !geometry_pair_valid(&after[i],&current[i]);
    }
    unsigned edge_count=geometry_edges(current,count,edges,0);
    unsigned cracks_before=cracks(edge_count,before),cracks_after=cracks(edge_count,after);
    printf("ops=%u eligible=%u matched_before=%u retained=%u moving=%u held=%u changed_vertices=%u\n",
           count,eligible,matched,retained,moving,held,changed);
    printf("shared_edge_cracks_before=%u after=%u deformed_faces=%u winding_reversals=%u unsafe_paths=%u\n",
           cracks_before,cracks_after,deformed,inverted,unsafe);
    if(argc==3) {
        FILE *file=fopen(argv[2],"wb");if(!file){perror(argv[2]);return 2;}
        int ok=fwrite(after,sizeof *after,count,file)==count;
        if(fclose(file)||!ok)return 2;
    }
    return cracks_after||deformed||inverted||unsafe ? 1 : 0;
}
