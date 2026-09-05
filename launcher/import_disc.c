#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p,0755)
#endif
static void xml(FILE *f,const char *s){for(;*s;s++){switch(*s){case '&':fputs("&amp;",f);break;case '<':fputs("&lt;",f);break;case '>':fputs("&gt;",f);break;case '\"':fputs("&quot;",f);break;default:if((unsigned char)*s>=32)fputc(*s,f);}}}
static int safe_path(const char *s){
 if(*s=='/')s++;if(!*s||strchr(s,'\\')||strchr(s,':'))return 0;
 for(const char *p=s;*p;){const char *e=strchr(p,'/');size_t n=e?(size_t)(e-p):strlen(p);if(!n||(n==1&&p[0]=='.')||(n==2&&p[0]=='.'&&p[1]=='.'))return 0;for(size_t i=0;i<n;i++)if((unsigned char)p[i]<32||strchr("<>\"|?*",p[i]))return 0;if(!e)break;p=e+1;}
 return 1;
}
static int parents(char *p){for(char *q=p+1;*q;q++)if(*q=='/'||*q=='\\'){char c=*q;*q=0;if(MKDIR(p)&&errno!=EEXIST){*q=c;return 0;}*q=c;}return 1;}
int main(int argc,char **argv){
 if(argc!=3){fputs("usage: saturn-import <disc> <new-output-folder>\n",stderr);return 2;}
 disc d;iso_fs fs={0};saturn_ip ip;int result=1;FILE *manifest=NULL;
 if(disc_open(&d,argv[1])){fprintf(stderr,"Disc could not be opened: %s\n",d.err);return 1;}
 if(ip_read(&d,&ip)||iso_read(&d,&fs)){fputs("Disc is not a readable Sega Saturn ISO9660 image.\n",stderr);iso_free(&fs);disc_close(&d);return 1;}
 const iso_entry *boot=NULL;
 for(int i=0;i<fs.nentries;i++)if(!fs.entries[i].is_dir&&!strchr(fs.entries[i].path+1,'/')){boot=&fs.entries[i];break;}
 if(!boot){fputs("No boot file in ISO root directory.\n",stderr);iso_free(&fs);disc_close(&d);return 1;}
 char dest[4096];snprintf(dest,sizeof dest,"%s/assets/",argv[2]);if(!parents(dest)){perror("Output folder");goto done;}
 snprintf(dest,sizeof dest,"%s/manifest.xml",argv[2]);manifest=fopen(dest,"wb");if(!manifest){perror("Manifest");goto done;}
 fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<saturn-game schema=\"1\" title=\"",manifest);xml(manifest,ip.title);
 fputs("\" product=\"",manifest);xml(manifest,ip.product_no);fputs("\" version=\"",manifest);xml(manifest,ip.version);fputs("\" areas=\"",manifest);xml(manifest,ip.area);
 fputs("\" boot-file=\"",manifest);xml(manifest,boot->path);
 fprintf(manifest,"\" load-address=\"0x%08X\" first-read-size=\"%u\">\n<files>\n",ip.first_read_addr,ip.first_read_size);
 unsigned total=0,donefiles=0;for(int i=0;i<fs.nentries;i++)if(!fs.entries[i].is_dir)total++;
 for(int i=0;i<fs.nentries;i++){
  iso_entry *e=&fs.entries[i];if(e->is_dir)continue;
  if(!safe_path(e->path)){fputs("Unsafe path in disc directory.\n",stderr);goto done;}
  if(!e->readable){fprintf(stderr,"Disc file is not readable: %s\n",e->path);goto done;}
  const char *rel=e->path+(*e->path=='/');
  int len=snprintf(dest,sizeof dest,"%s/assets/%s",argv[2],rel);if(len<0||(size_t)len>=sizeof dest||!parents(dest)){fputs("Asset path unavailable.\n",stderr);goto done;}
  size_t bytes=0;void *data=iso_extract(&d,e,&bytes);if(!data&&e->size){fprintf(stderr,"Could not extract %s\n",e->path);goto done;}
  FILE *out=fopen(dest,"wb");if(!out){free(data);perror("Asset");goto done;}
  int ok=fwrite(data,1,bytes,out)==bytes;if(fclose(out))ok=0;free(data);if(!ok){fputs("Asset write failed.\n",stderr);goto done;}
  fputs("<file path=\"",manifest);xml(manifest,e->path);fprintf(manifest,"\" lba=\"%u\" size=\"%zu\" storage=\"%s\"/>\n",e->lba,bytes,e->in_audio?"audio-track":"data-track");
  printf("FILE %u %u\n",++donefiles,total);fflush(stdout);
 }
 fputs("</files>\n<tracks>\n",manifest);
 for(int i=0;i<d.ntracks;i++){
  const disc_track *t=&d.tracks[i];
  fprintf(manifest,"<track number=\"%d\" mode=\"%d\" lba=\"%u\" pregap=\"%u\" file-lba=\"%u\" source=\"",t->num,(int)t->mode,t->start_lba,t->pregap,t->file_lba);
  xml(manifest,d.files[t->file_index].path);fputs("\"/>\n",manifest);
 }
 fputs("</tracks>\n</saturn-game>\n",manifest);result=0;
done:
 if(manifest&&fclose(manifest))result=1;iso_free(&fs);disc_close(&d);return result;
}
