#include "Loudness.h"

#include <cmath>
#include <limits>

namespace
{
constexpr double minusInfinity = -std::numeric_limits<double>::infinity();
}

void LoudnessMeter::prepare (double sampleRate)
{
    // Pre-filter and RLB filter as in BS.1770, recomputed for the sample rate
    // the way libebur128 does it.
    {
        const double f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
        const double vh = std::pow (10.0, gainDb / 20.0);
        const double vb = std::pow (vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf.b0 = (vh + vb * k / q + k * k) / a0;
        shelf.b1 = 2.0 * (k * k - vh) / a0;
        shelf.b2 = (vh - vb * k / q + k * k) / a0;
        shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf.a2 = (1.0 - k / q + k * k) / a0;
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
        const double a0 = 1.0 + k / q + k * k;
        highpass.b0 = 1.0;
        highpass.b1 = -2.0;
        highpass.b2 = 1.0;
        highpass.a1 = 2.0 * (k * k - 1.0) / a0;
        highpass.a2 = (1.0 - k / q + k * k) / a0;
    }

    hopLength = juce::roundToInt (sampleRate / 10.0);
    doReset();
}

void LoudnessMeter::doReset()
{
    states.fill ({});
    hopSum.fill (0.0);
    hopFill = 0;
    hopCount.store (0);
}

void LoudnessMeter::process (const float* const* channels, int numChannels, int numSamples)
{
    if (resetRequested.exchange (false))
        doReset();

    numChannels = juce::jmin (numChannels, maxChannels);

    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& s = states[(size_t) ch];
            const double x = channels[ch][i];

            // Both stages in transposed direct form II
            const double y1 = shelf.b0 * x + s.shelf1;
            s.shelf1 = shelf.b1 * x - shelf.a1 * y1 + s.shelf2;
            s.shelf2 = shelf.b2 * x - shelf.a2 * y1;

            const double y2 = highpass.b0 * y1 + s.high1;
            s.high1 = highpass.b1 * y1 - highpass.a1 * y2 + s.high2;
            s.high2 = highpass.b2 * y1 - highpass.a2 * y2;

            hopSum[(size_t) ch] += y2 * y2;
        }

        if (++hopFill == hopLength)
        {
            double sum = 0.0;
            for (int ch = 0; ch < numChannels; ++ch)
                sum += hopSum[(size_t) ch] / hopLength;

            const int count = hopCount.load (std::memory_order_relaxed);
            if (count < capacity)
            {
                hops[(size_t) count] = sum;
                hopCount.store (count + 1, std::memory_order_release);
            }

            hopSum.fill (0.0);
            hopFill = 0;
        }
    }
}

double LoudnessMeter::integratedLufs() const
{
    const int count = hopCount.load (std::memory_order_acquire);
    if (count < hopsPerBlock)
        return minusInfinity;

    std::vector<double> blocks;
    blocks.reserve ((size_t) count);

    double window = 0.0;
    for (int i = 0; i < count; ++i)
    {
        window += hops[(size_t) i];
        if (i >= hopsPerBlock)
            window -= hops[(size_t) (i - hopsPerBlock)];

        if (i >= hopsPerBlock - 1)
        {
            const double z = window / hopsPerBlock;
            if (lufs (z) > -70.0)
                blocks.push_back (z);
        }
    }

    if (blocks.empty())
        return minusInfinity;

    double mean = 0.0;
    for (auto z : blocks)
        mean += z;
    mean /= (double) blocks.size();

    const double threshold = lufs (mean) - 10.0;
    double gatedSum = 0.0;
    int gatedCount = 0;
    for (auto z : blocks)
    {
        if (lufs (z) > threshold)
        {
            gatedSum += z;
            ++gatedCount;
        }
    }

    return gatedCount > 0 ? lufs (gatedSum / gatedCount) : minusInfinity;
}

double LoudnessMeter::shortTermLufs() const
{
    const int count = hopCount.load (std::memory_order_acquire);
    if (count == 0)
        return minusInfinity;

    const int n = juce::jmin (count, shortTermHops);
    double sum = 0.0;
    for (int i = count - n; i < count; ++i)
        sum += hops[(size_t) i];

    return lufs (sum / n);
}

double LoudnessMeter::measure (const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    LoudnessMeter meter;
    meter.prepare (sampleRate);

    const int numChannels = juce::jmin (buffer.getNumChannels(), maxChannels);
    const int total = buffer.getNumSamples();
    const float* channels[maxChannels] = {};

    for (int start = 0; start < total; start += 4096)
    {
        const int n = juce::jmin (4096, total - start);
        for (int ch = 0; ch < numChannels; ++ch)
            channels[ch] = buffer.getReadPointer (ch, start);
        meter.process (channels, numChannels, n);
    }

    return meter.integratedLufs();
}
