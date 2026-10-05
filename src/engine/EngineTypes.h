// Engine-wide constants and the real-time I/O contract between a backend and the AudioEngine.
#pragma once

namespace hr
{
constexpr int kMaxAppChannels = 16;     // fixed port slots on the PipeWire filter
constexpr int kMaxInputChannels = 8;    // fixed port slots for extra capture devices (a second mic, a capture card, a line-in)
constexpr int kMaxBlock       = 1024;   // engine splits larger quanta into chunks

struct EngineIO
{
    // One stereo pair per app-channel slot. nullptr = port not linked -> treat as silence.
    const float* appIn[kMaxAppChannels][2] {};
    const float* micIn[2] {};           // [1] may be nullptr (mono mic)
    // One stereo pair per input-channel slot. nullptr = port not linked -> silence. The backend links a mono
    // device to both sides, so the engine always sees a pair; it never has to guess.
    const float* inputIn[kMaxInputChannels][2] {};
    float* monitorOut[2] {};            // -> headphones (may be nullptr)
    float* streamOut[2] {};             // -> "Headroom Stream Mix" virtual source
    float* micOut[2] {};                // -> "Headroom Mic" virtual source (processed mic)
    int numFrames = 0;                  // any size 1...8192
    double sampleRate = 48000.0;
};

// A gain in dB at or below this means -infinity everywhere (model, UI, engine).
constexpr float kMinusInfDb = -100.0f;

// The two mixes every channel feeds. The values are used as array indices.
enum class Mix : int { Monitor = 0, Stream = 1 };
constexpr int kNumMixes = 2;

// Engine channel indices: app slots 0..kMaxAppChannels-1, then the mic, then the input slots.
constexpr int kMicChannel    = kMaxAppChannels;
constexpr int kInputChannel0 = kMaxAppChannels + 1;
constexpr int kNumChannels   = kMaxAppChannels + 1 + kMaxInputChannels;
}
