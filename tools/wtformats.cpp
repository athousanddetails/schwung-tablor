/* Does every wavetable shape actually PLAY?
 *
 * "Shorter wavetable lengths aren't playing... makes me wonder if there is a
 * preferential length" -- the answer used to be yes, and silently: frame-size
 * inference stopped at 256, so a short cycle inferred nothing, the 2048
 * fallback made nFrames 0, and the file loaded as nothing at all.
 *
 * This walks a folder, loads every file through the module the way the device
 * does, plays a note on each and reports the peak. A file that produces
 * silence is a file the user cannot use.
 *
 *   ./tablor_wtformats ./dsp.so "/data/UserData/UserLibrary/Wavetables/ZZ Test"
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <dlfcn.h>
#include <dirent.h>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>

#include "../src/host/plugin_api_v1.h"

static void host_log(const char *) {}

int main(int argc, char **argv)
{
    const char *so   = (argc > 1) ? argv[1] : "./dsp.so";
    const char *dir  = (argc > 2) ? argv[2]
                     : "/data/UserData/UserLibrary/Wavetables/ZZ Test";

    void *dl = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!dl) { printf("dlopen: %s\n", dlerror()); return 1; }
    auto init = (move_plugin_init_v2_fn) dlsym(dl, MOVE_PLUGIN_INIT_V2_SYMBOL);
    host_api_v1_t host = {};
    host.api_version = 1;
    host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    host.log = host_log;
    plugin_api_v2_t *api = init(&host);
    void *inst = api->create_instance(".", nullptr);
    if (!inst) { printf("create_instance failed\n"); return 1; }
    { char rb[8] = {}; struct timespec ts = { 0, 10 * 1000000 };
      for (int i = 0; i < 1500; i++) {
          api->get_param(inst, "ready", rb, sizeof rb);
          if (rb[0] == '1') break;
          nanosleep(&ts, nullptr);
      } }

    std::vector<std::string> files;
    if (DIR *d = opendir(dir)) {
        while (struct dirent *e = readdir(d))
            if (e->d_name[0] != '.') files.push_back(e->d_name);
        closedir(d);
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { printf("no files in %s\n", dir); return 1; }

    int16_t out[MOVE_FRAMES_PER_BLOCK * 2];
    const uint8_t on[]  = { 0x90, 60, 100 };
    const uint8_t off[] = { 0xB0, 123, 0 };
    int silent = 0;

    api->set_param(inst, "wt1_level", "127");
    api->set_param(inst, "wt2_level", "0");
    api->set_param(inst, "flt_freq", "127");
    api->set_param(inst, "vca_s", "127");

    /* the built-in Init table's digest: anything that fails to load reads back
     * as this, because the engine keeps whatever it already had */
    static char initDigest[64 * 1024];
    api->set_param(inst, "wt1_table", "");
    { char busy[8] = {}; struct timespec ts = { 0, 5 * 1000000 };
      for (int i = 0; i < 400; i++) {
          api->get_param(inst, "busy", busy, sizeof busy);
          if (busy[0] == '0') break;
          nanosleep(&ts, nullptr);
      } }
    api->get_param(inst, "wt1_shape", initDigest, sizeof initDigest);

    for (const auto &f : files) {
        std::string path = std::string(dir) + "/" + f;
        api->set_param(inst, "wt1_table", "");      /* back to Init first */
        { char b2[8] = {}; struct timespec t2 = { 0, 5 * 1000000 };
          for (int i = 0; i < 400; i++) {
              api->get_param(inst, "busy", b2, sizeof b2);
              if (b2[0] == '0') break;
              nanosleep(&t2, nullptr);
          } }
        api->set_param(inst, "wt1_table", path.c_str());
        char busy[8] = {};
        struct timespec ts = { 0, 5 * 1000000 };
        for (int i = 0; i < 400; i++) {
            api->get_param(inst, "busy", busy, sizeof busy);
            if (busy[0] == '0') break;
            nanosleep(&ts, nullptr);
        }
        /* Sound alone proves nothing: a failed load LEAVES THE PREVIOUS TABLE
         * in place, so the note still plays -- with the wrong wavetable. The
         * shape digest is derived from what is actually loaded, so comparing
         * it against the built-in Init is what catches a file that did not
         * load at all. */
        static char dig[64 * 1024];
        api->get_param(inst, "wt1_shape", dig, sizeof dig);
        bool loaded = strcmp(dig, initDigest) != 0;

        api->on_midi(inst, on, 3, 0);
        long peak = 0;
        for (int b = 0; b < 120; b++) {
            api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
            if (b < 20) continue;
            for (size_t i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++) {
                long v = out[i] < 0 ? -out[i] : out[i];
                if (v > peak) peak = v;
            }
        }
        api->on_midi(inst, off, 3, 0);
        for (int b = 0; b < 60; b++) api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);

        /* the digest header is "<frames>,<samples>," -- the frame count is
         * the thing that says whether the cycle length was read correctly.
         * One frame for a file that is really eight is the "looping" report. */
        int frames = 0;
        sscanf(dig, "%d,", &frames);

        bool ok = peak > 500 && loaded;
        if (!ok) silent++;
        printf("  %-22s peak %6ld  frames %3d  %s\n", f.c_str(), peak, frames,
               !loaded ? "NOT LOADED (fell back to the previous table)"
                       : (peak > 500 ? "plays" : "SILENT"));
    }

    printf("\n%s: %d of %d files silent\n",
           silent ? "FAILED" : "all formats play", silent, (int) files.size());
    api->destroy_instance(inst);
    return silent ? 1 : 0;
}
