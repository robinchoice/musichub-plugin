#include "HubClient.h"

namespace
{
juce::String str (const juce::var& object, const char* key)
{
    return object.getProperty (key, juce::var()).toString();
}

std::optional<double> number (const juce::var& object, const char* key)
{
    const auto value = object.getProperty (key, juce::var());
    if (value.isDouble() || value.isInt() || value.isInt64())
        return static_cast<double> (value);
    return std::nullopt;
}

juce::var objectWith (const char* key, const juce::String& value)
{
    auto* object = new juce::DynamicObject();
    object->setProperty (key, value);
    return juce::var (object);
}

HubUser parseUser (const juce::var& object)
{
    return { str (object, "id"), str (object, "email"), str (object, "name") };
}
} // namespace

juce::String HubVersion::displayName() const
{
    juce::String name = "V" + juce::String (versionNumber);
    if (label.isNotEmpty())
        name << " - " << label;
    if (branchLabel.isNotEmpty())
        name << " (" << branchLabel << ")";
    return name;
}

HubClient::HubClient (juce::String url) : baseUrl (std::move (url)) {}

void HubClient::setToken (juce::String newToken)
{
    const juce::ScopedLock lock (tokenLock);
    token = std::move (newToken);
}

juce::String HubClient::getToken() const
{
    const juce::ScopedLock lock (tokenLock);
    return token;
}

HubClient::HttpResponse HubClient::request (const juce::String& method, const juce::String& path, const juce::var& body,
                                            std::optional<juce::String> bearer)
{
    HttpResponse response;

    juce::URL url (baseUrl + "/api/v1" + path);
    juce::String headers ("Accept: application/json\r\n");

    if (! body.isVoid())
    {
        url = url.withPOSTData (juce::JSON::toString (body, true));
        headers << "Content-Type: application/json\r\n";
    }

    if (const auto token = bearer.value_or (getToken()); token.isNotEmpty())
        headers << "Authorization: Bearer " << token << "\r\n";

    const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                             .withExtraHeaders (headers)
                             .withHttpRequestCmd (method)
                             .withConnectionTimeoutMs (15000)
                             .withNumRedirectsToFollow (0)
                             .withStatusCode (&response.status);

    const auto stream = url.createInputStream (options);

    if (stream == nullptr)
    {
        response.error = "Keine Verbindung zu " + baseUrl;
        return response;
    }

    response.json = juce::JSON::parse (stream->readEntireStreamAsString());

    if (response.status >= 400)
    {
        response.error = str (response.json, "error");
        if (response.error.isEmpty())
            response.error = "HTTP " + juce::String (response.status);
    }

    return response;
}

juce::Result HubClient::check (const HttpResponse& response) const
{
    if (response.status == 401)
        return juce::Result::fail (unauthorized);
    if (response.error.isNotEmpty())
        return juce::Result::fail (response.error);
    if (response.status < 200 || response.status >= 300)
        return juce::Result::fail ("HTTP " + juce::String (response.status));
    return juce::Result::ok();
}

juce::Result HubClient::startDeviceLogin (const juce::String& clientName, DeviceLogin& out)
{
    const auto response = request ("POST", "/auth/device", objectWith ("client", clientName));
    if (const auto result = check (response); result.failed())
        return result;

    out.deviceCode = str (response.json, "deviceCode");
    out.userCode = str (response.json, "userCode");
    out.verificationUrl = str (response.json, "verificationUrl");
    out.pollIntervalSeconds = juce::jmax (1, static_cast<int> (response.json.getProperty ("pollIntervalSeconds", 5)));
    return juce::Result::ok();
}

HubClient::Poll HubClient::pollDeviceLogin (const juce::String& deviceCode, juce::String& tokenOut, HubUser& userOut, juce::String& errorOut)
{
    const auto response = request ("POST", "/auth/device/token", objectWith ("deviceCode", deviceCode));

    if (response.status == 202)
        return Poll::pending;

    if (response.status == 200)
    {
        tokenOut = str (response.json, "token");
        userOut = parseUser (response.json["user"]);
        return Poll::approved;
    }

    errorOut = response.error;
    return response.status == 400 ? Poll::expired : Poll::failed;
}

juce::Result HubClient::fetchMe (HubUser& out)
{
    const auto response = request ("GET", "/auth/me");
    if (const auto result = check (response); result.failed())
        return result;

    const auto user = response.json["user"];
    if (! user.isObject())
        return juce::Result::fail (unauthorized);

    out = parseUser (user);
    return juce::Result::ok();
}

juce::Result HubClient::logout (const juce::String& token)
{
    return check (request ("POST", "/auth/logout", {}, token));
}

juce::Result HubClient::fetchProjects (std::vector<HubProject>& out)
{
    const auto response = request ("GET", "/projects");
    if (const auto result = check (response); result.failed())
        return result;

    out.clear();
    if (const auto* items = response.json["projects"].getArray())
        for (const auto& item : *items)
        {
            const auto project = item["project"];
            out.push_back ({ str (project, "id"), str (project, "name"), str (project, "artist") });
        }

    return juce::Result::ok();
}

juce::Result HubClient::fetchTracks (const juce::String& projectId, std::vector<HubTrack>& out)
{
    const auto response = request ("GET", "/tracks/project/" + projectId);
    if (const auto result = check (response); result.failed())
        return result;

    out.clear();
    if (const auto* items = response.json["tracks"].getArray())
        for (const auto& item : *items)
            out.push_back ({ str (item, "id"), str (item, "name"), static_cast<int> (item.getProperty ("versionCount", 0)) });

    return juce::Result::ok();
}

juce::Result HubClient::fetchVersions (const juce::String& trackId, std::vector<HubVersion>& out)
{
    const auto response = request ("GET", "/versions/track/" + trackId);
    if (const auto result = check (response); result.failed())
        return result;

    out.clear();
    if (const auto* items = response.json["versions"].getArray())
        for (const auto& item : *items)
        {
            HubVersion version;
            version.id = str (item, "id");
            version.label = str (item, "label");
            version.branchLabel = str (item, "branchLabel");
            version.status = str (item, "status");
            version.originalFileName = str (item, "originalFileName");
            version.versionNumber = static_cast<int> (item.getProperty ("versionNumber", 0));
            version.duration = number (item, "duration").value_or (0.0);
            version.integratedLufs = number (item, "integratedLufs");
            out.push_back (std::move (version));
        }

    return juce::Result::ok();
}

juce::Result HubClient::fetchDownloadUrl (const juce::String& versionId, juce::String& urlOut)
{
    const auto response = request ("GET", "/versions/" + versionId + "/download-url");
    if (const auto result = check (response); result.failed())
        return result;

    urlOut = str (response.json, "url");
    return urlOut.isNotEmpty() ? juce::Result::ok() : juce::Result::fail ("Keine Download-URL erhalten");
}

juce::Result HubClient::downloadFile (const juce::String& urlString, const juce::File& destination,
                                      const std::function<void (double)>& onProgress,
                                      const std::atomic<bool>& cancelled)
{
    int status = 0;
    // Presigned S3 URLs must stay byte for byte as issued, so no re-encoding of the query
    const auto stream = juce::URL::createWithoutParsing (urlString).createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs (20000)
            .withStatusCode (&status));

    if (stream == nullptr || status >= 400)
        return juce::Result::fail ("Download fehlgeschlagen (HTTP " + juce::String (status) + ")");

    destination.getParentDirectory().createDirectory();
    const auto partFile = destination.getSiblingFile (destination.getFileName() + ".part");
    partFile.deleteFile();

    bool aborted = false;
    {
        juce::FileOutputStream out (partFile);
        if (! out.openedOk())
            return juce::Result::fail ("Cache-Datei kann nicht geschrieben werden");

        const auto total = stream->getTotalLength();
        constexpr int chunkSize = 1 << 16;
        juce::HeapBlock<char> chunk (chunkSize);
        juce::int64 done = 0;

        while (! stream->isExhausted())
        {
            if (cancelled.load())
            {
                aborted = true;
                break;
            }

            const auto n = stream->read (chunk, chunkSize);
            if (n <= 0)
                break;

            out.write (chunk, static_cast<size_t> (n));
            done += n;

            if (total > 0 && onProgress != nullptr)
                onProgress (static_cast<double> (done) / static_cast<double> (total));
        }

        out.flush();
    }

    if (aborted)
    {
        partFile.deleteFile();
        return juce::Result::fail ("Abgebrochen");
    }

    if (! partFile.moveFileTo (destination))
        return juce::Result::fail ("Cache-Datei kann nicht umbenannt werden");

    return juce::Result::ok();
}
