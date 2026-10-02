#include <juce_audio_basics/juce_audio_basics.h>

#include "Engine.h"
#include "Loudness.h"

#include <cmath>
#include <cstdio>
#include <memory>

static int failures = 0;

static void check (bool ok, const char* what)
{
    std::printf ("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (! ok)
        ++failures;
}

static void checkNear (double value, double expected, double tolerance, const char* what)
{
    const bool ok = std::abs (value - expected) <= tolerance;
    std::printf ("%s %s (%.3f, expected %.3f ± %.3f)\n", ok ? "ok  " : "FAIL", what, value, expected, tolerance);
    if (! ok)
        ++failures;
}

// EBU Tech 3341: a stereo 1 kHz sine at -23 dBFS reads -23.0 LUFS ± 0.1.
static void testLoudness (double sampleRate)
{
    const int n = (int) (sampleRate * 10);
    juce::AudioBuffer<float> buffer (2, n);
    const float amplitude = juce::Decibels::decibelsToGain (-23.0f);
    for (int i = 0; i < n; ++i)
    {
        const float v = amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / sampleRate);
        buffer.setSample (0, i, v);
        buffer.setSample (1, i, v);
    }
    checkNear (LoudnessMeter::measure (buffer, sampleRate), -23.0, 0.1, "1 kHz sine at -23 dBFS");

    buffer.applyGain (juce::Decibels::decibelsToGain (-10.0f));
    checkNear (LoudnessMeter::measure (buffer, sampleRate), -33.0, 0.1, "same signal 10 dB lower");

    juce::AudioBuffer<float> silence (2, n);
    silence.clear();
    check (std::isinf (LoudnessMeter::measure (silence, sampleRate)), "silence is gated out");
}

static std::shared_ptr<LoadedAudio> makeRamp (int length, double sampleRate)
{
    auto audio = std::make_shared<LoadedAudio>();
    audio->sampleRate = sampleRate;
    audio->buffer.setSize (2, length);
    for (int i = 0; i < length; ++i)
    {
        audio->buffer.setSample (0, i, (float) i / 1.0e6f);
        audio->buffer.setSample (1, i, -(float) i / 1.0e6f);
    }
    return audio;
}

static bool blockEquals (const juce::AudioBuffer<float>& buffer, float value)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (std::abs (buffer.getSample (ch, i) - value) > 1.0e-6f)
                return false;
    return true;
}

static void fillLive (juce::AudioBuffer<float>& buffer, float value)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), value, buffer.getNumSamples());
}

static void testEngine()
{
    const double sampleRate = 48000.0;
    const int block = 512;
    Engine engine;
    engine.prepare (sampleRate, block);

    const int fileLength = (int) sampleRate * 4;
    engine.setSlotAudio (1, makeRamp (fileLength, sampleRate));
    engine.slot (1).offsetSeconds.store (1.0);   // file starts at host second 1

    juce::AudioBuffer<float> buffer (2, block);

    // Live passes through while nothing else is selected
    fillLive (buffer, 0.5f);
    engine.process (buffer, 0, true, false);
    check (blockEquals (buffer, 0.5f), "live passes through on slot A");

    // Switch to B: the crossfade finishes within fadeLength samples
    engine.setActiveSlot (1);
    int64_t host = 24000;   // 0.5 s before the file starts: B is silent here
    fillLive (buffer, 0.5f);
    engine.process (buffer, host, true, false);
    check (std::abs (buffer.getSample (0, 0) - 0.5f) < 0.01f, "fade starts at the live level");
    check (std::abs (buffer.getSample (0, engine.getFadeLength())) < 1.0e-6f, "fade reaches B after fadeLength samples");
    host += block;

    // Run up to the file start and compare sample-exactly
    while (host < 48000 - block)
    {
        fillLive (buffer, 0.5f);
        engine.process (buffer, host, true, false);
        host += block;
    }
    bool exact = true;
    for (int b = 0; b < 20; ++b)
    {
        fillLive (buffer, 0.5f);
        engine.process (buffer, host, true, false);
        for (int i = 0; i < block; ++i)
        {
            const int64_t fileIndex = host + i - 48000;
            const float expected = fileIndex >= 0 ? (float) fileIndex / 1.0e6f : 0.0f;
            if (std::abs (buffer.getSample (0, i) - expected) > 1.0e-7f || std::abs (buffer.getSample (1, i) + expected) > 1.0e-7f)
                exact = false;
        }
        host += block;
    }
    check (exact, "B follows the host position sample-exactly");

    // Past the end of the file B is silent
    fillLive (buffer, 0.5f);
    engine.process (buffer, 48000 + fileLength + 1000, true, false);
    check (blockEquals (buffer, 0.0f), "B is silent after the file ends");

    // Gain: +6 dB trim doubles the output once the smoothing settled
    engine.slot (1).trimDb.store (6.0206f);
    for (int b = 0; b < 10; ++b)
    {
        fillLive (buffer, 0.5f);
        engine.process (buffer, 96000 + b * block, true, false);
    }
    {
        const int64_t pos = 96000 + 10 * block;
        fillLive (buffer, 0.5f);
        engine.process (buffer, pos, true, false);
        const float expected = 2.0f * (float) (pos - 48000) / 1.0e6f;
        checkNear (buffer.getSample (0, 0), expected, 1.0e-5, "trim applies as gain");
    }
    engine.slot (1).trimDb.store (0.0f);

    // Stopped transport and offline renders always return the live signal
    fillLive (buffer, 0.5f);
    engine.process (buffer, 96000, false, false);
    for (int b = 0; b < 4; ++b)
    {
        fillLive (buffer, 0.5f);
        engine.process (buffer, 96000, false, false);
    }
    check (blockEquals (buffer, 0.5f), "stopped transport passes live through");

    engine.process (buffer, 96000, true, false);   // back to B
    for (int b = 0; b < 4; ++b)
    {
        fillLive (buffer, 0.5f);
        engine.process (buffer, 96000, true, true);
    }
    check (blockEquals (buffer, 0.5f), "offline render passes live through");

    // Missing host position: live too
    for (int b = 0; b < 4; ++b)
    {
        fillLive (buffer, 0.5f);
        engine.process (buffer, std::nullopt, true, false);
    }
    check (blockEquals (buffer, 0.5f), "no host position passes live through");

    // Empty slot: silence, no crash
    engine.setActiveSlot (2);
    for (int b = 0; b < 4; ++b)
    {
        fillLive (buffer, 0.5f);
        engine.process (buffer, 96000, true, false);
    }
    check (blockEquals (buffer, 0.0f), "empty slot is silent");

    // Replacing audio keeps the old buffer alive until released
    auto first = engine.getSlotAudio (1);
    engine.setSlotAudio (1, makeRamp (100, sampleRate));
    check (first.use_count() == 2, "replaced audio sits in the release pool");
    first.reset();
    engine.clearReleasePool();
    check (engine.getSlotAudio (1)->buffer.getNumSamples() == 100, "new audio is active");
}

int main()
{
    testLoudness (48000.0);
    testLoudness (44100.0);
    testEngine();

    std::printf ("%s\n", failures == 0 ? "All tests passed" : "Tests failed");
    return failures == 0 ? 0 : 1;
}
