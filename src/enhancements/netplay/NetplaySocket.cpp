#include "NetplaySocket.h"
#include "NetplayProtocol.h"

#include <cstring>

#ifdef _WIN32
// Winsock's select() handles only 64 sockets by default; the relay can have many more.
#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
static_assert(sizeof(SOCKET) == sizeof(NetSocketHandle), "socket handle size");
#define NP_CLOSE closesocket
#define NP_INVALID INVALID_SOCKET
#define NP_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK)
#define NP_INPROGRESS(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
static int np_errno() {
    return WSAGetLastError();
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define NP_CLOSE ::close
#define NP_INVALID (-1)
#define NP_WOULDBLOCK(e) ((e) == EWOULDBLOCK || (e) == EAGAIN)
#define NP_INPROGRESS(e) ((e) == EINPROGRESS)
static int np_errno() {
    return errno;
}
#endif

#ifdef MSG_NOSIGNAL
#define NP_SENDFLAGS MSG_NOSIGNAL
#else
#define NP_SENDFLAGS 0
#endif

namespace Netplay {

static bool SetNonBlocking(NetSocketHandle s) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static void SetNoDelay(NetSocketHandle s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*) &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

bool NetInit() {
#ifdef _WIN32
    static bool sInit = false;
    if (!sInit) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
            return false;
        }
        sInit = true;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    return true;
}

std::string NetLastError() {
#ifdef _WIN32
    return "socket error " + std::to_string(WSAGetLastError());
#else
    return strerror(errno);
#endif
}

// ---------------------------------------------------------------------------
Connection::Connection(NetSocketHandle s) : mSock(s) {
    SetNonBlocking(mSock);
    SetNoDelay(mSock);
}

Connection::~Connection() {
    Close();
}

bool Connection::IsOpen() const {
    return mSock != (NetSocketHandle) NP_INVALID && !mDead;
}

void Connection::Close() {
    if (mSock != (NetSocketHandle) NP_INVALID) {
        NP_CLOSE(mSock);
        mSock = (NetSocketHandle) NP_INVALID;
    }
    mRecv.clear();
    mSendBuf.clear();
    mInbox.clear();
    mDead = false;
}

bool Connection::Connect(const std::string& host, uint16_t port, int timeoutMs, std::string& err) {
    Close();
    NetInit();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr) {
        err = "Could not find host \"" + host + "\"";
        return false;
    }
    bool ok = false;
    for (addrinfo* ai = res; ai != nullptr && !ok; ai = ai->ai_next) {
        NetSocketHandle s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == (NetSocketHandle) NP_INVALID) {
            continue;
        }
        SetNonBlocking(s);
        int r = connect(s, ai->ai_addr, (int) ai->ai_addrlen);
        if (r != 0 && !NP_INPROGRESS(np_errno())) {
            NP_CLOSE(s);
            continue;
        }
        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        r = select((int) s + 1, nullptr, &wfds, &efds, &tv);
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        if (r > 0 && FD_ISSET(s, &wfds) && getsockopt(s, SOL_SOCKET, SO_ERROR, (char*) &soErr, &len) == 0 &&
            soErr == 0) {
            mSock = s;
            SetNoDelay(mSock);
            ok = true;
        } else {
            NP_CLOSE(s);
        }
    }
    freeaddrinfo(res);
    if (!ok) {
        err = "Could not connect to " + host + ":" + portStr + ".";
    }
    return ok;
}

bool Connection::Send(const std::vector<uint8_t>& framed) {
    if (!IsOpen()) {
        return false;
    }
    mSendBuf.insert(mSendBuf.end(), framed.begin(), framed.end());
    return Flush();
}

bool Connection::Flush() {
    if (!IsOpen()) {
        return false;
    }
    while (!mSendBuf.empty()) {
        int n = send(mSock, (const char*) mSendBuf.data(), (int) mSendBuf.size(), NP_SENDFLAGS);
        if (n > 0) {
            mSendBuf.erase(mSendBuf.begin(), mSendBuf.begin() + n);
        } else if (n < 0 && NP_WOULDBLOCK(np_errno())) {
            break; // kernel buffer full, try again later
        } else {
            mDead = true;
            return false;
        }
    }
    // Guard against a peer that never reads.
    if (mSendBuf.size() > 8 * kMaxMessageSize) {
        mDead = true;
        return false;
    }
    return true;
}

bool Connection::Pump() {
    if (!IsOpen()) {
        return false;
    }
    Flush();
    uint8_t buf[16384];
    while (true) {
        int n = recv(mSock, (char*) buf, sizeof(buf), 0);
        if (n > 0) {
            mRecv.insert(mRecv.end(), buf, buf + n);
        } else if (n < 0 && NP_WOULDBLOCK(np_errno())) {
            break;
        } else {
            mDead = true; // closed (n == 0) or error
            break;
        }
    }
    // Parse complete frames
    size_t off = 0;
    while (mRecv.size() - off >= 5) {
        uint32_t len = mRecv[off + 1] | (mRecv[off + 2] << 8) | (mRecv[off + 3] << 16) | ((uint32_t) mRecv[off + 4] << 24);
        if (len > kMaxMessageSize) {
            mDead = true;
            break;
        }
        if (mRecv.size() - off - 5 < len) {
            break;
        }
        Message m;
        m.type = mRecv[off];
        m.payload.assign(mRecv.begin() + off + 5, mRecv.begin() + off + 5 + len);
        mInbox.push_back(std::move(m));
        off += 5 + len;
    }
    if (off > 0) {
        mRecv.erase(mRecv.begin(), mRecv.begin() + off);
    }
    // Messages already received are still delivered even if the socket just died.
    return !mDead;
}

bool Connection::PopMessage(Message& out) {
    if (mInbox.empty()) {
        return false;
    }
    out = std::move(mInbox.front());
    mInbox.pop_front();
    return true;
}

// ---------------------------------------------------------------------------
Listener::~Listener() {
    Close();
}

void Listener::Close() {
    if (mSock != (NetSocketHandle) NP_INVALID) {
        NP_CLOSE(mSock);
        mSock = (NetSocketHandle) NP_INVALID;
    }
}

bool Listener::Listen(uint16_t port, std::string& err) {
    Close();
    NetInit();
    // Prefer a dual-stack IPv6 socket so both IPv4 and IPv6 friends can join; fall back to plain IPv4
    // (e.g. on PCs with IPv6 disabled).
    for (int attempt = 0; attempt < 2; attempt++) {
        bool v6 = attempt == 0;
        NetSocketHandle s = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == (NetSocketHandle) NP_INVALID) {
            continue;
        }
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*) &one, sizeof(one));
        int r;
        if (v6) {
            int zero = 0;
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*) &zero, sizeof(zero));
            sockaddr_in6 a{};
            a.sin6_family = AF_INET6;
            a.sin6_addr = in6addr_any;
            a.sin6_port = htons(port);
            r = bind(s, (sockaddr*) &a, sizeof(a));
        } else {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons(port);
            r = bind(s, (sockaddr*) &a, sizeof(a));
        }
        if (r != 0 || listen(s, 8) != 0) {
            err = "Could not open port " + std::to_string(port) + " (is something else using it?): " + NetLastError();
            NP_CLOSE(s);
            continue;
        }
        SetNonBlocking(s);
        mSock = s;
        err.clear();
        return true;
    }
    if (err.empty()) {
        err = "Could not create socket: " + NetLastError();
    }
    return false;
}

Connection* Listener::Accept() {
    if (mSock == (NetSocketHandle) NP_INVALID) {
        return nullptr;
    }
    NetSocketHandle c = accept(mSock, nullptr, nullptr);
    if (c == (NetSocketHandle) NP_INVALID) {
        return nullptr;
    }
    return new Connection(c);
}

void WaitReadable(const std::vector<NetSocketHandle>& socks, int timeoutMs) {
    fd_set rfds;
    FD_ZERO(&rfds);
    int maxfd = 0;
    for (NetSocketHandle s : socks) {
        if (s == (NetSocketHandle) NP_INVALID) {
            continue;
        }
        FD_SET(s, &rfds);
        if ((int) s > maxfd) {
            maxfd = (int) s;
        }
    }
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
}

} // namespace Netplay
