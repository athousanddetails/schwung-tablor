/* Background wavetable loader — the machinery behind glitch-free switching.
 *
 * set_param posts a request; this worker thread decodes (WAV or .wtNNNN
 * FLAC), infers the frame size, normalizes, builds the decimated mip
 * pyramid, and hands the finished shared_ptr<Wavetable> to the engine via
 * an atomic swap. The audio thread never sees a partial table and never
 * touches the filesystem. In-flight notes finish on the old table (the
 * engine's render block pins it with a shared_ptr).
 *
 * A small cache means WT1 and WT2 picking the same table share one build.
 */
#pragma once

#include "scanner.h"
#include "../engine.h"
#include "../../ported/wav.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace tb {

/* implemented in loader.cpp (dr_flac lives there) */
bool wtDecodeFlac(const char *path, std::vector<float> &samples,
                  float &sampleRate);

class WtLoader {
public:
    /* ~24 MB per oscillator: frames are subsampled to fit if a pathological
     * table would blow past it (PLAN 5.4). ~12,384 samples/frame decimated. */
    static constexpr size_t kMaxBytesPerTable = 24u * 1024 * 1024;
    static constexpr int    kBytesPerFrame    = 12384 * 4;

    explicit WtLoader(Engine &engine_) : engine(engine_) {}

    ~WtLoader()
    {
        {
            std::lock_guard<std::mutex> lk(m);
            quit = true;
        }
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }

    /* UI thread. osc: 0/1. entry: a copy of the scanner row. */
    void requestLoad(int osc, const WtEntry &entry)
    {
        {
            std::lock_guard<std::mutex> lk(m);
            pending[osc & 1] = entry;
            hasPending[osc & 1] = true;
            if (!worker.joinable())
                worker = std::thread([this] { run(); });
        }
        cv.notify_all();
    }

    bool busy() const
    {
        std::lock_guard<std::mutex> lk(m);
        return hasPending[0] || hasPending[1] || working;
    }

private:
    void run()
    {
        for (;;) {
            WtEntry entry;
            int osc = -1;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [this] { return quit || hasPending[0] || hasPending[1]; });
                if (quit) return;
                for (int i = 0; i < 2; i++) {
                    if (hasPending[i]) {
                        entry = pending[i];
                        hasPending[i] = false;
                        osc = i;
                        break;
                    }
                }
                working = true;
            }

            std::shared_ptr<Wavetable> table = load(entry);
            if (table)
                engine.setTable(osc, std::move(table));

            {
                std::lock_guard<std::mutex> lk(m);
                working = false;
            }
        }
    }

    std::shared_ptr<Wavetable> load(const WtEntry &entry)
    {
        if (entry.path.empty())
            return makeInitTable();

        /* cache hit? (weak: a table both oscs use is built once) */
        {
            std::lock_guard<std::mutex> lk(cacheM);
            auto it = cache.find(entry.path);
            if (it != cache.end())
                if (auto sp = it->second.lock())
                    return sp;
        }

        std::vector<float> samples;
        float rate = 44100.0f;
        int frameSize = 0;

        if (entry.flacFrameSize > 0) {
            if (!wtDecodeFlac(entry.path.c_str(), samples, rate))
                return nullptr;
            frameSize = entry.flacFrameSize;
        } else {
            WavData w;
            if (!wavLoad(entry.path.c_str(), w))
                return nullptr;
            samples = std::move(w.samples);
            rate = w.sampleRate > 0 ? w.sampleRate : 44100.0f;
            frameSize = wavInferFrameSize(w);
            if (!frameSize)
                frameSize = 2048;                  /* last resort default */
        }

        int nFrames = (int) samples.size() / frameSize;
        if (nFrames < 1) return nullptr;

        /* memory ceiling: subsample frames evenly to fit */
        int maxFrames = (int) (kMaxBytesPerTable / (size_t) kBytesPerFrame);
        if (nFrames > maxFrames) {
            std::vector<float> picked((size_t) maxFrames * (size_t) frameSize);
            for (int i = 0; i < maxFrames; i++) {
                int src = (int) ((long) i * (nFrames - 1) / (maxFrames - 1));
                std::memcpy(&picked[(size_t) i * frameSize],
                            &samples[(size_t) src * frameSize],
                            (size_t) frameSize * sizeof(float));
            }
            samples = std::move(picked);
            nFrames = maxFrames;
        } else {
            samples.resize((size_t) nFrames * (size_t) frameSize);
        }

        /* normalize to 0.9 peak for consistent loudness across libraries */
        float peak = 0.0f;
        for (float v : samples) peak = std::max(peak, std::fabs(v));
        if (peak > 0.0f) {
            float g = 0.9f / peak;
            for (float &v : samples) v *= g;
        }

        auto wt = std::make_shared<Wavetable>();
        if (!wtBuild(*wt, samples.data(), nFrames, frameSize, 44100.0f, rate))
            return nullptr;

        {
            std::lock_guard<std::mutex> lk(cacheM);
            cache[entry.path] = wt;
            /* drop dead cache rows now and then */
            if (cache.size() > 16)
                for (auto it = cache.begin(); it != cache.end();)
                    it = it->second.expired() ? cache.erase(it) : std::next(it);
        }
        return wt;
    }

    Engine &engine;

    mutable std::mutex m;
    std::condition_variable cv;
    std::thread worker;
    WtEntry pending[2];
    bool hasPending[2] = { false, false };
    bool working = false;
    bool quit = false;

    std::mutex cacheM;
    std::map<std::string, std::weak_ptr<Wavetable>> cache;
};

} // namespace tb
