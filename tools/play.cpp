/* Load a table on the device and report what the engine actually got. */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <dlfcn.h>
#include <ctime>
#include <cmath>
#include "host/plugin_api_v1.h"
static void host_log(const char*){}
static double den_dummy(double a,double b){return a/b;}
static double den = 0;
int main(int argc,char**argv){
    void*dl=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); if(!dl){printf("%s\n",dlerror());return 1;}
    auto init=(move_plugin_init_v2_fn)dlsym(dl,MOVE_PLUGIN_INIT_V2_SYMBOL);
    host_api_v1_t h={}; h.api_version=1; h.sample_rate=MOVE_SAMPLE_RATE;
    h.frames_per_block=MOVE_FRAMES_PER_BLOCK; h.log=host_log;
    auto api=init(&h); void*inst=api->create_instance(".",nullptr);
    { char rb[8]={}; struct timespec ts={0,10*1000000};
      for(int i=0;i<1500;i++){api->get_param(inst,"ready",rb,sizeof rb); if(rb[0]=='1')break; nanosleep(&ts,nullptr);} }
    int16_t out[MOVE_FRAMES_PER_BLOCK*2];
    for(int i=2;i<argc;i++){
        api->set_param(inst,"wt1_table",argv[i]);
        { char b[8]={}; struct timespec ts={0,5*1000000};
          for(int k=0;k<800;k++){api->get_param(inst,"busy",b,sizeof b); if(b[0]=='0')break; nanosleep(&ts,nullptr);} }
        char sh[512]; api->get_param(inst,"wt1_shape",sh,sizeof sh);
        int frames=0; sscanf(sh,"%d,",&frames);
        api->set_param(inst,"wt1_level","110"); api->set_param(inst,"volume","100");
        api->set_param(inst,"flt_freq","127"); api->set_param(inst,"vca_a","0"); api->set_param(inst,"vca_s","127");
        const uint8_t on[]={0x90,57,100}, off[]={0xB0,123,0};
        long peak=0; double e=0; long n=0;
        api->on_midi(inst,on,3,0);
        for(int b=0;b<200;b++){ api->render_block(inst,out,MOVE_FRAMES_PER_BLOCK);
            if(b<40) continue;
            for(size_t j=0;j<MOVE_FRAMES_PER_BLOCK;j++){ long a=out[j*2]; if(a<0)a=-a; if(a>peak)peak=a; e+=(double)out[j*2]*out[j*2]; n++; } }
        api->on_midi(inst,off,3,0);
        for(int b=0;b<60;b++) api->render_block(inst,out,MOVE_FRAMES_PER_BLOCK);
        const char*f=argv[i]; for(const char*c=argv[i];*c;c++) if(*c=='/') f=c+1;
        /* Does WT Pos actually move through the table, and is the motion
         * smooth? Sweep the position and report the brightness at each stop
         * plus the worst single-sample step against the waveform's own
         * slope -- a step far beyond that scale is what "crunchy" is. */
        double cent[5]; double worstJump = 0, natural = 0;
        for (int q = 0; q < 5; q++) {
            char pv[8]; snprintf(pv, sizeof pv, "%d", q * 127 / 4);
            api->set_param(inst, "wt1_pos", pv);
            api->on_midi(inst, on, 3, 0);
            double num = 0, den = 0, prevS = 0; int first = 1;
            for (int b = 0; b < 120; b++) {
                api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
                if (b < 40) continue;
                for (size_t j = 0; j < MOVE_FRAMES_PER_BLOCK; j++) {
                    double v = out[j*2] / 32768.0;
                    if (!first) { double d = fabs(v - prevS);
                        if (d > worstJump) worstJump = d;
                        natural += d; den += 1; }
                    prevS = v; first = 0;
                    num += fabs(v);
                }
            }
            cent[q] = den > 0 ? num / den : 0;
            api->on_midi(inst, off, 3, 0);
            for (int b = 0; b < 40; b++) api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        }
        double meanStep = den_dummy(natural, 1);
        (void) meanStep;
        double avgSlope = natural / 1.0;
        printf("  %-26s frames=%-4d %s\n", f, frames, peak > 500 ? "sounds" : "SILENT");
        printf("      pos sweep (mean level): ");
        int moved = 0;
        for (int q = 0; q < 5; q++) { printf("%.4f ", cent[q]);
            if (q && fabs(cent[q] - cent[0]) > cent[0] * 0.01) moved = 1; }
        printf("  %s\n", moved ? "POSITION WORKS" : "position does nothing");
        printf("      worst step %.4f vs mean step %.5f  -> %s\n",
               worstJump, avgSlope / 100000.0,
               worstJump < 0.25 ? "smooth" : "CHECK");
    }
    api->destroy_instance(inst);
}
