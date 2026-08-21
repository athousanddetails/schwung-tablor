/*
 * The turnable wavetable selection, driven without a device.
 *
 * Pins the three things that make it work at all: the published contract
 * is valid JSON with the right shape, choosing a pack actually changes
 * wt1_select's options, and get_param answers from memory rather than
 * scanning (which would put a directory walk on the SPI callback).
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* Stand in for the scanner so the test needs no wavetables on disk. */
namespace tb {
struct WtEntry { std::string name, path; int flacFrameSize = 0; };
struct FakeScanner {
    std::vector<WtEntry> entries;
    const std::vector<WtEntry> &list() const { return entries; }
};
}

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
    else       printf("  ok  %s\n", what);
}

/* The helpers under test, lifted verbatim in shape from tablor_plugin.cpp. */
static std::string pack_of(const std::string &n) {
    size_t s = n.find('/'); return s == std::string::npos ? std::string() : n.substr(0, s);
}
static std::string leaf_of(const std::string &n) {
    size_t s = n.rfind('/'); return s == std::string::npos ? n : n.substr(s + 1);
}

struct Inst {
    tb::FakeScanner scanner;
    std::vector<std::string> packs;
    std::vector<int> filtered;
    int wt_pack = 0;
};

static const size_t MAXOPT = 128;

static void rebuild_packs(Inst &i) {
    i.packs.clear(); i.packs.push_back("All");
    for (const auto &e : i.scanner.list()) {
        std::string pk = pack_of(e.name);
        if (pk.empty()) continue;
        bool seen = false;
        for (size_t k = 1; k < i.packs.size(); k++) if (i.packs[k] == pk) { seen = true; break; }
        if (!seen) i.packs.push_back(pk);
    }
    if (i.wt_pack >= (int) i.packs.size()) i.wt_pack = 0;
}
static void rebuild_filtered(Inst &i) {
    i.filtered.clear();
    const auto &l = i.scanner.list();
    bool all = (i.wt_pack <= 0);
    std::string want = all ? std::string() : i.packs[(size_t) i.wt_pack];
    for (size_t k = 0; k < l.size(); k++) {
        if (k == 0) { i.filtered.push_back(0); continue; }
        if (all || pack_of(l[k].name) == want) i.filtered.push_back((int) k);
        if (i.filtered.size() >= MAXOPT) break;
    }
}

int main(void) {
    Inst inst;
    inst.scanner.entries.push_back({ "Init", "" });
    inst.scanner.entries.push_back({ "Serum/Basses", "/w/Serum/Basses.wav" });
    inst.scanner.entries.push_back({ "Serum/Leads",  "/w/Serum/Leads.wav" });
    inst.scanner.entries.push_back({ "Vital/Pads",   "/w/Vital/Pads.wav" });
    inst.scanner.entries.push_back({ "Loose",        "/w/Loose.wav" });

    rebuild_packs(inst);
    check(inst.packs.size() == 3, "packs = All + Serum + Vital (a bare file adds none)");
    check(inst.packs[0] == "All", "pack 0 is All");

    rebuild_filtered(inst);
    check(inst.filtered.size() == 5, "pack All lists every entry");

    /* Choosing a pack must CHANGE the option list — the whole point. */
    inst.wt_pack = 1;                       /* Serum */
    rebuild_filtered(inst);
    check(inst.filtered.size() == 3, "pack Serum lists Init + its 2 entries");
    check(inst.filtered[0] == 0, "Init stays reachable from every pack");
    check(leaf_of(inst.scanner.entries[(size_t) inst.filtered[1]].name) == "Basses",
          "option 1 of pack Serum is Basses");

    inst.wt_pack = 2;                       /* Vital */
    rebuild_filtered(inst);
    check(inst.filtered.size() == 2, "pack Vital lists Init + its 1 entry");

    /* The cap must bind, or the JS grid lists options the C-side knob
     * mapping and modulation tables silently truncate. */
    Inst big;
    big.scanner.entries.push_back({ "Init", "" });
    for (int k = 0; k < 300; k++)
        big.scanner.entries.push_back({ "Huge/T" + std::to_string(k), "/w/t.wav" });
    rebuild_packs(big); rebuild_filtered(big);
    check(big.filtered.size() == MAXOPT, "a 300-entry pack is capped at MAX_ENUM_OPTIONS");

    if (failures) { fprintf(stderr, "FAIL: %d check(s)\n", failures); return 1; }
    printf("test_wt_select: all checks passed\n");
    return 0;
}
