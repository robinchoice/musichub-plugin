#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

#include <array>
#include <memory>
#include <vector>

class MusicHubAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     private juce::ChangeListener,
                                     private juce::Timer
{
public:
    explicit MusicHubAudioProcessorEditor (MusicHubAudioProcessor&);
    ~MusicHubAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct SlotRow
    {
        juce::Label title, status;
        juce::ComboBox versions;
        juce::Slider offset, trim;
        std::unique_ptr<juce::SliderParameterAttachment> offsetAttachment, trimAttachment;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refresh();

    MusicHubAudioProcessor& processor;
    HubSession& session;

    juce::Label titleLabel, userLabel, codeLabel, hintLabel, liveLabel, audibleLabel, errorLabel;
    juce::TextButton loginButton, browserButton, refreshButton, resetMeterButton;
    juce::ComboBox projectBox, trackBox;
    std::array<juce::TextButton, Engine::numSlots> sourceButtons;
    std::unique_ptr<juce::ParameterAttachment> sourceAttachment;
    juce::ToggleButton levelMatchButton;
    std::unique_ptr<juce::ButtonParameterAttachment> levelMatchAttachment;
    std::array<SlotRow, HubSession::numVersionSlots> slotRows;

    // Combo box item order to id
    std::vector<juce::String> projectIds, trackIds, versionIds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MusicHubAudioProcessorEditor)
};
