#include "runtime_settings.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

typedef struct setting_key {const char *name;int section;size_t offset;} setting_key;
#define KEY(name,section,field) {name,section,offsetof(saturn_runtime_settings,field)}
static const setting_key keys[]={
    KEY("WindowWidth",0,window_width),KEY("WindowHeight",0,window_height),
    KEY("Fullscreen",0,fullscreen),KEY("InternalScale",0,internal_scale),
    KEY("TextureFilter",0,texture_filter),KEY("Antialiasing",0,antialiasing),
    KEY("ModelSmoothing",0,model_smoothing),KEY("Interpolation",0,interpolation),
    KEY("TargetHz",0,target_hz),KEY("Volume",1,volume),KEY("Muted",1,muted)
};
enum {KEY_COUNT=sizeof keys/sizeof *keys,INTERPOLATION_KEY=7,TARGET_KEY=8};
static const char *sections[]={"Video","Audio"};

static int clamp(int n,int low,int high) {return n<low?low:n>high?high:n;}
void saturn_settings_defaults(saturn_runtime_settings *s) {
    if(!s)return;
    *s=(saturn_runtime_settings){960,720,0,1,0,0,0,0,120,100,0};
}
void saturn_settings_normalize(saturn_runtime_settings *s) {
    if(!s)return;
    s->window_width=clamp(s->window_width,SATURN_MIN_WINDOW_WIDTH,7680);
    s->window_height=clamp(s->window_height,SATURN_MIN_WINDOW_HEIGHT,4320);
    s->internal_scale=clamp(s->internal_scale,1,4);
    s->target_hz=clamp(s->target_hz,60,240);s->volume=clamp(s->volume,0,100);
    s->fullscreen=!!s->fullscreen;s->interpolation=!!s->interpolation;s->muted=!!s->muted;
    s->texture_filter=clamp(s->texture_filter,0,1);
    s->antialiasing=clamp(s->antialiasing,0,1);
    s->model_smoothing=clamp(s->model_smoothing,0,1);
}
static int equal(const char *a,size_t n,const char *b) {
    if(strlen(b)!=n)return 0;
    for(size_t i=0;i<n;i++)if(tolower((unsigned char)a[i])!=tolower((unsigned char)b[i]))return 0;
    return 1;
}
/* Parse a bounded line without changing unknown text or comment bytes. */
static int line_key(const char *p,const char *end,int *section,int *header,const char **value) {
    *header=0;*value=NULL;
    if(end-p>=3&&!memcmp(p,"\xef\xbb\xbf",3))p+=3;
    while(p<end&&isspace((unsigned char)*p))p++;
    if(p==end||*p==';'||*p=='#')return -1;
    if(*p=='[') {
        const char *q=++p;while(q<end&&*q!=']')q++;
        if(q==end)return -1;
        while(q>p&&isspace((unsigned char)q[-1]))q--;
        while(p<q&&isspace((unsigned char)*p))p++;
        *section=equal(p,(size_t)(q-p),"Video")?0:equal(p,(size_t)(q-p),"Audio")?1:-1;
        *header=1;return -1;
    }
    const char *q=p;while(q<end&&*q!='=')q++;
    if(q==end)return -1;
    *value=q+1;while(q>p&&isspace((unsigned char)q[-1]))q--;
    for(unsigned i=0;i<KEY_COUNT;i++)
        if(keys[i].section==*section&&equal(p,(size_t)(q-p),keys[i].name))return (int)i;
    return -1;
}
#ifdef _WIN32
static wchar_t *wide_path(const char *path) {
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,NULL,0);
    if(!n)return NULL;
    wchar_t *out=malloc((size_t)n*sizeof *out);
    if(out&&!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,out,n)){free(out);out=NULL;}
    return out;
}
static FILE *open_path(const char *path,int write) {
    wchar_t *wide=wide_path(path);if(!wide)return NULL;
    FILE *f=_wfopen(wide,write?L"wb":L"rb");free(wide);return f;
}
#else
static FILE *open_path(const char *path,int write) {return fopen(path,write?"wb":"rb");}
#endif
static char *read_file(const char *path,size_t *length,int *missing) {
    *length=0;*missing=0;errno=0;
    FILE *f=open_path(path,0);
    if(!f){*missing=errno==ENOENT;return NULL;}
    if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}
    long size=ftell(f);
    if(size<0||size>2*1024*1024||fseek(f,0,SEEK_SET)){fclose(f);return NULL;}
    char *text=malloc((size_t)size+1);
    if(!text){fclose(f);return NULL;}
    if(fread(text,1,(size_t)size,f)!=(size_t)size||ferror(f)){free(text);fclose(f);return NULL;}
    fclose(f);text[size]=0;*length=(size_t)size;return text;
}
int saturn_settings_load(saturn_runtime_settings *s,const char *path) {
    if(!s)return 0;
    saturn_settings_defaults(s);
    if(!path||!*path)return 0;
    size_t length;int missing;char *text=read_file(path,&length,&missing);
    if(!text)return 0;
    int section=-1,target_seen=0,legacy_hz=0;
    for(char *p=text;p<text+length;) {
        char *end=memchr(p,'\n',(size_t)(text+length-p));if(!end)end=text+length;
        int header;const char *value;int key=line_key(p,end,&section,&header,&value);
        if(key>=0) {
            char saved=*end;*end=0;
            char *tail;errno=0;long number=strtol(value,&tail,10);
            while(tail<end&&isspace((unsigned char)*tail))tail++;
            if(tail!=value&&!errno&&number>=INT_MIN&&number<=INT_MAX&&
               (tail==end||*tail==';'||*tail=='#')) {
                int *field=(int*)((char*)s+keys[key].offset);*field=(int)number;
                if(key==INTERPOLATION_KEY){s->interpolation=number>0;if(number>1)legacy_hz=(int)number;}
                if(key==TARGET_KEY)target_seen=1;
            }
            *end=saved;
        }
        p=end<text+length?end+1:end;
    }
    if(!target_seen&&legacy_hz)s->target_hz=legacy_hz;
    saturn_settings_normalize(s);free(text);return 1;
}
static int write_key(FILE *f,const saturn_runtime_settings *s,unsigned key) {
    int value=*(const int*)((const char*)s+keys[key].offset);
    if(key==INTERPOLATION_KEY)value=s->interpolation?s->target_hz:0;
    return fprintf(f,"%s=%d\n",keys[key].name,value)>=0;
}
static int finish_section(FILE *f,const saturn_runtime_settings *s,int section,unsigned *seen) {
    if(section<0)return 1;
    for(unsigned i=0;i<KEY_COUNT;i++)if(keys[i].section==section&&!(*seen&(1u<<i))) {
        if(!write_key(f,s,i))return 0;
        *seen|=1u<<i;
    }
    return 1;
}
int saturn_settings_save(const saturn_runtime_settings *settings,const char *path) {
    if(!settings||!path||!*path)return 0;
    saturn_runtime_settings s=*settings;saturn_settings_normalize(&s);
    size_t length=0;int missing=0;char *text=read_file(path,&length,&missing);
    if(!text&&!missing)return 0;
    char *temp=malloc(strlen(path)+48);if(!temp){free(text);return 0;}
#ifdef _WIN32
    unsigned long pid=GetCurrentProcessId();
#else
    unsigned long pid=(unsigned long)getpid();
#endif
    sprintf(temp,"%s.runtime-%lu.tmp",path,pid);
    FILE *f=open_path(temp,1);if(!f){free(text);free(temp);return 0;}
    unsigned seen=0,section_seen=0;int section=-1,ok=1;
    for(const char *p=text;p&&p<text+length;) {
        const char *end=memchr(p,'\n',(size_t)(text+length-p));if(!end)end=text+length;
        int previous=section,header;const char *value;
        int key=line_key(p,end,&section,&header,&value);
        if(header) {
            ok&=finish_section(f,&s,previous,&seen);
            if(section>=0)section_seen|=1u<<section;
        }
        if(key<0) {
            ok&=fwrite(p,1,(size_t)(end-p),f)==(size_t)(end-p);
            ok&=fputc('\n',f)!=EOF;
        } else if(!(seen&(1u<<key))) {
            ok&=write_key(f,&s,(unsigned)key);seen|=1u<<key;
        }
        p=end<text+length?end+1:end;
    }
    ok&=finish_section(f,&s,section,&seen);
    for(unsigned i=0;i<2;i++)if(!(section_seen&(1u<<i))) {
        ok&=fprintf(f,"\n[%s]\n",sections[i])>=0;
        ok&=finish_section(f,&s,(int)i,&seen);
    }
    if(ferror(f))ok=0;
    if(fclose(f))ok=0;
#ifdef _WIN32
    wchar_t *source=wide_path(temp),*target=wide_path(path);
    if(!source||!target)ok=0;
    if(ok&&!MoveFileExW(source,target,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))ok=0;
    if(!ok&&source)DeleteFileW(source);
    free(source);free(target);
#else
    if(ok&&rename(temp,path))ok=0;
    if(!ok)remove(temp);
#endif
    free(text);free(temp);return ok;
}
