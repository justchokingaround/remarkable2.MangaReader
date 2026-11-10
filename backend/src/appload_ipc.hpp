#pragma once

#include "json.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace appload {

constexpr int32_t MSG_SYSTEM_TERMINATE = -1;
constexpr int32_t MSG_SYSTEM_NEW_COORDINATOR = -2;
constexpr int32_t MSG_SYSTEM_LOST_COORDINATOR = -3;
constexpr size_t MAX_MESSAGE_LENGTH = 10485760;  // 10 MiB

#pragma pack(push, 1)
struct PacketHeader {
    int32_t type;
    int32_t messageLength;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 8, "PacketHeader must be 8 bytes");

class IpcConnection {
    int fd_ = -1;
    std::mutex sendMutex_;
    std::atomic<bool> closed_{false};

public:
    IpcConnection() = default;
    ~IpcConnection() { closeSocket(); }

    bool connectTo(const std::string& socketPath) {
#ifdef __linux__
        const int sockType = SOCK_SEQPACKET;
#else
        const int sockType = SOCK_STREAM;
#endif
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        if (socketPath.size() >= sizeof(addr.sun_path)) {
            std::cerr << "[rm2manga] Socket path too long: " << socketPath << "\n";
            return false;
        }
        std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

        // Retry briefly in case rm-appload's detached listener thread hasn't bound yet.
        for (int attempt = 0; attempt < 60; ++attempt) {
            int fd = ::socket(AF_UNIX, sockType, 0);
            if (fd < 0) {
                std::cerr << "[rm2manga] socket() failed\n";
                return false;
            }
            if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
#ifdef __linux__
                // Each SEQPACKET message must fit in the send buffer (default wmem_max is only
                // ~176KB on the rM2), otherwise send() fails with EMSGSIZE after the header was
                // already sent and the stream desyncs. Large chapter lists (One Piece: 2000+
                // chapters) easily exceed that. Root may raise it past wmem_max with SNDBUFFORCE.
                int sndbuf = 8 * 1024 * 1024;
                if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, &sndbuf, sizeof(sndbuf)) != 0) {
                    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                }
#endif
                fd_ = fd;
                closed_ = false;
                return true;
            }
            ::close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        std::cerr << "[rm2manga] Failed to connect to AppLoad socket: " << socketPath << "\n";
        return false;
    }

    void closeSocket() {
        bool expected = false;
        if (closed_.compare_exchange_strong(expected, true)) {
            if (fd_ >= 0) {
                ::shutdown(fd_, SHUT_RDWR);
                ::close(fd_);
                fd_ = -1;
            }
        }
    }

    bool isClosed() const { return closed_.load(); }

    // Backend -> Frontend:
    // rm-appload reads PacketHeader (8 bytes), and reads payload ONLY when messageLength > 0.
    bool sendRaw(int32_t msgType, const std::string& payload) {
        if (closed_.load() || fd_ < 0) return false;
        if (payload.size() > MAX_MESSAGE_LENGTH) {
            std::cerr << "[rm2manga] Outbound message exceeds MAX_MESSAGE_LENGTH\n";
            return false;
        }

        std::lock_guard<std::mutex> lock(sendMutex_);
        if (closed_.load() || fd_ < 0) return false;

#ifdef __linux__
        {
            // Refuse to send a header whose payload cannot fit (would desync the stream)
            int sndbuf = 0;
            socklen_t sl = sizeof(sndbuf);
            if (::getsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, &sl) == 0 && sndbuf > 0 &&
                payload.size() + 1024 > static_cast<size_t>(sndbuf)) {
                std::cerr << "[rm2manga] Dropping oversized message type " << msgType << " ("
                          << payload.size() << " bytes > sndbuf " << sndbuf << ")\n";
                return false;
            }
        }
#endif
        PacketHeader hdr{msgType, static_cast<int32_t>(payload.size())};
        ssize_t n = ::send(fd_, &hdr, sizeof(hdr), MSG_NOSIGNAL_COMPAT());
        if (n != static_cast<ssize_t>(sizeof(hdr))) {
            return false;
        }
        if (hdr.messageLength > 0) {
            n = ::send(fd_, payload.data(), payload.size(), MSG_NOSIGNAL_COMPAT());
            if (n != static_cast<ssize_t>(payload.size())) {
                return false;
            }
        }
        return true;
    }

    bool sendJson(int32_t msgType, const Json::Value& val) {
        return sendRaw(msgType, Json::stringify(val));
    }

    // Frontend -> Backend:
    // rm-appload sends PacketHeader datagram followed by payload datagram (even if length == 0).
    void runLoop(const std::function<void(int32_t, const std::string&)>& handler) {
        std::vector<char> buffer(65536);

        while (!closed_.load() && fd_ >= 0) {
            PacketHeader hdr{0, 0};
            ssize_t r = recvPacket(&hdr, sizeof(hdr), true);
            if (r < static_cast<ssize_t>(sizeof(hdr))) {
                break;
            }
            if (hdr.messageLength < 0 || static_cast<size_t>(hdr.messageLength) > MAX_MESSAGE_LENGTH) {
                std::cerr << "[rm2manga] Invalid inbound messageLength: " << hdr.messageLength << "\n";
                break;
            }

            if (static_cast<size_t>(hdr.messageLength) > buffer.size()) {
                buffer.resize(static_cast<size_t>(hdr.messageLength));
            }

#ifdef __linux__
            // SOCK_SEQPACKET always queues a second packet from management.cpp::sendMessageTo
            ssize_t pr = ::recv(fd_, buffer.data(), static_cast<size_t>(hdr.messageLength), 0);
            if (pr < 0 || (hdr.messageLength > 0 && pr == 0)) {
                break;
            }
            std::string contents(buffer.data(), hdr.messageLength > 0 ? static_cast<size_t>(pr) : 0);
#else
            std::string contents;
            if (hdr.messageLength > 0) {
                ssize_t pr = recvPacket(buffer.data(), static_cast<size_t>(hdr.messageLength), true);
                if (pr != hdr.messageLength) break;
                contents.assign(buffer.data(), static_cast<size_t>(hdr.messageLength));
            }
#endif
            if (hdr.type == MSG_SYSTEM_TERMINATE) {
                handler(hdr.type, contents);
                break;
            }
            handler(hdr.type, contents);
        }
        closeSocket();
    }

private:
    static int MSG_NOSIGNAL_COMPAT() {
#ifdef MSG_NOSIGNAL
        return MSG_NOSIGNAL;
#else
        return 0;
#endif
    }

    ssize_t recvPacket(void* dst, size_t len, bool exactOnStream) {
#ifdef __linux__
        (void)exactOnStream;
        return ::recv(fd_, dst, len, 0);
#else
        if (!exactOnStream) return ::recv(fd_, dst, len, 0);
        size_t got = 0;
        char* p = static_cast<char*>(dst);
        while (got < len) {
            ssize_t n = ::recv(fd_, p + got, len - got, 0);
            if (n <= 0) return n;
            got += static_cast<size_t>(n);
        }
        return static_cast<ssize_t>(got);
#endif
    }
};

}  // namespace appload
