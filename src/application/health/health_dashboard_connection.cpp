#include <utility>

#include "application/health/health_dashboard.h"

namespace spark {
namespace {

class ViewerSocketConnection final : public HealthDashboardConnection {
public:
    ViewerSocketConnection(ViewerSocket::Config config, Crypto::KeyPair key_pair)
        : socket_(std::move(config), std::move(key_pair))
    {
    }

    std::string open(const UploadCallback &upload) override
    {
        return socket_.open([&upload](const std::string &) {
            const UploadResult result = upload();
            return result.ok ? result.key : std::string();
        });
    }

    bool tick() override { return socket_.tick(); }
    bool isOpen() const override { return socket_.isOpen(); }
    bool hasClient() const override { return socket_.hasClient(); }
    void close() override { socket_.close(); }
    SocketChannelInfo channelInfo() const override { return socket_.channelInfo(); }
    bool sendStatistics(const std::string &platform, const std::string &system, const std::string &metrics) override
    {
        return socket_.sendStatistics(platform, system, metrics);
    }
    std::vector<std::uint8_t> pendingKey(const std::string &client_id) const override
    {
        return socket_.pendingKey(client_id);
    }
    void sendClientTrusted(const std::string &client_id) override { socket_.sendClientTrusted(client_id); }
    void setIsKeyTrustedCallback(IsKeyTrustedCallback callback) override
    {
        socket_.setIsKeyTrustedCallback(std::move(callback));
    }

private:
    ViewerSocket socket_;
};

}  // namespace

std::unique_ptr<HealthDashboardConnection> makeHealthDashboardViewerSocketConnection(ViewerSocket::Config config,
                                                                                     Crypto::KeyPair key_pair)
{
    return std::make_unique<ViewerSocketConnection>(std::move(config), std::move(key_pair));
}

}  // namespace spark
