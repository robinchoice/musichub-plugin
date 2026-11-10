#include "Capture.h"

#include "Check.h"

#include <vector>

namespace
{
struct CaptureDirectory
{
    ~CaptureDirectory() { directory.deleteRecursively(); }

    int numFiles() const { return directory.getNumberOfChildFiles (juce::File::findFiles, "*"); }

    juce::File directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getNonexistentChildFile ("MusicHubCapture", "");
};

// Distinct 24-bit values, so the file must hold exactly these samples
int sampleValue (int channel, int index)
{
    return ((index * 7919 + channel * 104729) % (1 << 23)) - (1 << 22);
}

void fillBlock (juce::AudioBuffer<float>& block, int firstIndex)
{
    for (int ch = 0; ch < block.getNumChannels(); ++ch)
        for (int i = 0; i < block.getNumSamples(); ++i)
            block.setSample (ch, i, (float) sampleValue (ch, firstIndex + i) / (float) (1 << 23));
}

void testOfflineBounce (int numChannels, double sampleRate)
{
    CaptureDirectory dir;
    Capture capture (dir.directory);
    capture.setArmed (true);

    constexpr int blockSize = 512, numBlocks = 40;
    juce::AudioBuffer<float> block (numChannels, blockSize);

    capture.setNonRealtime (true);
    for (int b = 0; b < numBlocks; ++b)
    {
        fillBlock (block, b * blockSize);
        capture.process (block, numChannels, sampleRate, true);
    }
    check (capture.isRecording(), "an armed offline render records");
    capture.setNonRealtime (false);

    check (! capture.isRecording() && ! capture.isArmed(), "leaving offline mode ends the capture and disarms");
    const auto recording = capture.takeFinished();
    check (recording.has_value() && recording->error.isEmpty(), "the capture is handed over");
    if (! recording.has_value() || recording->error.isNotEmpty())
        return;
    checkNear (recording->seconds, numBlocks * blockSize / sampleRate, 1e-9, "recorded length");
    check (recording->file.getFileExtension() == ".flac" && recording->file.isAChildOf (dir.directory), "FLAC in the captures directory");

    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader (flac.createReaderFor (recording->file.createInputStream().release(), true));
    check (reader != nullptr, "the file reads as FLAC");
    if (reader == nullptr)
        return;
    check (reader->bitsPerSample == 24 && (int) reader->numChannels == numChannels
           && std::abs (reader->sampleRate - sampleRate) < 0.5, "24 bit, host rate, output channels");
    check (reader->lengthInSamples == numBlocks * blockSize, "every sample of every block");

    const int length = numBlocks * blockSize;
    std::vector<std::vector<int>> data ((size_t) numChannels, std::vector<int> ((size_t) length));
    std::vector<int*> channels;
    for (auto& c : data)
        channels.push_back (c.data());
    reader->read (channels.data(), numChannels, 0, length, false);

    bool exact = true;
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < length; ++i)
            exact = exact && (data[(size_t) ch][(size_t) i] >> 8) == sampleValue (ch, i);
    check (exact, "the file holds exactly the input samples");
}

void testNothingWithoutSwitch()
{
    CaptureDirectory dir;
    Capture capture (dir.directory);
    juce::AudioBuffer<float> block (2, 256);
    fillBlock (block, 0);

    capture.setNonRealtime (true);
    for (int b = 0; b < 10; ++b)
        capture.process (block, 2, 48000.0, true);
    capture.setNonRealtime (false);
    check (! capture.takeFinished().has_value() && dir.numFiles() == 0, "unarmed offline render writes no file");

    capture.setArmed (true);
    for (int b = 0; b < 10; ++b)
        capture.process (block, 2, 48000.0, false);
    check (! capture.takeFinished().has_value() && dir.numFiles() == 0, "armed realtime playback writes no file");
    check (capture.isArmed(), "and stays armed for the next offline bounce");

    // Armed only after the host went offline: that render isn't the one meant
    capture.setNonRealtime (true);
    capture.setArmed (false);
    capture.setArmed (true);
    capture.setNonRealtime (false);
    check (dir.numFiles() == 0, "switching offline and back without blocks writes nothing");
}

// A host that stops rendering but never leaves offline mode
void testIdleTimeout()
{
    CaptureDirectory dir;
    Capture capture (dir.directory);
    capture.setArmed (true);
    juce::AudioBuffer<float> block (2, 256);
    fillBlock (block, 0);

    capture.setNonRealtime (true);
    capture.process (block, 2, 44100.0, true);
    const auto now = juce::Time::getMillisecondCounter();
    capture.finishIfIdle (now);
    check (capture.isRecording(), "a short pause keeps recording");
    capture.finishIfIdle (now + Capture::idleTimeoutMs + 1000);
    check (! capture.isRecording(), "two seconds without a block end the capture");
    const auto recording = capture.takeFinished();
    check (recording.has_value() && recording->file.existsAsFile(), "the timed-out capture is handed over");
}

void testCleanUp()
{
    CaptureDirectory dir;
    dir.directory.createDirectory();
    const auto old = dir.directory.getChildFile ("old.flac");
    const auto oldKept = dir.directory.getChildFile ("kept.flac");
    const auto fresh = dir.directory.getChildFile ("fresh.flac");
    for (const auto& f : { old, oldKept, fresh })
        f.replaceWithText ("x");
    const auto weekAgo = juce::Time::getCurrentTime() - juce::RelativeTime::days (8);
    old.setLastModificationTime (weekAgo);
    oldKept.setLastModificationTime (weekAgo);

    Capture::cleanUp (dir.directory, oldKept);
    check (! old.exists() && oldKept.exists() && fresh.exists(), "captures older than a week are deleted, except the kept one");
}
} // namespace

void testCapture()
{
    testOfflineBounce (2, 48000.0);
    testOfflineBounce (1, 44100.0);
    testNothingWithoutSwitch();
    testIdleTimeout();
    testCleanUp();
}
