#ifndef SPARK_APPLICATION_HEALTH_HEALTH_DASHBOARD_CONNECTION_H
#define SPARK_APPLICATION_HEALTH_HEALTH_DASHBOARD_CONNECTION_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/ws/viewer_socket.h"
#include "net/bytebin.h"

namespace spark {

// Small connection boundary used by HealthDashboard and its offline tests.
class HealthDashboardConnection {
public:
    using UploadCallback = std::function<UploadResult()>;
    using IsKeyTrustedCallback = std::function<bool(const std::vector<std::uint8_t> &)>;

    virtual ~HealthDashboardConnection() = default;

    // Opens the connection and invokes upload for the initial health payload.
    // The returned string is the viewer URL, or empty on failure.
    virtual std::string open(const UploadCallback &upload) = 0;
    virtual bool tick() = 0;
    virtual bool isOpen() const = 0;
    virtual bool hasClient() const = 0;
    virtual void close() = 0;
    virtual SocketChannelInfo channelInfo() const = 0;
    virtual bool sendStatistics(const std::string &platform, const std::string &system, const std::string &metrics) = 0;
    virtual std::vector<std::uint8_t> pendingKey(const std::string &client_id) const = 0;
    virtual void sendClientTrusted(const std::string &client_id) = 0;
    virtual void setIsKeyTrustedCallback(IsKeyTrustedCallback callback) = 0;
};

// Creates the production ViewerSocket-backed connection used by the platform integration.
std::unique_ptr<HealthDashboardConnection> makeHealthDashboardViewerSocketConnection(ViewerSocket::Config config,
                                                                                     Crypto::KeyPair key_pair);

}  // namespace spark

#endif  // SPARK_APPLICATION_HEALTH_HEALTH_DASHBOARD_CONNECTION_H
