// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include <chrono>
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
            LOGE("audio: XAudio2 unavailable, no sound; the mixer is pulled on the clock instead");
            if (voice) voice->DestroyVoice();
            if (master) master->DestroyVoice();
            if (xa) xa->Release();
            // a title's sound still runs by the samples taken: with nothing taking them, its streams stood still, and
            // so did 50 Cent: Bulletproof's CRI movies, timed by their sound (a machine with no audio device stayed on
            // the first logo). Pulled every 10 ms, as a device would, the output dropped
            std::vector<float> b(kRate / 100 * 2);
            auto next = std::chrono::steady_clock::now();
            for (;;) {
                if (!app::Paused()) render(b.data(), kRate / 100);
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
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
