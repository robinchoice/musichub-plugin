#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Engine.h"
#include "HubSession.h"

#include <array>
#include <atomic>

class MusicHubAudioProcessor : public juce::AudioProcessor,
                               private juce::Timer
{
public:
    MusicHubAudioProcessor();
    ~MusicHubAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static constexpr const char* sourceParameterId = "source";
    static constexpr const char* levelMatchParameterId = "levelMatch";
    static juce::String offsetParameterId (int slot) { return "offset" + juce::String (slot); }
    static juce::String trimParameterId (int slot) { return "trim" + juce::String (slot); }

    juce::AudioProcessorValueTreeState parameters;
    Engine engine;
    HubSession session;

    // The slot that is audible right now: the chosen one, or the live mix
    // while its slot is empty, the transport stands still or a bounce renders
    int getAudibleSource() const noexcept { return audibleSource.load(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void timerCallback() override;

    std::atomic<float>* sourceParameter = nullptr;
    std::atomic<float>* levelMatchParameter = nullptr;
    std::array<std::atomic<float>*, Engine::numSlots> offsetParameters {};
    std::array<std::atomic<float>*, Engine::numSlots> trimParameters {};
    std::atomic<int> audibleSource { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MusicHubAudioProcessor)
};
