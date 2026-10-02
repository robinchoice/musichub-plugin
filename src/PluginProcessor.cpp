#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

namespace
{
// Shown on the approval page in the browser, so the user knows what asks
juce::String describeHost()
{
    const auto machine = juce::SystemStats::getComputerName();
    if (juce::JUCEApplicationBase::isStandaloneApp())
        return "Music Hub Plugin (Standalone) auf " + machine;
    return "Music Hub Plugin in " + juce::String (juce::PluginHostType().getHostDescription()) + " auf " + machine;
}
} // namespace

MusicHubAudioProcessor::MusicHubAudioProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "MusicHub", createParameterLayout()),
      session (describeHost())
{
    sourceParameter = parameters.getRawParameterValue (sourceParameterId);
    levelMatchParameter = parameters.getRawParameterValue (levelMatchParameterId);
    for (int slot = 1; slot < Engine::numSlots; ++slot)
    {
        offsetParameters[(size_t) slot] = parameters.getRawParameterValue (offsetParameterId (slot));
        trimParameters[(size_t) slot] = parameters.getRawParameterValue (trimParameterId (slot));
    }

    session.onSlotAudio = [this] (int slot, std::shared_ptr<LoadedAudio> audio)
    {
        engine.setSlotAudio (slot, std::move (audio));
    };

    startTimerHz (4);
}

MusicHubAudioProcessor::~MusicHubAudioProcessor()
{
    stopTimer();
    session.onSlotAudio = nullptr;
}

juce::AudioProcessorValueTreeState::ParameterLayout MusicHubAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { sourceParameterId, 1 }, "Quelle",
                                                        StringArray { "A (DAW)", "B", "C", "D" }, 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { levelMatchParameterId, 1 }, "Level-Match", true));

    for (int slot = 1; slot < Engine::numSlots; ++slot)
    {
        const auto name = String::charToString ((juce_wchar) ('A' + slot));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { offsetParameterId (slot), 1 }, "Offset " + name,
                                                           NormalisableRange<float> (-10000.0f, 10000.0f, 1.0f), 0.0f,
                                                           AudioParameterFloatAttributes().withLabel ("ms")));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { trimParameterId (slot), 1 }, "Trim " + name,
                                                           NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f,
                                                           AudioParameterFloatAttributes().withLabel ("dB")));
    }
    return layout;
}

void MusicHubAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);

    // Loaded versions belong to the previous rate; the session decodes them again
    juce::MessageManager::callAsync ([weak = juce::WeakReference<HubSession> (&session), sampleRate]
    {
        if (weak != nullptr)
            weak->setHostSampleRate (sampleRate);
    });
}

bool MusicHubAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& output = layouts.getMainOutputChannelSet();
    if (output != juce::AudioChannelSet::mono() && output != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == output;
}

void MusicHubAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    std::optional<int64_t> hostPosition;
    bool playing = false;
    if (auto* playHead = getPlayHead())
        if (const auto position = playHead->getPosition())
        {
            playing = position->getIsPlaying();
            if (const auto samples = position->getTimeInSamples())
                hostPosition = *samples;
        }

    for (int slot = 1; slot < Engine::numSlots; ++slot)
    {
        engine.slot (slot).offsetSeconds.store (offsetParameters[(size_t) slot]->load() / 1000.0);
        engine.slot (slot).trimDb.store (trimParameters[(size_t) slot]->load());
    }
    engine.setActiveSlot (juce::roundToInt (sourceParameter->load()));
    engine.process (buffer, hostPosition, playing, isNonRealtime());
    audibleSource.store (engine.getPlayingSlot());
}

// Message thread: level matching against the live mix, and freeing replaced audio
void MusicHubAudioProcessor::timerCallback()
{
    engine.clearReleasePool();

    const bool match = levelMatchParameter->load() > 0.5f;
    const double live = engine.liveMeter().integratedLufs();

    for (int slot = 1; slot < Engine::numSlots; ++slot)
    {
        float matchDb = 0.0f;
        if (match && std::isfinite (live))
            if (const auto audio = engine.getSlotAudio (slot); audio != nullptr && std::isfinite (audio->integratedLufs))
                matchDb = (float) juce::jlimit (-40.0, 20.0, live - audio->integratedLufs);
        engine.slot (slot).matchDb.store (matchDb);
    }
}

void MusicHubAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.appendChild (session.toState(), nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MusicHubAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    const auto hub = state.getChildWithName ("hub").createCopy();
    state.removeChild (state.getChildWithName ("hub"), nullptr);
    parameters.replaceState (state);

    if (hub.isValid())
        juce::MessageManager::callAsync ([weak = juce::WeakReference<HubSession> (&session), hub]
        {
            if (weak != nullptr)
                weak->restoreState (hub);
        });
}

juce::AudioProcessorEditor* MusicHubAudioProcessor::createEditor()
{
    return new MusicHubAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MusicHubAudioProcessor();
}
