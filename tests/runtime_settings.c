#include "runtime_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
static unsigned checks,failures;
#define CHECK(c) do {checks++;if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);failures++;}}while(0)
static void fixture(const char *path,const char *text) {
    FILE *f=fopen(path,"wb");CHECK(f!=NULL);if(!f)return;
    CHECK(fwrite(text,1,strlen(text),f)==strlen(text));CHECK(fclose(f)==0);
}
int main(void) {
    char path[160],text[8192];
#ifdef _WIN32
    unsigned long pid=GetCurrentProcessId();
#else
    unsigned long pid=(unsigned long)getpid();
#endif
    snprintf(path,sizeof path,"out/runtime-settings-test-%lu.ini",pid);
    remove(path);
    saturn_runtime_settings s,t;
    CHECK(!saturn_settings_load(&s,path));
    CHECK(s.window_width==960&&s.window_height==720&&s.volume==100&&s.target_hz==120&&!s.interpolation);
    fixture(path,"; keep this comment\r\n[Video]\r\nInterpolation=165\r\nFutureRenderer=custom:100%\r\n[Plugin]\r\nToken = untouched=string\r\n[Audio]\r\nVolume=43\r\nDriver=automatic");
    CHECK(saturn_settings_load(&s,path));
    CHECK(s.interpolation&&s.target_hz==165&&s.volume==43);
    s.interpolation=0;s.muted=1;s.window_width=1920;s.window_height=1080;
    CHECK(saturn_settings_save(&s,path));
    CHECK(saturn_settings_load(&t,path));
    CHECK(!memcmp(&s,&t,sizeof s));
    FILE *f=fopen(path,"rb");CHECK(f!=NULL);
    size_t n=f?fread(text,1,sizeof text-1,f):0;text[n]=0;if(f)fclose(f);
    CHECK(strstr(text,"; keep this comment\r\n")!=NULL);
    CHECK(strstr(text,"FutureRenderer=custom:100%\r\n")!=NULL);
    CHECK(strstr(text,"Token = untouched=string\r\n")!=NULL);
    CHECK(strstr(text,"Driver=automatic\n")!=NULL);
    CHECK(strstr(text,"Interpolation=0\n")&&strstr(text,"TargetHz=165\n"));
    t.interpolation=1;CHECK(saturn_settings_save(&t,path));
    CHECK(saturn_settings_load(&s,path)&&s.interpolation&&s.target_hz==165);

    fixture(path,"\xef\xbb\xbf[video]\nTargetHz=239\nInterpolation=120\nWindowWidth=100000\nWindowHeight=-1\nInternalScale=50\nFullscreen=-3\nTextureFilter=-1\nAntialiasing=2\nModelSmoothing=9\n[AuDio]\nVolume=200\nMuted=3\n");
    CHECK(saturn_settings_load(&s,path));
    CHECK(s.target_hz==239&&s.interpolation&&s.window_width==7680&&s.window_height==480);
    CHECK(s.internal_scale==4&&s.fullscreen==1&&s.texture_filter==0&&s.antialiasing==1&&s.model_smoothing==1);
    CHECK(s.volume==100&&s.muted==1);
    CHECK(saturn_settings_save(&s,path)&&saturn_settings_load(&t,path)&&!memcmp(&s,&t,sizeof s));

    fixture(path,"[Video]\nTargetHz=500000000000000000000000000000\nInterpolation=garbage\nWindowWidth=123pixels\nWindowHeight=\nInternalScale=3 ; comment\n[Audio]\nVolume=19 # comment\n");
    CHECK(saturn_settings_load(&s,path));
    CHECK(s.target_hz==120&&!s.interpolation&&s.window_width==960&&s.window_height==720);
    CHECK(s.internal_scale==3&&s.volume==19);
    for(int hz=60;hz<=240;hz+=17) {
        s.target_hz=hz;s.interpolation=1;
        CHECK(saturn_settings_save(&s,path)&&saturn_settings_load(&t,path));
        CHECK(t.target_hz==hz&&t.interpolation);
    }
    s.window_width=1;s.window_height=99999;s.internal_scale=0;s.target_hz=0;s.volume=-50;
    CHECK(saturn_settings_save(&s,path)&&saturn_settings_load(&t,path));
    CHECK(t.window_width==640&&t.window_height==4320&&t.internal_scale==1&&t.target_hz==60&&t.volume==0);
    CHECK(!saturn_settings_save(&s,"out/no-such-runtime-settings-directory/settings.ini"));
    CHECK(!saturn_settings_load(NULL,path)&&!saturn_settings_save(NULL,path));
    CHECK(remove(path)==0);
    printf("runtime settings: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
