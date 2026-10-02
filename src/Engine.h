#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "Loudness.h"

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

// A decoded version, resampled to the engine's sample rate.
struct LoadedAudio
{
    juce::AudioBuffer<float> buffer;
    double sampleRate = 0;
    double integratedLufs = -std::numeric_limits<double>::infinity();
};

struct Slot
{
    // Host time at which the file's first sample plays
    std::atomic<double> offsetSeconds { 0.0 };
    std::atomic<float> trimDb { 0.0f };
    std::atomic<float> matchDb { 0.0f };
};

// Plays the active source: the live input (slot 0) or a loaded version that
// follows the host transport. Switches crossfade; offline renders always pass
// the live signal through so a bounce never contains a reference.
class Engine
{
public:
    static constexpr int numSlots = 4;

    void prepare (double sampleRate, int maxBlockSize);

    // Audio thread. hostPosition is the host's sample position for the first sample of the block.
    void process (juce::AudioBuffer<float>& buffer, std::optional<int64_t> hostPosition, bool playing, bool nonRealtime);

    void setActiveSlot (int slot) { active.store (juce::jlimit (0, numSlots - 1, slot)); }
    int getActiveSlot() const { return active.load(); }
    // The slot that is actually audible after the pass-through rules
    int getPlayingSlot() const { return playingSlot.load(); }

    Slot& slot (int index) { return slots[(size_t) index]; }

    // Message thread. The replaced audio stays in a release pool so the audio
    // thread never frees it; clearReleasePool() drops it once unused.
    void setSlotAudio (int slot, std::shared_ptr<const LoadedAudio> audio);
    std::shared_ptr<const LoadedAudio> getSlotAudio (int slot) const;
    void clearReleasePool();

    LoudnessMeter& liveMeter() { return meter; }
    double getSampleRate() const { return sampleRate.load(); }
    int getFadeLength() const { return fadeLength; }

private:
    void render (int source, const juce::AudioBuffer<float>& live, juce::AudioBuffer<float>& out, std::optional<int64_t> hostPosition);
    void renderVersion (int slotIndex, const LoadedAudio& audio, int64_t hostPosition, juce::AudioBuffer<float>& out);

    std::atomic<double> sampleRate { 48000.0 };
    int fadeLength = 480;

    std::array<Slot, numSlots> slots;
    std::array<std::shared_ptr<const LoadedAudio>, numSlots> audio;
    mutable juce::SpinLock lock;
    std::vector<std::shared_ptr<const LoadedAudio>> releasePool;

    std::atomic<int> active { 0 };
    std::atomic<int> playingSlot { 0 };
    int current = 0, fadeFrom = 0, fadeLeft = 0;

    juce::AudioBuffer<float> liveCopy, fromBuffer, toBuffer;
    std::array<juce::LinearSmoothedValue<float>, numSlots> gains;
    LoudnessMeter meter;
};
