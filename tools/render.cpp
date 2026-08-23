/* Render notes through the module and write a WAV, so the output can be
 * analysed offline instead of guessed at.
 *   ./tablor_render ./dsp.so <wavetable> <pos 0-127> <out.wav>
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <dlfcn.h>
#include <ctime>
#include <vector>
#include "host/plugin_api_v1.h"
static void host_log(const char *m) { fprintf(stderr, "  [log] %s\n", m); }
static void put32(FILE*f,uint32_t v){fwrite(&v,4,1,f);} static void put16(FILE*f,uint16_t v){fwrite(&v,2,1,f);}
int main(int argc, char **argv){
    const char *so=argc>1?argv[1]:"./dsp.so";
    const char *wt=argc>2?argv[2]:"";
    const char *pos=argc>3?argv[3]:"0";
    const char *outp=argc>4?argv[4]:"/tmp/out.wav";
    void *dl=dlopen(so,RTLD_NOW|RTLD_LOCAL); if(!dl){printf("dlopen %s\n",dlerror());return 1;}
    auto init=(move_plugin_init_v2_fn)dlsym(dl,MOVE_PLUGIN_INIT_V2_SYMBOL);
    host_api_v1_t h={}; h.api_version=1; h.sample_rate=MOVE_SAMPLE_RATE;
    h.frames_per_block=MOVE_FRAMES_PER_BLOCK; h.log=host_log;
    auto api=init(&h); void*inst=api->create_instance(".",nullptr);
    { char rb[8]={}; struct timespec ts={0,10*1000000};
      for(int i=0;i<1500;i++){api->get_param(inst,"ready",rb,sizeof rb); if(rb[0]=='1')break; nanosleep(&ts,nullptr);} }

    if (wt[0]) {
        api->set_param(inst,"wt1_table",wt);
        char b[8]={}; struct timespec ts={0,5*1000000};
        for(int i=0;i<600;i++){api->get_param(inst,"busy",b,sizeof b); if(b[0]=='0')break; nanosleep(&ts,nullptr);}
        char sh[256]; api->get_param(inst,"wt1_shape",sh,sizeof sh);
        int fr=0; sscanf(sh,"%d,",&fr); fprintf(stderr,"  loaded, digest frames=%d\n",fr);
    }
    api->set_param(inst,"wt1_pos",pos);
    api->set_param(inst,"wt2_level","0");
    api->set_param(inst,"wt1_level","110");
    api->set_param(inst,"flt_freq","127");
    api->set_param(inst,"vca_a","45");   /* ~45 ms */
    api->set_param(inst,"vca_d","90");
    api->set_param(inst,"vca_s","100");
    api->set_param(inst,"vca_r","70");   /* audible tail */
    api->set_param(inst,"volume","100");

    std::vector<int16_t> pcm;
    int16_t out[MOVE_FRAMES_PER_BLOCK*2];
    auto run=[&](int blocks){ for(int b=0;b<blocks;b++){ api->render_block(inst,out,MOVE_FRAMES_PER_BLOCK);
        pcm.insert(pcm.end(), out, out+MOVE_FRAMES_PER_BLOCK*2);} };
    const uint8_t on[]={0x90,57,100}, off[]={0x80,57,0};   /* A3 */
    run(20); api->on_midi(inst,on,3,0); run(260); api->on_midi(inst,off,3,0); run(400);

    FILE*f=fopen(outp,"wb"); uint32_t bytes=pcm.size()*2;
    fwrite("RIFF",1,4,f); put32(f,36+bytes); fwrite("WAVEfmt ",1,8,f);
    put32(f,16); put16(f,1); put16(f,2); put32(f,44100); put32(f,44100*4); put16(f,4); put16(f,16);
    fwrite("data",1,4,f); put32(f,bytes); fwrite(pcm.data(),1,bytes,f); fclose(f);
    fprintf(stderr,"  wrote %s (%zu frames)\n",outp,pcm.size()/2);
    api->destroy_instance(inst);
}
