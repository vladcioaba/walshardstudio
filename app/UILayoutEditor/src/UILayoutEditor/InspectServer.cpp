#include "UILayoutEditor/InspectServer.h"

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX  // keep <windows.h> min/max macros off std::min/std::max
#    define NOMINMAX
#endif
#    include <winsock2.h>
#    include <ws2tcpip.h>
// Link the Winsock library implicitly so callers don't have to remember.
#    pragma comment(lib, "ws2_32.lib")
using ssize_t = SSIZE_T;
#    define VS_CLOSE_SOCK(fd) ::closesocket(fd)
#    define VS_SHUT_RDWR      SD_BOTH
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <sys/types.h>
#    include <unistd.h>
#    define VS_CLOSE_SOCK(fd) ::close(fd)
#    define VS_SHUT_RDWR      SHUT_RDWR
#endif

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <future>

namespace opencs
{

struct InspectServer::Cmd
{
    std::string                                   line;
    // shared_ptr so the promise can outlive the Cmd: a deferred
    // handler steals it via `InspectReply::defer()` into a
    // `PendingReply` that lives in editor-side state across frames.
    std::shared_ptr<std::promise<std::string>>    reply;
    // Points at the per-connection auth flag living on the worker
    // thread's stack. Safe: the worker blocks on `reply`'s future
    // while drain() runs the handler, so the flag outlives the call
    // and is never touched concurrently.
    bool*                                         authed = nullptr;
};

InspectServer::~InspectServer() { stop(); }

bool InspectServer::start(int port)
{
    if (_running.load()) return true;

#if defined(_WIN32)
    // One-shot Winsock init. WSAStartup is refcounted; pair with
    // WSACleanup in stop().
    WSADATA wsaData;
    if (::WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::perror("[inspect] WSAStartup");
        return false;
    }
#endif

    _listenFd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (_listenFd < 0) { std::perror("[inspect] socket"); return false; }

    int yes = 1;
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::perror("[inspect] bind");
        VS_CLOSE_SOCK(_listenFd); _listenFd = -1; return false;
    }
    if (::listen(_listenFd, 4) < 0)
    {
        std::perror("[inspect] listen");
        VS_CLOSE_SOCK(_listenFd); _listenFd = -1; return false;
    }

#if !defined(_WIN32)
    // Ignore SIGPIPE — if the peer hangs up while we're mid-write,
    // the write should return EPIPE instead of killing the process.
    // Windows has no SIGPIPE; Winsock returns WSAECONNRESET instead,
    // which we already tolerate via the error-return check in writeAll.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    _port = port;
    _running = true;
    _thread = std::thread(&InspectServer::runAccept, this);
    std::fprintf(stderr, "[inspect] listening on 127.0.0.1:%d\n", port);
    return true;
}

void InspectServer::stop()
{
    if (!_running.exchange(false)) return;
    if (_listenFd >= 0) { ::shutdown(_listenFd, VS_SHUT_RDWR); VS_CLOSE_SOCK(_listenFd); _listenFd = -1; }
    if (_thread.joinable()) _thread.join();

    // Fail any in-flight commands first so no worker stays parked on a
    // future the main thread will never fulfil (stop() usually runs on
    // the main thread, which is the only one that drains).
    {
        std::lock_guard<std::mutex> lk(_mu);
        while (!_queue.empty())
        {
            auto& c = _queue.front();
            if (c->reply)
            { try { c->reply->set_value("ERR: server stopped\n"); } catch (...) {} }
            _queue.pop_front();
        }
    }
    {
        std::lock_guard<std::mutex> lk(_subMu);
        _subscribers.clear();
    }

    // Wake every worker (subscriber or not) by shutting its socket down;
    // its recv returns EOF and the conn loop exits. Workers waiting on a
    // deferred reply notice `_running == false` within one poll tick.
    // Then wait until all of them have released `this`.
    {
        std::unique_lock<std::mutex> lk(_connsMu);
        for (int fd : _connFds) ::shutdown(fd, VS_SHUT_RDWR);
        _connsCv.wait(lk, [this] { return _activeConns == 0; });
    }

#if defined(_WIN32)
    // Pair the WSAStartup from start(). Refcounted, so multiple
    // start/stop cycles are safe.
    ::WSACleanup();
#endif
}

/// Read one '\n'-terminated line (max 4KB). Returns empty on EOF / error.
static std::string readLine(int fd)
{
    std::string out;
    out.reserve(128);
    char ch = 0;
    while (out.size() < 4096)
    {
        ssize_t n = ::recv(fd, &ch, 1, 0);
        if (n <= 0) return {};
        if (ch == '\n') break;
        if (ch != '\r') out.push_back(ch);
    }
    return out;
}

static bool writeAll(int fd, const std::string& s)
{
    const char* p = s.data();
    size_t left = s.size();
    while (left > 0)
    {
        ssize_t n = ::send(fd, p, left, 0);
        if (n <= 0) return false;
        p += n; left -= static_cast<size_t>(n);
    }
    return true;
}

/// True when `line` is an HTTP request line ("POST / HTTP/1.1") or an
/// HTTP header. A browser can POST to 127.0.0.1:<port> from any web page
/// (`fetch(..., {mode: "no-cors"})`); without this check every line of
/// the request body would be executed as a command (cross-protocol
/// attack). Commands are lowercase, so the uppercase method + " HTTP/"
/// pair never collides with a real command.
static bool looksLikeHttp(const std::string& line)
{
    static const char* kMethods[] = {"GET ", "POST ", "PUT ", "HEAD ",
                                     "OPTIONS ", "DELETE ", "PATCH ",
                                     "CONNECT ", "TRACE "};
    for (const char* m : kMethods)
        if (line.rfind(m, 0) == 0 && line.find(" HTTP/") != std::string::npos)
            return true;
    for (const char* h : {"Host:", "Origin:", "User-Agent:", "Content-Type:"})
        if (line.rfind(h, 0) == 0) return true;
    return false;
}

void InspectServer::runAccept()
{
    while (_running.load())
    {
        sockaddr_in peer{};
        socklen_t   sz = sizeof(peer);
        int fd = ::accept(_listenFd,
                          reinterpret_cast<sockaddr*>(&peer), &sz);
        if (fd < 0)
        {
            if (_running.load())
                std::perror("[inspect] accept");
            break;
        }

        // Bound sends so a stalled client (e.g. an event subscriber
        // that stopped reading) can't block the main thread inside
        // publishEvent forever; a timed-out send drops the client.
#if defined(_WIN32)
        DWORD sndTimeout = 2000;
#else
        timeval sndTimeout{2, 0};
#endif
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&sndTimeout),
                     sizeof(sndTimeout));

        // Per-connection worker. Multiple clients can hold sessions in
        // parallel so an event subscriber doesn't block command-only
        // clients (and vice versa). Workers are detached; stop() waits
        // on `_activeConns` instead of joining.
        {
            std::lock_guard<std::mutex> lk(_connsMu);
            if (_activeConns >= kMaxConns || !_running.load())
            {
                writeAll(fd, "ERR: too many connections\n");
                VS_CLOSE_SOCK(fd);
                continue;
            }
            ++_activeConns;
            _connFds.insert(fd);
        }
        std::thread(&InspectServer::runConn, this, fd).detach();
    }
}

void InspectServer::runConn(int fd)
{
    // One command per line, one reply per line. Two side-channel
    // commands — `watch on` / `watch off` — toggle event subscription
    // and are handled inline (no main-thread roundtrip). Everything
    // else is queued for `drain` to invoke under the editor lock.
    // Per-connection auth state. Each connection starts unauthenticated;
    // the `auth <token>` command flips it for THIS connection only.
    bool authed = false;

    while (_running.load())
    {
        std::string line = readLine(fd);
        if (line.empty()) break;

        // Cross-protocol guard: drop the connection without executing
        // anything once it looks like a browser's HTTP request.
        if (looksLikeHttp(line)) break;

        if (line == "watch on")
        {
            if (!_token.empty() && !authed)
            {
                std::lock_guard<std::mutex> lk(_writeMu);
                if (!writeAll(fd, "ERR: auth required; send 'auth <token>'\n")) break;
                continue;
            }
            { std::lock_guard<std::mutex> lk(_subMu); _subscribers.insert(fd); }
            std::lock_guard<std::mutex> lk(_writeMu);
            if (!writeAll(fd, "ok subscribed\n")) break;
            continue;
        }
        if (line == "watch off")
        {
            { std::lock_guard<std::mutex> lk(_subMu); _subscribers.erase(fd); }
            std::lock_guard<std::mutex> lk(_writeMu);
            if (!writeAll(fd, "ok unsubscribed\n")) break;
            continue;
        }

        auto cmd = std::make_shared<Cmd>();
        cmd->line  = std::move(line);
        cmd->reply = std::make_shared<std::promise<std::string>>();
        cmd->authed = &authed;
        std::future<std::string> fut = cmd->reply->get_future();

        { std::lock_guard<std::mutex> lk(_mu); _queue.push_back(cmd); }

        // Poll rather than block: a deferred reply held editor-side may
        // never be fulfilled if the editor shuts down, and stop() must
        // still be able to reap this worker.
        bool stopped = false;
        while (fut.wait_for(std::chrono::milliseconds(100)) ==
               std::future_status::timeout)
        {
            if (!_running.load()) { stopped = true; break; }
        }
        if (stopped) break;
        std::string reply;
        try { reply = fut.get(); }
        catch (...) { reply = "ERR: reply failure\n"; }

        if (!reply.empty() && reply.back() != '\n') reply.push_back('\n');
        {
            std::lock_guard<std::mutex> lk(_writeMu);
            if (!writeAll(fd, reply)) break;
        }
    }
    // Drop subscription, then close under _writeMu so a concurrent
    // publishEvent (which re-checks membership under _writeMu) can never
    // write to this fd after it is closed and possibly reused.
    { std::lock_guard<std::mutex> lk(_subMu); _subscribers.erase(fd); }
    {
        std::lock_guard<std::mutex> lk(_writeMu);
        VS_CLOSE_SOCK(fd);
    }
    // Last touch of `this`: notify while holding the lock so stop()
    // can't return (and destroy the cv) before notify_all completes.
    std::lock_guard<std::mutex> lk(_connsMu);
    _connFds.erase(fd);
    --_activeConns;
    _connsCv.notify_all();
}

void InspectServer::drain(const Handler& h)
{
    std::deque<std::shared_ptr<Cmd>> local;
    { std::lock_guard<std::mutex> lk(_mu); local.swap(_queue); }
    for (auto& c : local)
    {
        InspectReply reply(c->reply);
        // Fallback flag if a Cmd ever arrives without a connection-owned
        // one (shouldn't happen via runConn, but keeps the ref valid).
        bool authedFallback = false;
        bool& authed = c->authed ? *c->authed : authedFallback;
        try { h(c->line, reply, authed); }
        catch (const std::exception& e)
        {
            if (!reply.delivered())
                reply.send(std::string("ERR: ") + e.what());
        }
        catch (...)
        {
            if (!reply.delivered()) reply.send("ERR: unknown");
        }
        // If the handler neither sent nor deferred, the worker thread
        // would hang on its future forever — fall back to an empty ok.
        if (!reply.delivered()) reply.send("");
    }
}

void InspectServer::publishEvent(const std::string& line)
{
    if (!_running.load()) return;
    // Snapshot subscriber set under _subMu so the fan-out below
    // doesn't hold the membership lock while blocking on send().
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lk(_subMu);
        fds.assign(_subscribers.begin(), _subscribers.end());
    }
    if (fds.empty()) return;

    std::string framed = "EVENT ";
    framed += line;
    if (framed.empty() || framed.back() != '\n') framed.push_back('\n');

    std::vector<int> dead;
    for (int fd : fds)
    {
        std::lock_guard<std::mutex> lk(_writeMu);
        {
            // Still subscribed? (fd may have been closed + reused since
            // the snapshot.) Lock order: _writeMu -> _subMu.
            std::lock_guard<std::mutex> slk(_subMu);
            if (!_subscribers.count(fd)) continue;
        }
        if (!writeAll(fd, framed)) dead.push_back(fd);
    }
    if (!dead.empty())
    {
        std::lock_guard<std::mutex> lk(_subMu);
        for (int fd : dead) _subscribers.erase(fd);
    }
}

}  // namespace opencs
