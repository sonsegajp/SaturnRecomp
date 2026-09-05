#include "frame_pacing.h"
#include <stdio.h>
#include <stdint.h>
static int failures;
#define CHECK(c) do {if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c);failures++;}}while(0)
int main(void)
{
    const uint64_t period=16715;
    uint64_t now=1000000,deadline=now+period,start=now;
    /* Alternating heavy/light fields average below the budget. Retain the
     * timeline rather than permanently adding each heavy field's lateness. */
    for(unsigned i=0;i<600;i++){
        now+=(i&1)?9000:21000;
        if(now<deadline)now=deadline;
        deadline=saturn_next_field_deadline(deadline,now,period);
    }
    CHECK(now==start+600*period);
    CHECK(deadline==now+period);
    /* Ordinary host wake-up error must not compound every frame. */
    now=start;deadline=now+period;
    for(unsigned i=0;i<600;i++){
        now+=8000;
        if(now<deadline)now=deadline;
        now+=200;
        deadline=saturn_next_field_deadline(deadline,now,period);
    }
    CHECK(now==start+600*period+200);
    CHECK(saturn_next_field_deadline(0,now,period)==now+period);
    /* After a long OS stall, do not fast-forward a large frame backlog. */
    now=deadline+1000000;
    CHECK(saturn_next_field_deadline(deadline,now,period)==now+period);
    CHECK(saturn_next_field_deadline(deadline,deadline+period*2,period)==deadline+period);
    printf("%s frame pacing: %d failure(s)\n",failures?"FAIL":"PASS",failures);
    return failures!=0;
}
