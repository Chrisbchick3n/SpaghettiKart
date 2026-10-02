#pragma once
// Minimal cross-platform TCP with message framing for netplay.
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Winsock headers are kept out of this header on purpose: they drag windows.h (and its min/max macros)
// into every file that includes it. SOCKET is a UINT_PTR on Windows.
#ifdef _WIN32
typedef uintptr_t NetSocketHandle;
#else
typedef int NetSocketHandle;
#endif

namespace Netplay {

struct Message {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
};

bool NetInit();                 // WSAStartup on Windows, no-op elsewhere
std::string NetLastError();

class Connection {
  public:
    Connection() = default;
    explicit Connection(NetSocketHandle s);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Blocking connect with timeout. host may be a name or IP.
    bool Connect(const std::string& host, uint16_t port, int timeoutMs, std::string& err);
    void Close();
    bool IsOpen() const;

    // Queues and flushes a framed message. Returns false if the connection died.
    bool Send(const std::vector<uint8_t>& framed);
    // Flushes pending outgoing data without blocking. Returns false if the connection died.
    bool Flush();
    // Reads whatever is available (non-blocking) and parses complete messages into the inbox.
    // Returns false if the connection closed or errored.
    bool Pump();
    bool PopMessage(Message& out);
    // Puts a message back at the front of the inbox (the relay peeks at the first message).
    void PushFront(Message m) {
        mInbox.push_front(std::move(m));
    }

    NetSocketHandle Handle() const {
        return mSock;
    }

  private:
    NetSocketHandle mSock = (NetSocketHandle) -1;
    std::vector<uint8_t> mRecv;
    std::vector<uint8_t> mSendBuf;
    std::deque<Message> mInbox;
    bool mDead = false;
};

class Listener {
  public:
    ~Listener();
    bool Listen(uint16_t port, std::string& err);
    // Returns a new connection or nullptr (non-blocking).
    Connection* Accept();
    void Close();
    NetSocketHandle Handle() const {
        return mSock;
    }

  private:
    NetSocketHandle mSock = (NetSocketHandle) -1;
};

// Waits until any of the sockets is readable, or the timeout expires.
void WaitReadable(const std::vector<NetSocketHandle>& socks, int timeoutMs);

} // namespace Netplay
