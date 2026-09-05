#include "saturn.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
int main(void){
    unsigned capacity=(unsigned)(sizeof(((saturn *)0)->snd_buf)/sizeof(int16_t)/2);
    unsigned fill=(3u+SATURN_AUDIO_RING_FRAMES-(capacity-2u))%SATURN_AUDIO_RING_FRAMES;
    if(SATURN_AUDIO_RING_FRAMES!=capacity || fill!=5u){
        printf("FAIL frontend ring: macro=%u storage=%u wrapped_fill=%u\n",(unsigned)SATURN_AUDIO_RING_FRAMES,capacity,fill);return 1;
    }
    printf("PASS Windows frontend audio ring: %u frames, wrapped fill %u\n",capacity,fill);return 0;
}
