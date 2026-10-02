#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <optional>
#include <vector>

struct HubUser
{
    juce::String id, email, name;
};

struct HubProject
{
    juce::String id, name, artist;
};

struct HubTrack
{
    juce::String id, name;
    int versionCount = 0;
};

struct HubVersion
{
    juce::String id, label, branchLabel, status, originalFileName;
    int versionNumber = 0;
    double duration = 0;
    std::optional<double> integratedLufs;

    juce::String displayName() const;
};

struct DeviceLogin
{
    juce::String deviceCode, userCode, verificationUrl;
    int pollIntervalSeconds = 5;
};

// Blocking calls against the Music Hub REST API. Use from a worker thread.
class HubClient
{
public:
    explicit HubClient (juce::String baseUrl);

    const juce::String& getBaseUrl() const noexcept { return baseUrl; }
    void setToken (juce::String newToken);
    juce::String getToken() const;

    juce::Result startDeviceLogin (const juce::String& clientName, DeviceLogin& out);

    enum class Poll { pending, approved, expired, failed };
    Poll pollDeviceLogin (const juce::String& deviceCode, juce::String& tokenOut, HubUser& userOut, juce::String& errorOut);

    // These fail with `unauthorized` once the token is no longer valid
    juce::Result fetchMe (HubUser& out);
    juce::Result logout (const juce::String& token);
    juce::Result fetchProjects (std::vector<HubProject>& out);
    juce::Result fetchTracks (const juce::String& projectId, std::vector<HubTrack>& out);
    juce::Result fetchVersions (const juce::String& trackId, std::vector<HubVersion>& out);
    juce::Result fetchDownloadUrl (const juce::String& versionId, juce::String& urlOut);

    juce::Result downloadFile (const juce::String& url, const juce::File& destination,
                               const std::function<void (double)>& onProgress,
                               const std::atomic<bool>& cancelled);

    static constexpr const char* unauthorized = "unauthorized";

private:
    struct HttpResponse
    {
        int status = 0;
        juce::var json;
        juce::String error;
    };

    HttpResponse request (const juce::String& method, const juce::String& path, const juce::var& body = {},
                          std::optional<juce::String> bearer = std::nullopt);
    juce::Result check (const HttpResponse& response) const;

    juce::String baseUrl;
    juce::String token;
    mutable juce::CriticalSection tokenLock;
};
