// the mixer pulled every 10 ms by the clock (on a fixed schedule, as a sound device pulls), its output dropped
// WAV=file: the output also written there (relative to the run folder), 48 kHz 16 bit stereo, the header kept current
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

// a RIFF WAVE header for bytes of 16 bit stereo PCM at kRate
void WavHeader(FILE* f, uint32_t bytes) {
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fseek(f, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);  // PCM
    u16(2);
    u32(kRate);
    u32(kRate * 4);
    u16(4);
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(bytes);
    std::fseek(f, 0, SEEK_END);
}

}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    FILE* wav = nullptr;
    if (const char* name = std::getenv("WAV"); name && *name) {
        wav = std::fopen((app::ExeDir() / name).string().c_str(), "wb");
        if (wav) WavHeader(wav, 0);
        LOGI("audio: output started, written to %s", name);  // the log's time of the file's first sample
    }
    std::thread([render, wav] {
        std::vector<float> b(kRate / 100 * 2);
        std::vector<int16_t> pcm(b.size());
        uint32_t bytes = 0;
        auto next = std::chrono::steady_clock::now();
        for (int n = 0;; n++) {
            render(b.data(), kRate / 100);
            if (wav) {
                for (size_t i = 0; i < b.size(); i++) pcm[i] = int16_t(std::lround(b[i] * 32767.0f));
                std::fwrite(pcm.data(), 2, pcm.size(), wav);
                bytes += uint32_t(pcm.size() * 2);
                if (n % 100 == 0) {  // a run ends by being killed: the header says what is there each second
                    WavHeader(wav, bytes);
                    std::fflush(wav);
                }
            }
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
        }
    }).detach();
}

}  // namespace audio
