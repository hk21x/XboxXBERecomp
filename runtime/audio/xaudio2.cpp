// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace audio {

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IXAudio2* xa = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xa->CreateMasteringVoice(&master)) ||
            FAILED(xa->CreateSourceVoice(&voice, &wf))) {
            LOGE("audio: XAudio2 unavailable, no sound");
            if (!app::kGame.silent_clock) return;
            // GameInfo::silent_clock: the mixer pulled as a device would, 10 ms a time on the clock, and dropped
            LOGR("audio: the mixer runs without output (GameInfo::silent_clock)");
            constexpr uint32_t kFrames = kRate / 100;
            std::vector<float> buf(kFrames * 2);
            LARGE_INTEGER freq, now;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&now);
            const double tick = double(freq.QuadPart) / 100.0;
            double next = double(now.QuadPart);
            for (;;) {
                QueryPerformanceCounter(&now);
                if (app::Paused()) {
                    next = double(now.QuadPart);
                    Sleep(2);
                    continue;
                }
                if (double(now.QuadPart) < next) {
                    Sleep(1);
                    continue;
                }
                render(buf.data(), kFrames);
                next += tick;
                if (double(now.QuadPart) - next > tick * 10) next = double(now.QuadPart);  // after a stall: no catching up
            }
        }
        voice->Start();
        constexpr uint32_t kFrames = kRate / 100, kSlots = 4;
        std::vector<float> bufs(kSlots * kFrames * 2);
        for (uint32_t n = 0;;) {
            XAUDIO2_VOICE_STATE st;
            voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            // paused (app::Paused): the mixer is not pulled, so the title's sound and its sample clock stand still
            if (st.BuffersQueued >= 3 || app::Paused()) {
                Sleep(2);
                continue;
            }
            float* b = &bufs[(n % kSlots) * kFrames * 2];
            render(b, kFrames);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kFrames * 8;
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            voice->SubmitSourceBuffer(&xb);
            n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
        }
    }).detach();
}

}  // namespace audio
