/* Reproduce the "sounds modulated at zero, knobs stopped working" report
 * through the REAL host stack: the chain module with Tablor loaded as its
 * synth, and a Schwung slot LFO pointed at a Tablor parameter.
 *
 * The chain's mod overlay stores the BASE value of a modulated param and
 * writes base+mod into the plugin, while get_param returns the base -- so
 * every UI shows the knob where the user left it while the DSP hears
 * something else. This harness checks each step of the user's story against
 * that mechanism, on the device.
 *
 *   ./tablor_hostlfo /data/UserData/schwung/modules/chain/dsp.so
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <dlfcn.h>
#include <ctime>
#include <vector>
#include "host/plugin_api_v1.h"

static void host_log(const char *m) { fprintf(stderr, "  [log] %s\n", m); }
static float host_bpm() { return 120.0f; }

static plugin_api_v2_t *api; static void *inst;
static int16_t out[MOVE_FRAMES_PER_BLOCK * 2];

static void set(const char *k, const char *v){ api->set_param(inst,k,v); }
static const char *get(const char *k){
    static char b[256]; b[0]=0; api->get_param(inst,k,b,sizeof b); return b; }
static void settle(int ms){ struct timespec ts={0,1000000}; 
    for(int i=0;i<ms;i++) nanosleep(&ts,nullptr); }

/* render and report: rms + how much the spectrum MOVES over the window
 * (per-block centroid std-dev -- a wobbling wavetable position shows here) */
static void probe(const char *label, int blocks){
    std::vector<float> cents; double rmsAll=0; long n=0;
    for(int b=0;b<blocks;b++){
        api->render_block(inst,out,MOVE_FRAMES_PER_BLOCK);
        if (b < 40) continue;                    /* settle */
        double e=0, num=0, den=0, prev=0; int zc=0;
        for(size_t i=0;i<MOVE_FRAMES_PER_BLOCK;i++){
            float v=out[i*2]/32768.0f;
            e+=v*v; rmsAll+=v*v; n++;
            if(i>0 && prev<=0 && v>0) zc++;
            prev=v;
            (void)num;(void)den;
        }
        cents.push_back((float)e);
    }
    /* variance of per-block energy = amplitude/timbre wobble */
    double m=0; for(float c:cents) m+=c; m/= cents.size()?cents.size():1;
    double var=0; for(float c:cents) var+=(c-m)*(c-m); var/= cents.size()?cents.size():1;
    printf("  %-34s rms %.4f   block-energy wobble %5.1f%%\n",
           label, std::sqrt(rmsAll/(n?n:1)), m>1e-12? 100.0*std::sqrt(var)/m : 0.0);
}

int main(int argc, char **argv){
    const char *so = argc>1?argv[1]:"/data/UserData/schwung/modules/chain/dsp.so";
    void *dl=dlopen(so,RTLD_NOW|RTLD_LOCAL);
    if(!dl){printf("dlopen: %s\n",dlerror());return 1;}
    auto init=(move_plugin_init_v2_fn)dlsym(dl,MOVE_PLUGIN_INIT_V2_SYMBOL);
    host_api_v1_t h={}; h.api_version=1; h.sample_rate=MOVE_SAMPLE_RATE;
    h.frames_per_block=MOVE_FRAMES_PER_BLOCK; h.log=host_log; h.get_bpm=host_bpm;
    api=init(&h);
    inst=api->create_instance("/data/UserData/schwung/modules/chain",nullptr);
    if(!inst){printf("chain create failed\n");return 1;}

    printf("== load tablor into the chain ==\n");
    set("synth:module","tablor");
    settle(3000);                                 /* tablor's worker init */

    set("synth:wt2_level","0");
    set("synth:wt1_level","110");
    set("synth:wt1_pos","0");
    set("synth:flt_freq","127");
    set("synth:vca_a","0"); set("synth:vca_s","127");
    const uint8_t on[]={0x90,57,100}, off[]={0x80,57,0};

    printf("\n== step 1: plain note, pos 0, no host mod ==\n");
    api->on_midi(inst,on,3,0);
    probe("baseline",300);

    printf("\n== step 2: slot LFO -> synth:wt1_pos (the host's own LFO) ==\n");
    set("lfo1:target","synth");
    set("lfo1:target_param","wt1_pos");
    set("lfo1:shape","0"); set("lfo1:rate_hz","3");
    set("lfo1:depth","1"); set("lfo1:polarity","0");
    set("lfo1:enabled","1");
    probe("with slot LFO on wt1_pos",300);
    printf("  get_param(synth:wt1_pos) reports: \"%s\"  <- what every UI shows\n",
           get("synth:wt1_pos"));

    printf("\n== step 3: user turns the knob (base 0 -> 64) ==\n");
    set("synth:wt1_pos","64");
    probe("knob at 64, LFO still on",300);
    printf("  get_param(synth:wt1_pos) reports: \"%s\"\n", get("synth:wt1_pos"));

    printf("\n== step 4: user loads Tablor's Init preset ==\n");
    set("synth:preset","0");
    settle(400);
    api->on_midi(inst,off,3,0);
    for(int b=0;b<100;b++) api->render_block(inst,out,MOVE_FRAMES_PER_BLOCK);
    set("synth:wt1_level","110"); set("synth:flt_freq","127");
    set("synth:vca_a","0"); set("synth:vca_s","127");
    api->on_midi(inst,on,3,0);
    probe("after Tablor Init",300);

    printf("\n== step 5: slot LFO -> synth:wt2_level (the 'sounding at zero' claim) ==\n");
    set("lfo1:target_param","wt2_level");
    set("synth:wt1_level","0");                    /* mute osc1: anything heard is osc2 */
    set("synth:wt2_level","0");
    probe("osc1 muted, osc2 'at zero'",300);
    printf("  get_param(synth:wt2_level) reports: \"%s\"  <- the zero the user sees\n",
           get("synth:wt2_level"));

    printf("\n== step 6: slot LFO disabled -- does the mod clear? ==\n");
    set("lfo1:enabled","0");
    probe("LFO off",300);
    printf("  get_param(synth:wt2_level) reports: \"%s\"\n", get("synth:wt2_level"));

    api->on_midi(inst,off,3,0);
    api->destroy_instance(inst);
    return 0;
}
