#ifndef SPARK_TEST_WITHHELD_HTTP_SERVER_H
#define SPARK_TEST_WITHHELD_HTTP_SERVER_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#endif

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace spark::test {

class WithheldHttpServer {
public:
    WithheldHttpServer()
    {
#ifdef _WIN32
        WSADATA data{};
        assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(listener_ != invalid_socket);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        SocketLength length = sizeof(address);
        assert(::getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length) == 0);
        url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        assert(::listen(listener_, 1) == 0);
        worker_ = std::thread([this] {
            const auto client = ::accept(listener_, nullptr, nullptr);
            assert(client != invalid_socket);
            char byte{};
            assert(::recv(client, &byte, 1, 0) == 1);
            std::unique_lock lock(mutex_);
            received_ = true;
            cv_.notify_all();
            cv_.wait(lock, [this] { return release_; });
            lock.unlock();
            closeSocket(client);
        });
    }

    ~WithheldHttpServer()
    {
        {
            std::scoped_lock lock(mutex_);
            release_ = true;
        }
        cv_.notify_all();
        worker_.join();
        closeSocket(listener_);
#ifdef _WIN32
        WSACleanup();
#endif
    }

    const std::string &url() const { return url_; }
    bool waitReceived()
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(5), [this] { return received_; });
    }

private:
#ifdef _WIN32
    using Socket = SOCKET;
    using SocketLength = int;
    static constexpr Socket invalid_socket = INVALID_SOCKET;
    static void closeSocket(Socket socket) { ::closesocket(socket); }
#else
    using Socket = int;
    using SocketLength = socklen_t;
    static constexpr Socket invalid_socket = -1;
    static void closeSocket(Socket socket) { ::close(socket); }
#endif
    Socket listener_ = invalid_socket;
    std::string url_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool received_ = false;
    bool release_ = false;
};

}  // namespace spark::test

#endif
