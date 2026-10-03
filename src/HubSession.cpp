#include "HubSession.h"

#include <algorithm>
#include <cmath>

#if JUCE_MAC || JUCE_LINUX
 #include <sys/stat.h>
#endif

namespace
{
constexpr auto defaultBaseUrl = "https://hub.pleasance.org";
constexpr auto credentialsFileName = "credentials.json";

juce::String baseUrlFromEnvironment()
{
    return juce::SystemStats::getEnvironmentVariable ("MUSICHUB_URL", defaultBaseUrl).trimCharactersAtEnd ("/");
}

juce::String text (const juce::var& object, const char* key)
{
    return object.getProperty (key, juce::var()).toString();
}

std::shared_ptr<LoadedAudio> decodeFile (juce::AudioFormatManager& formats, const juce::File& file, double targetRate,
                                         std::optional<double> knownLufs, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        error = juce::String::fromUTF8 ("Dateiformat wird nicht unterst\xC3\xBCtzt");
        return nullptr;
    }
    if (reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max())
    {
        error = "Datei ist leer oder zu lang";
        return nullptr;
    }

    const int numChannels = (int) juce::jlimit (1u, 2u, reader->numChannels);
    const int length = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> source (numChannels, length);
    if (! reader->read (&source, 0, length, 0, true, numChannels > 1))
    {
        error = "Datei konnte nicht gelesen werden";
        return nullptr;
    }

    auto loaded = std::make_shared<LoadedAudio>();
    loaded->sampleRate = targetRate;
    loaded->integratedLufs = knownLufs.value_or (LoudnessMeter::measure (source, reader->sampleRate));

    if (std::abs (reader->sampleRate - targetRate) < 0.5)
    {
        loaded->buffer = std::move (source);
        return loaded;
    }

    const double ratio = reader->sampleRate / targetRate;
    const int outLength = (int) std::ceil ((double) length / ratio);
    loaded->buffer.setSize (numChannels, outLength);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        juce::LagrangeInterpolator interpolator;
        interpolator.process (ratio, source.getReadPointer (ch), loaded->buffer.getWritePointer (ch), outLength, length, 0);
    }
    return loaded;
}
} // namespace

HubSession::HubSession (juce::String name)
    : client (std::make_shared<HubClient> (baseUrlFromEnvironment())), clientName (std::move (name))
{
    // Worker threads take weak references to post their results back. The
    // first one creates the shared state, so do that here before they can race.
    masterReference.getSharedPointer (this);

    formats.registerBasicFormats();
    for (auto& w : work)
        w.cancelled = std::make_shared<std::atomic<bool>> (false);

    readCredentials();
    if (login == Login::loggedIn)
        refreshProjects();
}

HubSession::~HubSession()
{
    stopTimer();
    for (auto& w : work)
        w.cancelled->store (true);
    pool.removeAllJobs (true, 10000);
    masterReference.clear();
}

void HubSession::run (std::function<void()> job)
{
    pool.addJob ([job = std::move (job)]
    {
        job();
        return juce::ThreadPoolJob::jobHasFinished;
    });
}

void HubSession::later (std::function<void()> fn)
{
    juce::MessageManager::callAsync ([weak = juce::WeakReference<HubSession> (this), fn = std::move (fn)]
    {
        if (weak != nullptr)
            fn();
    });
}

// Login

juce::File HubSession::dataDirectory()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("Music Hub");
}

void HubSession::readCredentials()
{
    const auto json = juce::JSON::parse (dataDirectory().getChildFile (credentialsFileName));
    if (text (json, "baseUrl") != client->getBaseUrl())
        return;

    const auto token = text (json, "token");
    if (token.isEmpty())
        return;

    client->setToken (token);
    const auto stored = json["user"];
    user = { text (stored, "id"), text (stored, "email"), text (stored, "name") };
    login = Login::loggedIn;
}

void HubSession::writeCredentials() const
{
    const auto file = dataDirectory().getChildFile (credentialsFileName);
    file.getParentDirectory().createDirectory();

    auto* stored = new juce::DynamicObject();
    stored->setProperty ("id", user.id);
    stored->setProperty ("email", user.email);
    stored->setProperty ("name", user.name);

    auto* root = new juce::DynamicObject();
    root->setProperty ("baseUrl", client->getBaseUrl());
    root->setProperty ("token", client->getToken());
    root->setProperty ("user", juce::var (stored));

    file.replaceWithText (juce::JSON::toString (juce::var (root)));
   #if JUCE_MAC || JUCE_LINUX
    // The token is a login, so only this user may read it
    ::chmod (file.getFullPathName().toRawUTF8(), S_IRUSR | S_IWUSR);
   #endif
}

void HubSession::beginLogin()
{
    if (login != Login::loggedOut)
        return;

    login = Login::waitingForApproval;
    lastError.clear();
    deviceLogin = {};
    sendChangeMessage();

    run ([this, client = client, name = clientName]
    {
        DeviceLogin result;
        const auto outcome = client->startDeviceLogin (name, result);
        later ([this, outcome, result]
        {
            if (login != Login::waitingForApproval)
                return;

            if (outcome.failed())
            {
                login = Login::loggedOut;
                lastError = outcome.getErrorMessage();
            }
            else
            {
                deviceLogin = result;
                startTimer (result.pollIntervalSeconds * 1000);
            }
            sendChangeMessage();
        });
    });
}

void HubSession::timerCallback()
{
    if (pollInFlight || login != Login::waitingForApproval)
        return;

    pollInFlight = true;
    run ([this, client = client, deviceCode = deviceLogin.deviceCode]
    {
        juce::String token, error;
        HubUser newUser;
        const auto outcome = client->pollDeviceLogin (deviceCode, token, newUser, error);
        later ([this, outcome, token, newUser, error]
        {
            pollInFlight = false;
            if (login != Login::waitingForApproval)
                return;

            switch (outcome)
            {
                case HubClient::Poll::pending:
                    break;
                case HubClient::Poll::approved:
                    stopTimer();
                    finishLogin (token, newUser);
                    break;
                case HubClient::Poll::expired:
                    stopTimer();
                    login = Login::loggedOut;
                    lastError = "Der Code ist abgelaufen. Bitte neu verbinden.";
                    sendChangeMessage();
                    break;
                case HubClient::Poll::failed:
                    lastError = error;
                    sendChangeMessage();
                    break;
            }
        });
    });
}

void HubSession::cancelLogin()
{
    if (login != Login::waitingForApproval)
        return;

    stopTimer();
    login = Login::loggedOut;
    deviceLogin = {};
    sendChangeMessage();
}

void HubSession::finishLogin (const juce::String& token, const HubUser& newUser)
{
    client->setToken (token);
    user = newUser;
    login = Login::loggedIn;
    lastError.clear();
    deviceLogin = {};
    writeCredentials();
    sendChangeMessage();
    refreshProjects();
}

void HubSession::logout()
{
    if (login == Login::loggedIn)
        run ([client = client, token = client->getToken()] { client->logout (token); });

    forgetSession ({});
}

void HubSession::forgetSession (const juce::String& message)
{
    stopTimer();
    client->setToken ({});
    dataDirectory().getChildFile (credentialsFileName).deleteFile();

    login = Login::loggedOut;
    user = {};
    deviceLogin = {};
    projects.clear();
    tracks.clear();
    versions.clear();
    for (int slot = 1; slot <= numVersionSlots; ++slot)
        clearSlot (slot);

    lastError = message;
    sendChangeMessage();
}

bool HubSession::failed (const juce::Result& result)
{
    if (result.wasOk())
        return false;

    if (result.getErrorMessage() == HubClient::unauthorized)
        forgetSession ("Die Anmeldung ist abgelaufen. Bitte neu verbinden.");
    else
    {
        lastError = result.getErrorMessage();
        sendChangeMessage();
    }
    return true;
}

// Browsing

void HubSession::refreshProjects()
{
    if (login != Login::loggedIn)
        return;

    run ([this, client = client]
    {
        std::vector<HubProject> result;
        const auto outcome = client->fetchProjects (result);
        later ([this, outcome, result]
        {
            if (failed (outcome))
                return;

            projects = result;
            lastError.clear();
            const bool known = std::any_of (projects.begin(), projects.end(), [this] (const auto& p) { return p.id == projectId; });
            if (! known)
            {
                projectId.clear();
                trackId.clear();
                tracks.clear();
                versions.clear();
            }
            sendChangeMessage();
            if (known)
                fetchTracks();
        });
    });
}

void HubSession::selectProject (const juce::String& id)
{
    if (id == projectId)
        return;

    projectId = id;
    trackId.clear();
    tracks.clear();
    versions.clear();
    for (int slot = 1; slot <= numVersionSlots; ++slot)
        clearSlot (slot);
    sendChangeMessage();

    if (projectId.isNotEmpty())
        fetchTracks();
}

void HubSession::fetchTracks()
{
    run ([this, client = client, id = projectId]
    {
        std::vector<HubTrack> result;
        const auto outcome = client->fetchTracks (id, result);
        later ([this, outcome, result, id]
        {
            if (id != projectId || failed (outcome))
                return;

            tracks = result;
            const bool known = std::any_of (tracks.begin(), tracks.end(), [this] (const auto& t) { return t.id == trackId; });
            if (! known)
            {
                trackId.clear();
                versions.clear();
            }
            sendChangeMessage();
            if (known)
                fetchVersions();
        });
    });
}

void HubSession::selectTrack (const juce::String& id)
{
    if (id == trackId)
        return;

    trackId = id;
    versions.clear();
    for (int slot = 1; slot <= numVersionSlots; ++slot)
        clearSlot (slot);
    sendChangeMessage();

    if (trackId.isNotEmpty())
        fetchVersions();
}

void HubSession::fetchVersions()
{
    run ([this, client = client, id = trackId]
    {
        std::vector<HubVersion> result;
        const auto outcome = client->fetchVersions (id, result);
        later ([this, outcome, result, id]
        {
            if (id != trackId || failed (outcome))
                return;

            versions = result;
            sendChangeMessage();
            applyPendingSlots();
        });
    });
}

void HubSession::applyPendingSlots()
{
    for (int slot = 1; slot <= numVersionSlots; ++slot)
    {
        auto& w = work[(size_t) (slot - 1)];
        if (w.pendingVersionId.isNotEmpty())
            assignVersion (slot, std::exchange (w.pendingVersionId, {}));
    }
}

// Slots

juce::File HubSession::cacheFileFor (const HubVersion& version) const
{
    return dataDirectory().getChildFile ("cache").getChildFile (version.id)
        .getChildFile (juce::File::createLegalFileName (version.originalFileName));
}

void HubSession::dropSlotAudio (int slotIndex)
{
    if (onSlotAudio != nullptr)
        onSlotAudio (slotIndex, nullptr);
}

void HubSession::assignVersion (int slotIndex, const juce::String& versionId)
{
    if (versionId.isEmpty())
    {
        clearSlot (slotIndex);
        return;
    }

    auto& slot = slots[(size_t) (slotIndex - 1)];
    auto& w = work[(size_t) (slotIndex - 1)];
    if (slot.versionId == versionId && slot.status != SlotStatus::failed)
        return;

    const auto found = std::find_if (versions.begin(), versions.end(), [&] (const auto& v) { return v.id == versionId; });
    if (found == versions.end())
    {
        // The version list isn't there yet (restored session); pick it up when it arrives
        w.pendingVersionId = versionId;
        slot = {};
        slot.versionId = versionId;
        slot.status = SlotStatus::pending;
        sendChangeMessage();
        return;
    }
    const HubVersion version = *found;

    w.cancelled->store (true);
    w.cancelled = std::make_shared<std::atomic<bool>> (false);
    const int generation = ++w.generation;
    w.file = cacheFileFor (version);
    w.knownLufs = version.integratedLufs;
    w.pendingVersionId.clear();

    slot = {};
    slot.versionId = version.id;
    slot.name = version.displayName();
    slot.status = SlotStatus::downloading;
    dropSlotAudio (slotIndex);
    sendChangeMessage();

    if (w.file.existsAsFile())
    {
        decodeSlot (slotIndex);
        return;
    }

    run ([this, client = client, version, file = w.file, generation, slotIndex, cancelled = w.cancelled]
    {
        juce::String url;
        auto outcome = client->fetchDownloadUrl (version.id, url);
        if (outcome.wasOk())
        {
            int lastPercent = -1;
            outcome = client->downloadFile (url, file, [this, slotIndex, generation, &lastPercent] (double progress)
            {
                const int percent = (int) (progress * 100.0);
                if (percent == lastPercent)
                    return;
                lastPercent = percent;
                later ([this, slotIndex, generation, progress]
                {
                    if (work[(size_t) (slotIndex - 1)].generation != generation)
                        return;
                    slots[(size_t) (slotIndex - 1)].progress = progress;
                    sendChangeMessage();
                });
            }, *cancelled);
        }

        later ([this, outcome, slotIndex, generation]
        {
            if (work[(size_t) (slotIndex - 1)].generation != generation)
                return;

            if (outcome.wasOk())
            {
                decodeSlot (slotIndex);
                return;
            }
            if (outcome.getErrorMessage() == HubClient::unauthorized)
            {
                forgetSession ("Die Anmeldung ist abgelaufen. Bitte neu verbinden.");
                return;
            }
            auto& slot = slots[(size_t) (slotIndex - 1)];
            slot.status = SlotStatus::failed;
            slot.error = outcome.getErrorMessage();
            sendChangeMessage();
        });
    });
}

void HubSession::decodeSlot (int slotIndex)
{
    auto& slot = slots[(size_t) (slotIndex - 1)];
    auto& w = work[(size_t) (slotIndex - 1)];

    slot.status = SlotStatus::decoding;
    slot.progress = 1.0;
    sendChangeMessage();

    const double rate = hostSampleRate.load();
    if (rate <= 0)
        return;   // decoded once the host has told us its sample rate

    const int generation = ++w.generation;
    run ([this, file = w.file, knownLufs = w.knownLufs, rate, generation, slotIndex]
    {
        juce::String error;
        auto audio = decodeFile (formats, file, rate, knownLufs, error);
        later ([this, audio, error, generation, slotIndex]
        {
            if (work[(size_t) (slotIndex - 1)].generation != generation)
                return;

            auto& slot = slots[(size_t) (slotIndex - 1)];
            if (audio == nullptr)
            {
                slot.status = SlotStatus::failed;
                slot.error = error;
            }
            else
            {
                slot.status = SlotStatus::ready;
                slot.integratedLufs = audio->integratedLufs;
                if (onSlotAudio != nullptr)
                    onSlotAudio (slotIndex, audio);
            }
            sendChangeMessage();
        });
    });
}

void HubSession::clearSlot (int slotIndex)
{
    auto& w = work[(size_t) (slotIndex - 1)];
    w.cancelled->store (true);
    w.cancelled = std::make_shared<std::atomic<bool>> (false);
    ++w.generation;
    w.file = juce::File();
    w.knownLufs.reset();
    w.pendingVersionId.clear();

    slots[(size_t) (slotIndex - 1)] = {};
    dropSlotAudio (slotIndex);
    sendChangeMessage();
}

void HubSession::setHostSampleRate (double sampleRate)
{
    if (std::abs (hostSampleRate.load() - sampleRate) < 0.5)
        return;

    hostSampleRate.store (sampleRate);
    for (int slot = 1; slot <= numVersionSlots; ++slot)
    {
        const auto status = slots[(size_t) (slot - 1)].status;
        if ((status == SlotStatus::ready || status == SlotStatus::decoding) && work[(size_t) (slot - 1)].file.existsAsFile())
        {
            dropSlotAudio (slot);
            decodeSlot (slot);
        }
    }
}

// State saved with the DAW session

juce::ValueTree HubSession::toState() const
{
    juce::ValueTree state ("hub");
    state.setProperty ("project", projectId, nullptr);
    state.setProperty ("track", trackId, nullptr);
    for (int slot = 1; slot <= numVersionSlots; ++slot)
    {
        const auto& current = slots[(size_t) (slot - 1)];
        const auto& w = work[(size_t) (slot - 1)];
        state.setProperty ("slot" + juce::String (slot),
                           current.versionId.isNotEmpty() ? current.versionId : w.pendingVersionId, nullptr);
    }
    return state;
}

void HubSession::restoreState (const juce::ValueTree& state)
{
    if (! state.hasType ("hub"))
        return;

    projectId = state.getProperty ("project").toString();
    trackId = state.getProperty ("track").toString();
    for (int slot = 1; slot <= numVersionSlots; ++slot)
    {
        clearSlot (slot);
        work[(size_t) (slot - 1)].pendingVersionId = state.getProperty ("slot" + juce::String (slot)).toString();
    }
    sendChangeMessage();
    refreshProjects();
}
