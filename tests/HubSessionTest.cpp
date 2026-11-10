#include "HubSession.h"

#include "Check.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>

namespace
{
// A Music Hub API (or bucket) on localhost. Answers one connection at a time
// with what `respond` returns for the request line, e.g. "GET /api/v1/projects".
// Responses held back wait for `release`. Keeps every request it received.
class FakeHub : private juce::Thread
{
public:
    struct Response
    {
        int status = 200;
        juce::String body = "{}";
    };

    struct Request
    {
        juce::String line, headers;
        juce::MemoryBlock body;
    };

    using Respond = std::function<Response (const juce::String& request, juce::WaitableEvent& release)>;

    explicit FakeHub (Respond respondTo)
        : juce::Thread ("FakeHub"), respond (std::move (respondTo))
    {
        listener.createListener (0, "127.0.0.1");
        startThread();
    }

    ~FakeHub() override
    {
        release.signal();
        signalThreadShouldExit();
        listener.close();
        stopThread (5000);
    }

    juce::String url() const { return "http://127.0.0.1:" + juce::String (listener.getBoundPort()); }

    std::vector<Request> requests() const
    {
        const juce::ScopedLock lock (requestLock);
        return seen;
    }

    int count (const juce::String& line) const
    {
        const auto all = requests();
        return (int) std::count_if (all.begin(), all.end(), [&] (const auto& r) { return r.line == line; });
    }

    juce::WaitableEvent release { true };
    std::atomic<int> received { 0 }, answered { 0 };

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            if (listener.waitUntilReady (true, 50) != 1)
                continue;
            std::unique_ptr<juce::StreamingSocket> connection (listener.waitForNextConnection());
            if (connection == nullptr)
                continue;

            auto request = readRequest (*connection);
            request.line = request.line.upToLastOccurrenceOf (" HTTP/", false, false);
            {
                const juce::ScopedLock lock (requestLock);
                seen.push_back (request);
            }
            ++received;
            const auto response = respond (request.line, release);
            const auto body = response.body.toStdString();
            const auto head = "HTTP/1.1 " + juce::String (response.status) + " X\r\nContent-Type: application/json\r\n"
                              "Content-Length: " + juce::String ((int) body.size()) + "\r\nConnection: close\r\n\r\n";
            connection->write (head.toRawUTF8(), (int) head.getNumBytesAsUTF8());
            connection->write (body.data(), (int) body.size());
            ++answered;
        }
    }

    static Request readRequest (juce::StreamingSocket& socket)
    {
        juce::MemoryBlock data;
        char buffer[65536];
        for (;;)
        {
            const auto* bytes = static_cast<const char*> (data.getData());
            const auto end = std::search (bytes, bytes + data.getSize(), "\r\n\r\n", "\r\n\r\n" + 4);
            if (end != bytes + data.getSize())
            {
                const auto head = juce::String (bytes, (size_t) (end - bytes));
                const auto bodyStart = (size_t) (end - bytes) + 4;
                const int length = head.fromFirstOccurrenceOf ("Content-Length:", false, true).getIntValue();
                if (data.getSize() >= bodyStart + (size_t) length)
                    return { head.upToFirstOccurrenceOf ("\r\n", false, false),
                             head.fromFirstOccurrenceOf ("\r\n", false, false),
                             juce::MemoryBlock (bytes + bodyStart, (size_t) length) };
            }
            if (socket.waitUntilReady (true, 2000) != 1)
                return {};
            const int n = socket.read (buffer, sizeof (buffer), false);
            if (n <= 0)
                return {};
            data.append (buffer, (size_t) n);
        }
    }

    juce::StreamingSocket listener;
    Respond respond;
    juce::CriticalSection requestLock;
    std::vector<Request> seen;
};

struct TestDirectory
{
    TestDirectory() { directory.createDirectory(); }
    ~TestDirectory() { directory.deleteRecursively(); }

    // A stored login, as after an approved device login
    void storeLogin (const juce::String& baseUrl) const
    {
        directory.getChildFile ("credentials.json").replaceWithText (
            R"({"baseUrl":")" + baseUrl + R"(","token":"secret","user":{"id":"u1","email":"a@b.c","name":"A"}})");
    }

    juce::File directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getNonexistentChildFile ("MusicHubTest", "");
};

bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 5000)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (! done())
    {
        if (juce::Time::getMillisecondCounter() > end)
            return false;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    }
    return true;
}

constexpr auto projectList = R"({"projects":[{"project":{"id":"p1","name":"Album","artist":"A"}}]})";

// The project list was requested before logging out and arrives afterwards
void testAnswerAfterLogout (int status, const char* listName, const char* errorName)
{
    FakeHub hub ([status] (const juce::String& request, juce::WaitableEvent& release) -> FakeHub::Response
    {
        if (request == "GET /api/v1/projects")
        {
            release.wait (5000);
            return { status, status == 401 ? R"({"error":"unauthorized"})" : projectList };
        }
        return {};
    });
    TestDirectory dir;
    dir.storeLogin (hub.url());

    HubSession session ("test", hub.url(), dir.directory);
    check (session.getLogin() == HubSession::Login::loggedIn, "stored login is restored");
    pumpUntil ([&] { return hub.received.load() >= 1; });

    session.logout();
    hub.release.signal();
    pumpUntil ([&] { return hub.answered.load() >= 2; });   // projects and logout
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);

    check (session.getProjects().empty(), listName);
    check (session.getLastError().isEmpty(), errorName);
    check (session.getLogin() == HubSession::Login::loggedOut, "still logged out");
}

// Only a 401 ends the login, not an error text that happens to read "unauthorized"
void testRejectedRequest (int status, bool staysLoggedIn, const char* what)
{
    FakeHub hub ([status] (const juce::String& request, juce::WaitableEvent&) -> FakeHub::Response
    {
        if (request == "GET /api/v1/projects")
            return { status, R"({"error":"unauthorized"})" };
        return {};
    });
    TestDirectory dir;
    dir.storeLogin (hub.url());

    HubSession session ("test", hub.url(), dir.directory);
    pumpUntil ([&] { return session.getLastError().isNotEmpty(); });
    check (session.getLastError().isNotEmpty(), "the rejection is reported");
    check ((session.getLogin() == HubSession::Login::loggedIn) == staysLoggedIn, what);
}

// Removing the plugin while the hub doesn't answer must neither hang nor
// leave jobs running on a destroyed session
void testDestroyWhileRequestHangs()
{
    FakeHub hub ([] (const juce::String&, juce::WaitableEvent& release) -> FakeHub::Response
    {
        release.wait (30000);
        return {};
    });
    TestDirectory dir;
    dir.storeLogin (hub.url());

    auto session = std::make_unique<HubSession> ("test", hub.url(), dir.directory);
    pumpUntil ([&] { return hub.received.load() >= 1; });

    const auto start = std::chrono::steady_clock::now();
    session.reset();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - start).count();
    std::printf ("     destroyed after %lld ms\n", (long long) ms);
    check (ms < 5000, "destroying the session aborts a hanging request");
}
// Uploading a recorded bounce: a hub with project p1 and track t1, and a
// bucket on a second port that takes the PUT
struct UploadSetup
{
    using Answer = std::function<FakeHub::Response (const juce::String& request, juce::WaitableEvent& release)>;

    // `answer` may override hub routes; it returns status 0 to fall through
    explicit UploadSetup (Answer hubAnswer = {}, Answer bucketAnswer = {})
        : bucket ([bucketAnswer] (const juce::String& request, juce::WaitableEvent& release) -> FakeHub::Response
          {
              if (bucketAnswer != nullptr)
                  if (const auto r = bucketAnswer (request, release); r.status != 0)
                      return r;
              return { 200, "" };
          }),
          hub ([hubAnswer, uploadUrl = bucket.url() + "/bucket/key.flac?X-Amz-Signature=abc%2F1"]
               (const juce::String& request, juce::WaitableEvent& release) -> FakeHub::Response
          {
              if (hubAnswer != nullptr)
                  if (const auto r = hubAnswer (request, release); r.status != 0)
                      return r;
              if (request == "GET /api/v1/projects")
                  return { 200, projectList };
              if (request == "GET /api/v1/tracks/project/p1")
                  return { 200, R"({"tracks":[{"id":"t1","name":"Song","versionCount":4}]})" };
              if (request == "GET /api/v1/versions/track/t1")
                  return { 200, R"({"versions":[]})" };
              if (request == "POST /api/v1/versions/track/t1/upload-url")
                  return { 200, R"({"uploadUrl":")" + uploadUrl + R"(","fileKey":"projects/p1/tracks/t1/versions/v/original/b.flac","versionId":"v"})" };
              if (request == "POST /api/v1/versions/track/t1")
                  return { 201, R"({"version":{"id":"v","versionNumber":5}})" };
              return { 404, R"({"error":"Not found"})" };
          })
    {
        dir.storeLogin (hub.url());
        session = std::make_unique<HubSession> ("test", hub.url(), dir.directory);
        session->selectProject ("p1");
        session->selectTrack ("t1");

        file = dir.directory.getChildFile ("captures").getChildFile ("Bounce.flac");
        file.getParentDirectory().createDirectory();
        juce::MemoryBlock data (300000);
        juce::Random random (42);
        random.fillBitsRandomly (data.getData(), data.getSize());
        file.replaceWithData (data.getData(), data.getSize());
        content = data;
    }

    bool settled() const { return session->getUpload().status != HubSession::UploadStatus::uploading; }

    FakeHub bucket, hub;
    TestDirectory dir;
    std::unique_ptr<HubSession> session;
    juce::File file;
    juce::MemoryBlock content;
};

void testUploadSucceeds()
{
    UploadSetup setup;
    auto& session = *setup.session;
    session.bounceLabel = "Mix 3";
    session.uploadBounce (setup.file, 221.0);
    check (session.getUpload().status == HubSession::UploadStatus::uploading, "the upload starts");
    pumpUntil ([&] { return setup.settled(); });

    const auto& upload = session.getUpload();
    check (upload.status == HubSession::UploadStatus::done && upload.versionNumber == 5, "the bounce becomes V5");
    check (! setup.file.exists(), "the uploaded capture is deleted");
    check (session.bounceLabel.isEmpty(), "the label belongs to that upload only");

    const auto puts = setup.bucket.requests();
    check (puts.size() == 1 && puts[0].line == "PUT /bucket/key.flac?X-Amz-Signature=abc%2F1", "the file goes to the presigned URL as issued");
    if (puts.size() == 1)
    {
        check (puts[0].body == setup.content, "the bucket receives the file byte for byte");
        check (puts[0].headers.containsIgnoreCase ("Content-Type: audio/flac"), "with the signed content type");
        check (! puts[0].headers.containsIgnoreCase ("Authorization"), "and without the hub token");
    }

    juce::var created;
    for (const auto& r : setup.hub.requests())
        if (r.line == "POST /api/v1/versions/track/t1")
            created = juce::JSON::parse (r.body.toString());
    check (created["fileKey"].toString() == "projects/p1/tracks/t1/versions/v/original/b.flac"
           && created["label"].toString() == "Mix 3"
           && created["mimeType"].toString() == "audio/flac"
           && (juce::int64) created["fileSize"] == (juce::int64) setup.content.getSize()
           && created["sha256"].toString() == juce::SHA256 (setup.content).toHexString(), "the version is registered with key, label and checksum");

    pumpUntil ([&] { return setup.hub.count ("GET /api/v1/versions/track/t1") >= 2; });
    check (setup.hub.count ("GET /api/v1/versions/track/t1") >= 2, "the version list is reloaded");
}

void testUploadRejected (int status, const char* body, const juce::String& expectedError, const char* what)
{
    UploadSetup setup ([status, body] (const juce::String& request, juce::WaitableEvent&) -> FakeHub::Response
    {
        if (request == "POST /api/v1/versions/track/t1/upload-url")
            return { status, body };
        return { 0, {} };
    });
    auto& session = *setup.session;
    session.uploadBounce (setup.file, 10.0);
    pumpUntil ([&] { return setup.settled(); });

    const auto& upload = session.getUpload();
    check (upload.status == HubSession::UploadStatus::failed && upload.error == expectedError, what);
    check (setup.file.existsAsFile() && session.isUploadPending(), "the capture stays for a retry");
    check (session.getLogin() == HubSession::Login::loggedIn, "the login stays");
    check (setup.bucket.requests().empty(), "nothing reaches the bucket");

    session.discardUpload();
    check (! setup.file.exists() && session.getUpload().status == HubSession::UploadStatus::none, "discarding deletes the capture");
}

void testUploadRetry()
{
    std::atomic<int> attempts { 0 };
    UploadSetup setup ([&attempts] (const juce::String& request, juce::WaitableEvent&) -> FakeHub::Response
    {
        if (request == "POST /api/v1/versions/track/t1" && ++attempts == 1)
            return { 500, R"({"error":"Interner Fehler"})" };
        return { 0, {} };
    });
    auto& session = *setup.session;
    session.uploadBounce (setup.file, 10.0);
    pumpUntil ([&] { return setup.settled(); });
    check (session.getUpload().status == HubSession::UploadStatus::failed && session.getUpload().error == "Interner Fehler",
           "a failed registration is reported");

    session.retryUpload();
    pumpUntil ([&] { return setup.settled(); });
    check (session.getUpload().status == HubSession::UploadStatus::done && ! setup.file.exists(), "the retry uploads it");
    check (setup.hub.count ("POST /api/v1/versions/track/t1/upload-url") == 2 && setup.bucket.requests().size() == 2,
           "the retry starts over with a fresh upload URL");
}

// Discarding while the bucket hangs, then removing the plugin
void testUploadCancelled()
{
    UploadSetup setup ({}, [] (const juce::String&, juce::WaitableEvent& release) -> FakeHub::Response
    {
        release.wait (30000);
        return { 200, "" };
    });
    auto& session = *setup.session;
    session.uploadBounce (setup.file, 10.0);
    pumpUntil ([&] { return setup.bucket.received.load() >= 1; });

    session.discardUpload();
    check (session.getUpload().status == HubSession::UploadStatus::none && ! setup.file.exists(), "discarding stops waiting for the upload");

    const auto start = std::chrono::steady_clock::now();
    setup.session.reset();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - start).count();
    check (ms < 5000, "destroying the session aborts a hanging upload");
    check (setup.hub.count ("POST /api/v1/versions/track/t1") == 0, "a cancelled upload registers no version");
}
} // namespace

void testHubSession()
{
   #ifdef SIGPIPE
    std::signal (SIGPIPE, SIG_IGN);   // the fake hub writes to connections the client aborted
   #endif
    const juce::ScopedJuceInitialiser_GUI juce;
    testAnswerAfterLogout (200, "project list from before logout is dropped", "no error after logout");
    testAnswerAfterLogout (401, "rejected request from before logout is dropped", "late 401 doesn't report an expired login");
    testRejectedRequest (401, false, "401 ends the login");
    testRejectedRequest (403, true, "403 with the text unauthorized keeps the login");
    testDestroyWhileRequestHangs();
    testUploadSucceeds();
    testUploadRejected (403, R"({"error":"Forbidden"})", "Keine Berechtigung", "403 reports a missing permission");
    testUploadRejected (413, R"({"error":"Speicher voll: 4,9 von 5 GB belegt"})", "Speicher voll: 4,9 von 5 GB belegt",
                        "a full storage reports the hub's text");
    testUploadRetry();
    testUploadCancelled();
}
