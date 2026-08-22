/* Is there a transient at note start beyond what the envelope allows? */
#include <cstdio>
#include <initializer_list>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <dlfcn.h>
#include <ctime>
#include "host/plugin_api_v1.h"
static void host_log(const char *) {}
int main(int argc, char **argv){
    void *dl = dlopen(argc>1?argv[1]:"./dsp.so", RTLD_NOW|RTLD_LOCAL);
    if(!dl){printf("dlopen: %s\n", dlerror()); return 1;}
    auto init=(move_plugin_init_v2_fn)dlsym(dl, MOVE_PLUGIN_INIT_V2_SYMBOL);
    host_api_v1_t h={}; h.api_version=1; h.sample_rate=MOVE_SAMPLE_RATE;
    h.frames_per_block=MOVE_FRAMES_PER_BLOCK; h.log=host_log;
    auto api=init(&h); void *inst=api->create_instance(".", nullptr);
    { char rb[8]={}; struct timespec ts={0,10*1000000};
      for(int i=0;i<1500;i++){api->get_param(inst,"ready",rb,sizeof rb);
        if(rb[0]=='1')break; nanosleep(&ts,nullptr);} }

    api->set_param(inst,"flt_freq","127");   /* filter wide open */
    api->set_param(inst,"flt_env","64");     /* no filter env sweep */
    api->set_param(inst,"vca_d","127"); api->set_param(inst,"vca_s","127");
    api->set_param(inst,"wt2_level","0");
    const uint8_t on[]={0x90,60,100}, off[]={0xB0,123,0};
    int16_t out[MOVE_FRAMES_PER_BLOCK*2];

    for (int pot : {0,20,50,64}) {
        char v[8]; snprintf(v,sizeof v,"%d",pot);
        api->set_param(inst,"vca_a",v);
        api->on_midi(inst,on,3,0);
        /* peak per 1 ms window for the first 8 ms (44 samples/ms) */
        printf("  vca_a=%-4d ", pot);
        long win[8]={0}; int s=0;
        for(int b=0;b<4;b++){
            api->render_block(inst,out,MOVE_FRAMES_PER_BLOCK);
            for(size_t i=0;i<MOVE_FRAMES_PER_BLOCK;i++,s++){
                int w=s/44; if(w>=8) break;
                long a=out[i*2]<0?-out[i*2]:out[i*2];
                if(a>win[w]) win[w]=a;
            }
        }
        for(int w=0;w<8;w++) printf("%6ld", win[w]);
        printf("   (peak per ms, 0-8ms)\n");
        api->on_midi(inst,off,3,0);
        for(int b=0;b<200;b++) api->render_block(inst,out,MOVE_FRAMES_PER_BLOCK);
    }
    api->destroy_instance(inst);
}
