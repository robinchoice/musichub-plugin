#include "HubSession.h"

#include "Check.h"

#include <atomic>
#include <chrono>
#include <csignal>

namespace
{
// A Music Hub API on localhost. Answers one connection at a time with what
// `respond` returns for the request line, e.g. "GET /api/v1/projects".
// Responses held back wait for `release`.
class FakeHub : private juce::Thread
{
public:
    struct Response
    {
        int status = 200;
        juce::String body = "{}";
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

            const auto requestLine = readRequest (*connection).upToLastOccurrenceOf (" HTTP/", false, false);
            ++received;
            const auto response = respond (requestLine, release);
            const auto body = response.body.toStdString();
            const auto head = "HTTP/1.1 " + juce::String (response.status) + " X\r\nContent-Type: application/json\r\n"
                              "Content-Length: " + juce::String ((int) body.size()) + "\r\nConnection: close\r\n\r\n";
            connection->write (head.toRawUTF8(), (int) head.getNumBytesAsUTF8());
            connection->write (body.data(), (int) body.size());
            ++answered;
        }
    }

    static juce::String readRequest (juce::StreamingSocket& socket)
    {
        juce::MemoryBlock data;
        char buffer[4096];
        for (;;)
        {
            const auto text = data.toString();
            const int headerEnd = text.indexOf ("\r\n\r\n");
            if (headerEnd >= 0)
            {
                const int length = text.fromFirstOccurrenceOf ("Content-Length:", false, true).getIntValue();
                if ((int) data.getSize() >= headerEnd + 4 + length)
                    return text.upToFirstOccurrenceOf ("\r\n", false, false);
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
} // namespace

void testHubSession()
{
   #ifdef SIGPIPE
    std::signal (SIGPIPE, SIG_IGN);   // the fake hub writes to connections the client aborted
   #endif
    const juce::ScopedJuceInitialiser_GUI juce;
    testAnswerAfterLogout (200, "project list from before logout is dropped", "no error after logout");
    testAnswerAfterLogout (401, "rejected request from before logout is dropped", "late 401 doesn't report an expired login");
    testDestroyWhileRequestHangs();
}
