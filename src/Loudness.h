#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <vector>

// ITU-R BS.1770-4 / EBU R128 loudness: K-weighting, 400 ms blocks every 100 ms,
// absolute (-70 LUFS) and relative (-10 LU) gating. Mono or stereo.
class LoudnessMeter
{
public:
    void prepare (double sampleRate);

    // Any thread. Applied at the start of the next process() call.
    void reset() { resetRequested.store (true); }

    // Audio thread.
    void process (const float* const* channels, int numChannels, int numSamples);

    // Any thread. -inf until a block passed the gate.
    double integratedLufs() const;

    // Any thread. Loudness of the last 3 seconds.
    double shortTermLufs() const;

    // Measures a whole buffer offline.
    static double measure (const juce::AudioBuffer<float>& buffer, double sampleRate);

    static double lufs (double meanSquare) { return -0.691 + 10.0 * std::log10 (meanSquare); }

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };

    struct ChannelState
    {
        double shelf1 = 0, shelf2 = 0, high1 = 0, high2 = 0;
    };

    static constexpr int maxChannels = 2;
    static constexpr int hopsPerBlock = 4;              // 400 ms block, 100 ms hop
    static constexpr int shortTermHops = 30;            // 3 s
    static constexpr int capacity = 2 * 60 * 60 * 10;   // two hours of hops

    void doReset();

    Biquad shelf, highpass;
    std::array<ChannelState, maxChannels> states;
    std::array<double, maxChannels> hopSum {};
    int hopLength = 4800, hopFill = 0;

    // Mean square per hop, summed over channels. The audio thread writes at
    // hopCount and publishes by incrementing it; readers never go past it.
    std::vector<double> hops = std::vector<double> (capacity, 0.0);
    std::atomic<int> hopCount { 0 };
    std::atomic<bool> resetRequested { false };
};
