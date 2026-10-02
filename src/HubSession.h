#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include "Engine.h"
#include "HubClient.h"

#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

// What one plugin instance knows about Music Hub: the login, the chosen
// project and track, and the versions in the comparison slots. Public state
// is read and written on the message thread; requests and decoding run on
// worker threads.
class HubSession : public juce::ChangeBroadcaster,
                   private juce::Timer
{
public:
    static constexpr int numVersionSlots = Engine::numSlots - 1;   // engine slots 1..3

    enum class Login { loggedOut, waitingForApproval, loggedIn };
    enum class SlotStatus { empty, pending, downloading, decoding, ready, failed };

    struct Slot
    {
        juce::String versionId, name, error;
        SlotStatus status = SlotStatus::empty;
        double progress = 0;
        double integratedLufs = -std::numeric_limits<double>::infinity();
    };

    explicit HubSession (juce::String clientName);
    ~HubSession() override;

    Login getLogin() const noexcept { return login; }
    const HubUser& getUser() const noexcept { return user; }
    const DeviceLogin& getDeviceLogin() const noexcept { return deviceLogin; }
    const juce::String& getLastError() const noexcept { return lastError; }
    const juce::String& getBaseUrl() const noexcept { return client->getBaseUrl(); }

    void beginLogin();
    void cancelLogin();
    void logout();

    const std::vector<HubProject>& getProjects() const noexcept { return projects; }
    const std::vector<HubTrack>& getTracks() const noexcept { return tracks; }
    const std::vector<HubVersion>& getVersions() const noexcept { return versions; }
    const juce::String& getProjectId() const noexcept { return projectId; }
    const juce::String& getTrackId() const noexcept { return trackId; }

    void refreshProjects();
    void selectProject (const juce::String& id);
    void selectTrack (const juce::String& id);

    // Slot indices are engine slots, so 1..3
    const Slot& getSlot (int slotIndex) const { return slots[(size_t) (slotIndex - 1)]; }
    void assignVersion (int slotIndex, const juce::String& versionId);
    void clearSlot (int slotIndex);

    // Decoded audio for an engine slot, or nullptr when it empties. Message thread.
    std::function<void (int slotIndex, std::shared_ptr<LoadedAudio>)> onSlotAudio;

    // Versions are decoded at the host rate; a change decodes the loaded slots again
    void setHostSampleRate (double sampleRate);

    juce::ValueTree toState() const;
    void restoreState (const juce::ValueTree& state);

private:
    struct SlotWork
    {
        int generation = 0;
        std::shared_ptr<std::atomic<bool>> cancelled;
        juce::File file;
        std::optional<double> knownLufs;
        juce::String pendingVersionId;   // restored before the version list arrived
    };

    void timerCallback() override;
    void run (std::function<void()> job);
    void later (std::function<void()> fn);

    void readCredentials();
    void writeCredentials() const;
    void forgetSession (const juce::String& message);
    void finishLogin (const juce::String& token, const HubUser& newUser);
    bool failed (const juce::Result& result);

    void fetchTracks();
    void fetchVersions();
    void applyPendingSlots();
    void decodeSlot (int slotIndex);
    void dropSlotAudio (int slotIndex);
    juce::File cacheFileFor (const HubVersion& version) const;
    static juce::File dataDirectory();

    std::shared_ptr<HubClient> client;
    const juce::String clientName;
    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withNumberOfThreads (2) };
    juce::AudioFormatManager formats;

    Login login = Login::loggedOut;
    HubUser user;
    DeviceLogin deviceLogin;
    juce::String lastError;
    bool pollInFlight = false;

    std::vector<HubProject> projects;
    std::vector<HubTrack> tracks;
    std::vector<HubVersion> versions;
    juce::String projectId, trackId;

    std::array<Slot, numVersionSlots> slots;
    std::array<SlotWork, numVersionSlots> work;
    std::atomic<double> hostSampleRate { 0.0 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (HubSession)
};
