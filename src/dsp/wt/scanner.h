/* Wavetable scanner — builds the name-indexed table list that becomes the
 * WT1/WT2 enum options. Scans directory LISTINGS only (names, no audio
 * decode), so it is cheap enough to run at create_instance and on rescan.
 *
 * Sources, in order:
 *   0            "Init"  (the built-in table, always present)
 *   module dir   <module>/wavetables/** (factory packs, .wt2048 FLAC + .wav)
 *   user dir     /data/UserData/UserLibrary/Wavetables/** (.wav; what the
 *                user uploads via Move Manager / schwung-manager)
 */
#pragma once

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace tb {

struct WtEntry {
    std::string name;     /* shown in the enum list: "Pack/Name" or "Name" */
    std::string path;     /* absolute; empty for the built-in Init         */
    int flacFrameSize = 0;/* >0: .wtNNNN FLAC; 0: plain WAV                */
};

class WtScanner {
public:
    static constexpr const char *kUserDir = "/data/UserData/UserLibrary/Wavetables";

    void scan(const std::string &moduleDir)
    {
        entries.clear();
        entries.push_back({ "Init", "", 0 });

        if (!moduleDir.empty())
            scanDir(moduleDir + "/wavetables", "", 0);

        ::mkdir(kUserDir, 0755);   /* first run: create the drop folder */
        scanDir(kUserDir, "", 0);

        /* keep Init first, sort the rest by name */
        std::sort(entries.begin() + 1, entries.end(),
                  [](const WtEntry &a, const WtEntry &b) { return a.name < b.name; });
    }

    const std::vector<WtEntry> &list() const { return entries; }

    int indexOfName(const char *name) const
    {
        for (size_t i = 0; i < entries.size(); i++)
            if (entries[i].name == name)
                return (int) i;
        return -1;
    }

private:
    void scanDir(const std::string &dir, const std::string &prefix, int depth)
    {
        if (depth > 3) return;                     /* sanity bound */
        DIR *d = ::opendir(dir.c_str());
        if (!d) return;

        while (dirent *e = ::readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string full = dir + "/" + e->d_name;

            struct stat st;
            if (::stat(full.c_str(), &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                scanDir(full, prefix.empty() ? std::string(e->d_name)
                                             : prefix + "/" + e->d_name, depth + 1);
                continue;
            }

            const char *dot = std::strrchr(e->d_name, '.');
            if (!dot) continue;

            int flacSize = 0;
            if (!strcasecmp(dot, ".wav")) {
                /* plain WAV */
            } else if (!strncasecmp(dot, ".wt", 3)) {
                flacSize = std::atoi(dot + 3);     /* .wt2048 etc (FLAC) */
                if (flacSize < 256 || flacSize > 4096 ||
                    (flacSize & (flacSize - 1)) != 0)
                    continue;
            } else {
                continue;
            }

            std::string base(e->d_name, (size_t) (dot - e->d_name));
            entries.push_back({ prefix.empty() ? base : prefix + "/" + base,
                                full, flacSize });
        }
        ::closedir(d);
    }

    std::vector<WtEntry> entries;
};

} // namespace tb
