// Denoiser48: what the noise removal needs from an engine (RNNoise or DeepFilterNet), always mono lanes at 48 kHz.
#pragma once

#include <cstdint>

namespace hr::denoise
{
constexpr int kFrame = 480;         // 10 ms at 48 kHz: RNNoise's frame and DeepFilterNet's hop
constexpr int kMaxLanes = 2;        // left and right (the mic is one lane: both sides are identical)

// A streaming denoiser with a fixed delay. Frames are collected inside, so any block size works.
//
// Real-time rules: process() and setGate()/setPostFilter() are called from the audio thread only and never
// allocate, lock or wait. Everything else may be called from any thread.
class Denoiser48
{
public:
    virtual ~Denoiser48() = default;

    // Exactly: out[t] is the cleaned version of in[t - latency()], from the first call on (48 kHz samples).
    virtual int latency() const noexcept = 0;

    // numLanes is 1 or 2 and may change between calls. in and out may be the same arrays.
    virtual void process (const float* const* in, float* const* out, int numLanes, int n) noexcept = 0;

    // Voice probability 0..1 of the latest frame (RNNoise: its VAD; DeepFilterNet: from its local SNR estimate).
    virtual float voice() const noexcept = 0;

    // Light only: the voice gate on the cleaned signal (threshold 0..1, hold in frames).
    virtual void setGate (float threshold, int holdFrames) noexcept        { (void) threshold; (void) holdFrames; }
    virtual float gateLevel() const noexcept                               { return 1.0f; }

    // DeepFilterNet only: post filter strength (0 = off).
    virtual void setPostFilter (float beta) noexcept                       { (void) beta; }

    // DeepFilterNet only: frames for which the network's answer was not there in time and the input was let through
    // instead, and the share of one core the network's own thread uses (0..1).
    virtual uint32_t misses() const noexcept                               { return 0; }
    virtual float workerLoad() const noexcept                              { return 0.0f; }
    virtual float worstFrameMs() const noexcept                            { return 0.0f; }
};
}
