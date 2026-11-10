#include "Capture.h"

#include <algorithm>
#include <vector>

namespace
{
constexpr juce::int64 maxDirectoryBytes = (juce::int64) 2 * 1024 * 1024 * 1024;
const auto maxAge = juce::RelativeTime::days (7);
} // namespace

Capture::Capture (juce::File dir) : directory (std::move (dir)) {}

Capture::~Capture()
{
    const juce::ScopedLock sl (lock);
    if (writer != nullptr)
        finish();
}

void Capture::setNonRealtime (bool offline)
{
    if (offline)
    {
        if (! recording.load())
            pending.store (armed.load());
        return;
    }

    const juce::ScopedLock sl (lock);
    pending.store (false);
    if (writer != nullptr)
        finish();
}

void Capture::process (const juce::AudioBuffer<float>& buffer, int numChannels, double sampleRate, bool nonRealtime)
{
    // The realtime path: no lock, no allocation, no file access
    if (! nonRealtime || (! pending.load() && ! recording.load()))
        return;

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    const juce::ScopedLock sl (lock);
    if (pending.exchange (false))
        start (numChannels, sampleRate);
    if (writer == nullptr)
        return;

    if (! writer->writeFromFloatArrays (buffer.getArrayOfReadPointers(), numChannels, numSamples))
        writeFailed = true;
    samplesWritten += numSamples;
    recordedSeconds.store ((double) samplesWritten / rate);
    lastBlockMs.store (juce::Time::getMillisecondCounter());
}

void Capture::start (int numChannels, double sampleRate)
{
    rate = sampleRate;
    samplesWritten = 0;
    writeFailed = false;
    recordedSeconds.store (0.0);
    lastBlockMs.store (juce::Time::getMillisecondCounter());

    directory.createDirectory();
    file = directory.getNonexistentChildFile ("Bounce " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"),
                                              ".flac", false);

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    if (static_cast<juce::FileOutputStream&> (*stream).openedOk())
        writer = juce::FlacAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                                      .withSampleRate (sampleRate)
                                                                      .withNumChannels (numChannels)
                                                                      .withBitsPerSample (24)
                                                                      .withQualityOptionIndex (5));
    if (writer == nullptr)
    {
        stream.reset();
        file.deleteFile();
        armed.store (false);
        finished = Recording { {}, 0, "Aufnahme konnte nicht gestartet werden" };
        return;
    }

    recording.store (true);
}

void Capture::finish()
{
    writer.reset();   // writes the FLAC trailer and closes the file
    recording.store (false);
    armed.store (false);

    Recording result { file, (double) samplesWritten / rate, {} };
    if (writeFailed || ! file.existsAsFile())
    {
        file.deleteFile();
        result = { {}, 0, "Aufnahme konnte nicht geschrieben werden (Speicherplatz?)" };
    }
    finished = result;
}

void Capture::finishIfIdle (juce::uint32 now)
{
    if (! recording.load())
        return;

    const auto last = lastBlockMs.load();
    if (now <= last || now - last <= idleTimeoutMs)
        return;

    const juce::ScopedLock sl (lock);
    if (writer != nullptr && lastBlockMs.load() == last)
        finish();
}

std::optional<Capture::Recording> Capture::takeFinished()
{
    const juce::ScopedLock sl (lock);
    return std::exchange (finished, std::nullopt);
}

void Capture::cleanUp (const juce::File& dir, const juce::File& keep)
{
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.flac");
    const auto cutoff = juce::Time::getCurrentTime() - maxAge;

    std::vector<juce::File> left;
    for (const auto& f : files)
    {
        if (f != keep && f.getLastModificationTime() < cutoff)
            f.deleteFile();
        else
            left.push_back (f);
    }

    std::sort (left.begin(), left.end(), [] (const auto& a, const auto& b)
    {
        return a.getLastModificationTime() < b.getLastModificationTime();
    });

    juce::int64 total = 0;
    for (const auto& f : left)
        total += f.getSize();

    for (const auto& f : left)
    {
        if (total <= maxDirectoryBytes)
            break;
        if (f == keep)
            continue;
        total -= f.getSize();
        f.deleteFile();
    }
}
