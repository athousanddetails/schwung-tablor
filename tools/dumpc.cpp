#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <ctime>
#include "host/plugin_api_v1.h"
static void host_log(const char*){}
int main(int argc,char**argv){
    void*dl=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); if(!dl){printf("%s\n",dlerror());return 1;}
    auto init=(move_plugin_init_v2_fn)dlsym(dl,MOVE_PLUGIN_INIT_V2_SYMBOL);
    host_api_v1_t h={}; h.api_version=1; h.sample_rate=MOVE_SAMPLE_RATE;
    h.frames_per_block=MOVE_FRAMES_PER_BLOCK; h.log=host_log;
    auto api=init(&h); void*inst=api->create_instance(".",nullptr);
    { char rb[8]={}; struct timespec ts={0,10*1000000};
      for(int i=0;i<1500;i++){api->get_param(inst,"ready",rb,sizeof rb); if(rb[0]=='1')break; nanosleep(&ts,nullptr);} }
    static char big[131072];
    api->get_param(inst,argv[2],big,sizeof big);
    printf("%s\n", big);
    api->destroy_instance(inst);
}
