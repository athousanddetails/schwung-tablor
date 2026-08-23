/* How does the output quantise when the signal is QUIET?
 *
 * "A little crunchy around the edges... less smooth around the beginning/end
 * of the sounds." Fades are where the signal spends time near zero, which is
 * exactly where a truncating float->int16 conversion misbehaves: every value
 * within +/-1 LSB becomes exactly 0, so the waveform sticks to zero either
 * side of each crossing instead of passing through it.
 *
 * Measures that deadband directly: at a low level, how much of the signal is
 * pinned to zero, and how long the flat spots run.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <dlfcn.h>
#include <ctime>
#include "host/plugin_api_v1.h"
static void host_log(const char *) {}
int main(int argc, char **argv){
    void *dl=dlopen(argc>1?argv[1]:"./dsp.so", RTLD_NOW|RTLD_LOCAL);
    if(!dl){printf("dlopen: %s\n", dlerror()); return 1;}
    auto init=(move_plugin_init_v2_fn)dlsym(dl, MOVE_PLUGIN_INIT_V2_SYMBOL);
    host_api_v1_t h={}; h.api_version=1; h.sample_rate=MOVE_SAMPLE_RATE;
    h.frames_per_block=MOVE_FRAMES_PER_BLOCK; h.log=host_log;
    auto api=init(&h); void *inst=api->create_instance(".", nullptr);
    { char rb[8]={}; struct timespec ts={0,10*1000000};
      for(int i=0;i<1500;i++){api->get_param(inst,"ready",rb,sizeof rb);
        if(rb[0]=='1')break; nanosleep(&ts,nullptr);} }

    api->set_param(inst,"flt_freq","127");
    api->set_param(inst,"vca_a","0");  api->set_param(inst,"vca_d","127");
    api->set_param(inst,"vca_s","127");api->set_param(inst,"wt2_level","0");
    const uint8_t on[]={0x90,60,100}, off[]={0xB0,123,0};
    int16_t out[MOVE_FRAMES_PER_BLOCK*2];

    printf("  volume   peak   zeros%%  longest-zero-run   distinct-levels\n");
    for (int vol : {127, 40, 20, 12, 8}) {
        char v[8]; snprintf(v,sizeof v,"%d",vol);
        api->set_param(inst,"volume",v);
        api->on_midi(inst,on,3,0);
        for(int b=0;b<40;b++) api->render_block(inst,out,MOVE_FRAMES_PER_BLOCK); /* settle */
        long zeros=0, total=0, run=0, longest=0, peak=0;
        static unsigned char seen[65536];
        memset(seen,0,sizeof seen);
        for(int b=0;b<300;b++){
            api->render_block(inst,out,MOVE_FRAMES_PER_BLOCK);
            for(size_t i=0;i<MOVE_FRAMES_PER_BLOCK;i++){
                int16_t s=out[i*2]; total++;
                seen[(uint16_t)s]=1;
                long a=s<0?-s:s; if(a>peak) peak=a;
                if(s==0){zeros++; run++; if(run>longest) longest=run;} else run=0;
            }
        }
        int distinct=0; for(int i=0;i<65536;i++) distinct+=seen[i];
        printf("  %6d %6ld  %6.2f  %10ld       %8d\n",
               vol, peak, 100.0*zeros/total, longest, distinct);
        api->on_midi(inst,off,3,0);
        for(int b=0;b<120;b++) api->render_block(inst,out,MOVE_FRAMES_PER_BLOCK);
    }
    api->destroy_instance(inst);
}
