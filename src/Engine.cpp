#include "Engine.h"

void Engine::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate.store (newSampleRate);
    fadeLength = juce::jmax (1, juce::roundToInt (newSampleRate * 0.01));

    liveCopy.setSize (2, maxBlockSize);
    fromBuffer.setSize (2, maxBlockSize);
    toBuffer.setSize (2, maxBlockSize);

    for (auto& g : gains)
    {
        g.reset (newSampleRate, 0.02);
        g.setCurrentAndTargetValue (1.0f);
    }

    meter.prepare (newSampleRate);
    current = 0;
    fadeLeft = 0;
}

void Engine::setSlotAudio (int slot, std::shared_ptr<const LoadedAudio> newAudio)
{
    std::shared_ptr<const LoadedAudio> old;
    {
        const juce::SpinLock::ScopedLockType scoped (lock);
        old = std::move (audio[(size_t) slot]);
        audio[(size_t) slot] = std::move (newAudio);
    }
    if (old != nullptr)
        releasePool.push_back (std::move (old));
}

std::shared_ptr<const LoadedAudio> Engine::getSlotAudio (int slot) const
{
    const juce::SpinLock::ScopedLockType scoped (lock);
    return audio[(size_t) slot];
}

void Engine::clearReleasePool()
{
    releasePool.erase (std::remove_if (releasePool.begin(), releasePool.end(),
                                       [] (const auto& p) { return p.use_count() == 1; }),
                       releasePool.end());
}

void Engine::process (juce::AudioBuffer<float>& buffer, std::optional<int64_t> hostPosition, bool playing, bool nonRealtime)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (2, buffer.getNumChannels());
    if (numSamples == 0 || numChannels == 0)
        return;

    if (playing && ! nonRealtime)
        meter.process (buffer.getArrayOfReadPointers(), numChannels, numSamples);

    const int desired = (nonRealtime || ! playing || ! hostPosition.has_value()) ? 0 : active.load();
    if (desired != current)
    {
        fadeFrom = current;
        current = desired;
        fadeLeft = fadeLength;
    }
    playingSlot.store (current);

    if (current == 0 && fadeLeft == 0)
        return;

    for (int ch = 0; ch < numChannels; ++ch)
        liveCopy.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    render (current, liveCopy, toBuffer, hostPosition);

    if (fadeLeft > 0)
    {
        render (fadeFrom, liveCopy, fromBuffer, hostPosition);

        const int fadeSamples = juce::jmin (fadeLeft, numSamples);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* out = buffer.getWritePointer (ch);
            const auto* from = fromBuffer.getReadPointer (ch);
            const auto* to = toBuffer.getReadPointer (ch);

            for (int i = 0; i < fadeSamples; ++i)
            {
                const float t = 1.0f - (float) (fadeLeft - i) / (float) fadeLength;
                out[i] = from[i] * (1.0f - t) + to[i] * t;
            }
            for (int i = fadeSamples; i < numSamples; ++i)
                out[i] = to[i];
        }
        fadeLeft -= fadeSamples;
    }
    else
    {
        for (int ch = 0; ch < numChannels; ++ch)
            buffer.copyFrom (ch, 0, toBuffer, ch, 0, numSamples);
    }

    if (current != 0)
        for (int ch = numChannels; ch < buffer.getNumChannels(); ++ch)
            buffer.clear (ch, 0, numSamples);
}

void Engine::render (int source, const juce::AudioBuffer<float>& live, juce::AudioBuffer<float>& out, std::optional<int64_t> hostPosition)
{
    const int numSamples = live.getNumSamples();

    if (source == 0)
    {
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, 0, live, juce::jmin (ch, live.getNumChannels() - 1), 0, numSamples);
        return;
    }

    std::shared_ptr<const LoadedAudio> loaded;
    {
        const juce::SpinLock::ScopedTryLockType scoped (lock);
        if (scoped.isLocked())
            loaded = audio[(size_t) source];
    }

    if (loaded == nullptr || ! hostPosition.has_value())
    {
        out.clear (0, numSamples);
        return;
    }

    renderVersion (source, *loaded, *hostPosition, out);
}

void Engine::renderVersion (int slotIndex, const LoadedAudio& loaded, int64_t hostPosition, juce::AudioBuffer<float>& out)
{
    auto& s = slots[(size_t) slotIndex];
    const int numSamples = out.getNumSamples();
    const auto offsetSamples = (int64_t) std::llround (s.offsetSeconds.load() * sampleRate.load());
    const int64_t start = hostPosition - offsetSamples;
    const int64_t fileLength = loaded.buffer.getNumSamples();

    out.clear (0, numSamples);

    const int64_t first = juce::jmax<int64_t> (start, 0);
    const int64_t last = juce::jmin<int64_t> (start + numSamples, fileLength);
    if (last > first)
    {
        const int destOffset = (int) (first - start);
        const int count = (int) (last - first);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, destOffset, loaded.buffer, juce::jmin (ch, loaded.buffer.getNumChannels() - 1), (int) first, count);
    }

    auto& gain = gains[(size_t) slotIndex];
    gain.setTargetValue (juce::Decibels::decibelsToGain (s.trimDb.load() + s.matchDb.load(), -60.0f));
    gain.applyGain (out, numSamples);
}
