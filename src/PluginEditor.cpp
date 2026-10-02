#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

namespace
{
juce::String de (const char* utf8) { return juce::String::fromUTF8 (utf8); }

juce::String slotName (int slot) { return juce::String::charToString ((juce::juce_wchar) ('A' + slot)); }

juce::String lufsText (double lufs)
{
    return std::isfinite (lufs) ? juce::String (lufs, 1) + " LUFS" : juce::String ("-- LUFS");
}

juce::Font font (float height, bool bold = false)
{
    auto options = juce::FontOptions (height);
    return juce::Font (bold ? options.withStyle ("Bold") : options);
}

void selectItem (juce::ComboBox& box, const std::vector<juce::String>& ids, const juce::String& id, int firstItemId)
{
    const auto found = std::find (ids.begin(), ids.end(), id);
    box.setSelectedId (found == ids.end() ? 0 : firstItemId + (int) std::distance (ids.begin(), found), juce::dontSendNotification);
}
} // namespace

MusicHubAudioProcessorEditor::MusicHubAudioProcessorEditor (MusicHubAudioProcessor& p)
    : AudioProcessorEditor (p), processor (p), session (p.session)
{
    titleLabel.setText ("Music Hub", juce::dontSendNotification);
    titleLabel.setFont (font (22.0f, true));
    addAndMakeVisible (titleLabel);

    userLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (userLabel);

    loginButton.onClick = [this]
    {
        switch (session.getLogin())
        {
            case HubSession::Login::loggedOut:          session.beginLogin(); break;
            case HubSession::Login::waitingForApproval: session.cancelLogin(); break;
            case HubSession::Login::loggedIn:           session.logout(); break;
        }
    };
    addAndMakeVisible (loginButton);

    codeLabel.setFont (font (30.0f, true));
    codeLabel.setJustificationType (juce::Justification::centred);
    addChildComponent (codeLabel);

    hintLabel.setJustificationType (juce::Justification::centred);
    hintLabel.setFont (font (13.0f));
    addChildComponent (hintLabel);

    browserButton.setButtonText (de ("Im Browser best\xC3\xA4tigen"));
    browserButton.onClick = [this] { juce::URL (session.getDeviceLogin().verificationUrl).launchInDefaultBrowser(); };
    addChildComponent (browserButton);

    projectBox.setTextWhenNothingSelected ("Projekt");
    projectBox.onChange = [this]
    {
        const int index = projectBox.getSelectedId() - 1;
        if (index >= 0 && index < (int) projectIds.size())
            session.selectProject (projectIds[(size_t) index]);
    };
    addAndMakeVisible (projectBox);

    trackBox.setTextWhenNothingSelected ("Track");
    trackBox.onChange = [this]
    {
        const int index = trackBox.getSelectedId() - 1;
        if (index >= 0 && index < (int) trackIds.size())
            session.selectTrack (trackIds[(size_t) index]);
    };
    addAndMakeVisible (trackBox);

    refreshButton.setButtonText ("Aktualisieren");
    refreshButton.onClick = [this] { session.refreshProjects(); };
    addAndMakeVisible (refreshButton);

    for (int i = 0; i < Engine::numSlots; ++i)
    {
        auto& button = sourceButtons[(size_t) i];
        button.setButtonText (i == 0 ? "A  (DAW)" : slotName (i));
        button.setClickingTogglesState (false);
        button.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xfff43f5e));
        button.onClick = [this, i] { sourceAttachment->setValueAsCompleteGesture ((float) i); };
        addAndMakeVisible (button);
    }
    sourceAttachment = std::make_unique<juce::ParameterAttachment> (
        *processor.parameters.getParameter (MusicHubAudioProcessor::sourceParameterId),
        [this] (float value)
        {
            const int chosen = juce::roundToInt (value);
            for (int i = 0; i < Engine::numSlots; ++i)
                sourceButtons[(size_t) i].setToggleState (i == chosen, juce::dontSendNotification);
        });
    sourceAttachment->sendInitialUpdate();

    for (int slot = 1; slot < Engine::numSlots; ++slot)
    {
        auto& row = slotRows[(size_t) (slot - 1)];

        row.title.setText (slotName (slot), juce::dontSendNotification);
        row.title.setFont (font (18.0f, true));
        addAndMakeVisible (row.title);

        row.versions.setTextWhenNothingSelected (de ("Version w\xC3\xA4hlen"));
        row.versions.onChange = [this, slot]
        {
            const int id = slotRows[(size_t) (slot - 1)].versions.getSelectedId();
            if (id == 1)
                session.clearSlot (slot);
            else if (id >= 2 && id - 2 < (int) versionIds.size())
                session.assignVersion (slot, versionIds[(size_t) (id - 2)]);
        };
        addAndMakeVisible (row.versions);

        row.status.setFont (font (13.0f));
        row.status.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (row.status);

        row.offset.setSliderStyle (juce::Slider::LinearHorizontal);
        row.offset.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 20);
        row.offset.setTextValueSuffix (" ms");
        row.offsetAttachment = std::make_unique<juce::SliderParameterAttachment> (
            *processor.parameters.getParameter (MusicHubAudioProcessor::offsetParameterId (slot)), row.offset);
        addAndMakeVisible (row.offset);

        row.trim.setSliderStyle (juce::Slider::LinearHorizontal);
        row.trim.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 20);
        row.trim.setTextValueSuffix (" dB");
        row.trimAttachment = std::make_unique<juce::SliderParameterAttachment> (
            *processor.parameters.getParameter (MusicHubAudioProcessor::trimParameterId (slot)), row.trim);
        addAndMakeVisible (row.trim);
    }

    levelMatchButton.setButtonText ("Level-Match (LUFS)");
    levelMatchAttachment = std::make_unique<juce::ButtonParameterAttachment> (
        *processor.parameters.getParameter (MusicHubAudioProcessor::levelMatchParameterId), levelMatchButton);
    addAndMakeVisible (levelMatchButton);

    liveLabel.setFont (font (13.0f));
    addAndMakeVisible (liveLabel);

    resetMeterButton.setButtonText (de ("Meter zur\xC3\xBC" "cksetzen"));
    resetMeterButton.onClick = [this] { processor.engine.liveMeter().reset(); };
    addAndMakeVisible (resetMeterButton);

    audibleLabel.setFont (font (13.0f));
    addAndMakeVisible (audibleLabel);

    errorLabel.setFont (font (13.0f));
    errorLabel.setColour (juce::Label::textColourId, juce::Colour (0xfffb923c));
    addAndMakeVisible (errorLabel);

    setSize (780, 640);
    session.addChangeListener (this);
    refresh();
    startTimerHz (10);
}

MusicHubAudioProcessorEditor::~MusicHubAudioProcessorEditor()
{
    session.removeChangeListener (this);
}

void MusicHubAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void MusicHubAudioProcessorEditor::refresh()
{
    const auto login = session.getLogin();
    const bool loggedIn = login == HubSession::Login::loggedIn;

    switch (login)
    {
        case HubSession::Login::loggedOut:
            loginButton.setButtonText ("Mit Music Hub verbinden");
            userLabel.setText ("Nicht verbunden", juce::dontSendNotification);
            break;
        case HubSession::Login::waitingForApproval:
            loginButton.setButtonText ("Abbrechen");
            userLabel.setText ("Warte auf Freigabe im Browser", juce::dontSendNotification);
            break;
        case HubSession::Login::loggedIn:
            loginButton.setButtonText ("Abmelden");
            userLabel.setText (session.getUser().name + "  @ " + juce::URL (session.getBaseUrl()).getDomain(), juce::dontSendNotification);
            break;
    }

    const auto& device = session.getDeviceLogin();
    const bool showCode = login == HubSession::Login::waitingForApproval && device.userCode.isNotEmpty();
    codeLabel.setVisible (showCode);
    hintLabel.setVisible (showCode);
    browserButton.setVisible (showCode);
    codeLabel.setText (device.userCode, juce::dontSendNotification);
    hintLabel.setText ("Diesen Code eingeben auf " + device.verificationUrl.upToFirstOccurrenceOf ("?", false, false), juce::dontSendNotification);

    projectIds.clear();
    projectBox.clear (juce::dontSendNotification);
    for (const auto& project : session.getProjects())
    {
        projectIds.push_back (project.id);
        projectBox.addItem (project.artist.isNotEmpty() ? project.artist + " - " + project.name : project.name, (int) projectIds.size());
    }
    selectItem (projectBox, projectIds, session.getProjectId(), 1);
    projectBox.setEnabled (loggedIn && ! projectIds.empty());

    trackIds.clear();
    trackBox.clear (juce::dontSendNotification);
    for (const auto& track : session.getTracks())
    {
        trackIds.push_back (track.id);
        trackBox.addItem (track.name + "  (" + juce::String (track.versionCount) + ")", (int) trackIds.size());
    }
    selectItem (trackBox, trackIds, session.getTrackId(), 1);
    trackBox.setEnabled (loggedIn && ! trackIds.empty());

    refreshButton.setEnabled (loggedIn);

    versionIds.clear();
    for (const auto& version : session.getVersions())
        versionIds.push_back (version.id);

    for (int slot = 1; slot < Engine::numSlots; ++slot)
    {
        auto& row = slotRows[(size_t) (slot - 1)];
        const auto& state = session.getSlot (slot);

        row.versions.clear (juce::dontSendNotification);
        row.versions.addItem ("(leer)", 1);
        int id = 2;
        for (const auto& version : session.getVersions())
            row.versions.addItem (version.displayName(), id++);
        if (state.versionId.isEmpty())
            row.versions.setSelectedId (1, juce::dontSendNotification);
        else
            selectItem (row.versions, versionIds, state.versionId, 2);
        row.versions.setEnabled (loggedIn && ! versionIds.empty());

        juce::String status;
        switch (state.status)
        {
            case HubSession::SlotStatus::empty:       break;
            case HubSession::SlotStatus::pending:     status = "Wartet auf Versionsliste"; break;
            case HubSession::SlotStatus::downloading: status = de ("L\xC3\xA4" "dt ") + juce::String ((int) (state.progress * 100.0)) + " %"; break;
            case HubSession::SlotStatus::decoding:    status = "Dekodiert..."; break;
            case HubSession::SlotStatus::ready:       status = lufsText (state.integratedLufs); break;
            case HubSession::SlotStatus::failed:      status = "Fehler: " + state.error; break;
        }
        row.status.setText (status, juce::dontSendNotification);
    }

    errorLabel.setText (session.getLastError(), juce::dontSendNotification);
    resized();
}

void MusicHubAudioProcessorEditor::timerCallback()
{
    const auto& meter = processor.engine.liveMeter();
    liveLabel.setText ("Live (A): " + lufsText (meter.integratedLufs()) + "   kurzzeitig " + lufsText (meter.shortTermLufs()),
                       juce::dontSendNotification);

    const int chosen = juce::roundToInt (processor.parameters.getRawParameterValue (MusicHubAudioProcessor::sourceParameterId)->load());
    const int audible = processor.getAudibleSource();
    juce::String text = de ("H\xC3\xB6rbar: ") + slotName (audible);
    if (audible != chosen)
    {
        const bool slotReady = session.getSlot (chosen).status == HubSession::SlotStatus::ready;
        text << (slotReady ? "  (Transport steht oder Offline-Render)" : "  (Slot " + slotName (chosen) + " ist noch leer)");
    }
    audibleLabel.setText (text, juce::dontSendNotification);
}

void MusicHubAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0a0910));
}

void MusicHubAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (16);

    auto header = area.removeFromTop (36);
    titleLabel.setBounds (header.removeFromLeft (160));
    loginButton.setBounds (header.removeFromRight (220));
    header.removeFromRight (12);
    userLabel.setBounds (header);
    area.removeFromTop (10);

    if (codeLabel.isVisible())
    {
        auto panel = area.removeFromTop (112);
        codeLabel.setBounds (panel.removeFromTop (44));
        hintLabel.setBounds (panel.removeFromTop (24));
        panel.removeFromTop (6);
        browserButton.setBounds (panel.removeFromTop (30).withSizeKeepingCentre (220, 30));
        area.removeFromTop (10);
    }

    auto pickers = area.removeFromTop (30);
    refreshButton.setBounds (pickers.removeFromRight (120));
    pickers.removeFromRight (8);
    projectBox.setBounds (pickers.removeFromLeft ((pickers.getWidth() - 8) / 2));
    pickers.removeFromLeft (8);
    trackBox.setBounds (pickers);
    area.removeFromTop (14);

    auto sources = area.removeFromTop (56);
    const int buttonWidth = (sources.getWidth() - 3 * 8) / 4;
    for (auto& button : sourceButtons)
    {
        button.setBounds (sources.removeFromLeft (buttonWidth));
        sources.removeFromLeft (8);
    }
    area.removeFromTop (14);

    for (auto& row : slotRows)
    {
        auto block = area.removeFromTop (72);
        auto top = block.removeFromTop (30);
        row.title.setBounds (top.removeFromLeft (28));
        top.removeFromLeft (8);
        row.status.setBounds (top.removeFromRight (240));
        top.removeFromRight (8);
        row.versions.setBounds (top);
        block.removeFromTop (4);
        auto sliders = block.removeFromTop (28);
        sliders.removeFromLeft (36);
        row.offset.setBounds (sliders.removeFromLeft ((sliders.getWidth() - 8) / 2));
        sliders.removeFromLeft (8);
        row.trim.setBounds (sliders);
        area.removeFromTop (8);
    }
    area.removeFromTop (6);

    auto bottom = area.removeFromTop (28);
    levelMatchButton.setBounds (bottom.removeFromLeft (190));
    resetMeterButton.setBounds (bottom.removeFromRight (170));
    bottom.removeFromRight (8);
    liveLabel.setBounds (bottom);
    area.removeFromTop (6);
    audibleLabel.setBounds (area.removeFromTop (22));
    errorLabel.setBounds (area.removeFromTop (22));
}
