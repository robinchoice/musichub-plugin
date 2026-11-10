#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>
#include <optional>

// Records the plugin's input while the DAW bounces offline, once the user has
// armed it. The capture starts with the first offline block after the host
// switched to offline rendering and ends when it switches back, or two seconds
// after the last block. Realtime blocks are never touched.
class Capture
{
public:
    struct Recording
    {
        juce::File file;
        double seconds = 0;
        juce::String error;   // the file is gone then
    };

    explicit Capture (juce::File directory);
    ~Capture();

    const juce::File& getDirectory() const noexcept { return directory; }

    // Any thread. Disarms itself when a capture ends.
    void setArmed (bool shouldBeArmed) noexcept { armed.store (shouldBeArmed); }
    bool isArmed() const noexcept { return armed.load(); }
    bool isRecording() const noexcept { return recording.load(); }
    double getRecordedSeconds() const noexcept { return recordedSeconds.load(); }

    // Host thread, from AudioProcessor::setNonRealtime
    void setNonRealtime (bool offline);

    // Audio thread, with the input before anything changes it. Writes
    // blocking, which offline rendering allows.
    void process (const juce::AudioBuffer<float>& buffer, int numChannels, double sampleRate, bool nonRealtime);

    // Message thread: ends a capture whose host stopped sending blocks without
    // leaving offline mode. `now` is juce::Time::getMillisecondCounter().
    void finishIfIdle (juce::uint32 now);

    // Message thread: the capture that ended since the last call, if any
    std::optional<Recording> takeFinished();

    static constexpr juce::uint32 idleTimeoutMs = 2000;

    // Deletes captures older than a week, then the oldest ones beyond 2 GB.
    // `keep` survives either way.
    static void cleanUp (const juce::File& directory, const juce::File& keep = {});

private:
    void start (int numChannels, double sampleRate);
    void finish();   // with lock held

    const juce::File directory;
    juce::CriticalSection lock;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::File file;
    juce::int64 samplesWritten = 0;
    double rate = 0;
    bool writeFailed = false;
    std::optional<Recording> finished;

    std::atomic<bool> armed { false }, pending { false }, recording { false };
    std::atomic<double> recordedSeconds { 0.0 };
    std::atomic<juce::uint32> lastBlockMs { 0 };

    JUCE_DECLARE_NON_COPYABLE (Capture)
};
