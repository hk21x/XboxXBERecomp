// mixer output through SDL3 audio (macOS, and any platform without XAudio2)
#include <SDL3/SDL.h>

#include <chrono>
#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace audio {

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOGE("audio: SDL audio unavailable (%s), no sound", SDL_GetError());
            return;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(kRate)};
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) {
            LOGE("audio: no output device (%s), no sound", SDL_GetError());
            return;
        }
        SDL_ResumeAudioStreamDevice(stream);
        LOGR("audio: output started, %s", SDL_GetCurrentAudioDriver());
        // as the XAudio2 output: 10 ms blocks, at most three queued ahead of the device
        constexpr uint32_t kFrames = kRate / 100;
        constexpr int kQueued = 3 * kFrames * 8;
        std::vector<float> buf(kFrames * 2);
        for (;;) {
            // paused (app::Paused): the mixer is not pulled, so the title's sound and its sample clock stand still
            if (SDL_GetAudioStreamQueued(stream) >= kQueued || app::Paused()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            render(buf.data(), kFrames);
            SDL_PutAudioStreamData(stream, buf.data(), int(kFrames * 8));
        }
    }).detach();
}

}  // namespace audio
