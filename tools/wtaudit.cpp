/* Load every table in the real library and report its frame size, so a
 * change to inference cannot quietly re-read the whole seeded set. */
#include "ported/wav.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>
#include <algorithm>
using namespace tb;
static void walk(const std::string &dir, std::vector<std::string> &out){
    DIR *d=opendir(dir.c_str()); if(!d) return;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0]=='.') continue;
        std::string p = dir + "/" + e->d_name;
        if (e->d_type == DT_DIR) walk(p, out);
        else if (strstr(e->d_name, ".wav") || strstr(e->d_name, ".wt")) out.push_back(p);
    }
    closedir(d);
}
int main(int argc, char **argv){
    std::vector<std::string> files;
    walk(argc>1?argv[1]:"/data/UserData/UserLibrary/Wavetables", files);
    std::sort(files.begin(), files.end());
    int counts[14]={0}, changed=0, wavs=0;
    for (auto &f : files) {
        if (!strstr(f.c_str(), ".wav")) continue;   /* .wtNNNN names its own */
        WavData w; if (!wavLoad(f.c_str(), w)) continue;
        wavs++;
        int named = wavFrameSizeFromName(f.c_str());
        int base  = named ? named : wavInferFrameSize(w);
        int after = (w.clmFrameSize <= 0 && !named)
                  ? wavRefineFrameSize(w.samples, base) : base;
        if (after != base) { changed++;
            if (changed <= 6) printf("  CHANGED %-52s %d -> %d\n",
                                     f.substr(f.rfind('/')+1).c_str(), base, after); }
        for (int i=0,p=32;i<8;i++,p*=2) if (after==p) counts[i]++;
    }
    printf("  %d .wav tables scanned, %d read differently than before\n", wavs, changed);
    for (int i=0,p=32;i<8;i++,p*=2) if (counts[i]) printf("    frame %5d : %d\n", p, counts[i]);
}
