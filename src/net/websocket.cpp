#include "net/websocket.h"

#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <utility>

#include <curl/curl.h>

namespace spark {

namespace {

struct CurlEasyDeleter {
    std::atomic<std::uint64_t> *cleanup_count = nullptr;

    void operator()(CURL *curl) const
    {
        if (curl != nullptr) {
            curl_easy_cleanup(curl);
            if (cleanup_count != nullptr) {
                cleanup_count->fetch_add(1, std::memory_order_release);
            }
        }
    }
};

class CurlHeaderList {
public:
    explicit CurlHeaderList(std::atomic<std::uint64_t> *cleanup_count) : cleanup_count_(cleanup_count) {}
    ~CurlHeaderList()
    {
        if (headers_ != nullptr) {
            curl_slist_free_all(headers_);
            if (cleanup_count_ != nullptr) {
                cleanup_count_->fetch_add(1, std::memory_order_release);
            }
        }
    }

    CurlHeaderList(const CurlHeaderList &) = delete;
    CurlHeaderList &operator=(const CurlHeaderList &) = delete;

    void append(const char *value)
    {
        if (curl_slist *updated = curl_slist_append(headers_, value)) {
            headers_ = updated;
        }
    }

    [[nodiscard]] curl_slist *get() const { return headers_; }

private:
    curl_slist *headers_ = nullptr;
    std::atomic<std::uint64_t> *cleanup_count_ = nullptr;
};

using CurlEasyPtr = std::unique_ptr<CURL, CurlEasyDeleter>;

// Parse {"key":"<channelId>"} from the /create response.
std::string parseChannelKey(const std::string &json)
{
    // Simple JSON extraction: find "key":"..." and extract the value.
    auto pos = json.find("\"key\"");
    if (pos == std::string::npos) {
        return {};
    }
    pos = json.find(':', pos + 5);
    if (pos == std::string::npos) {
        return {};
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
        return {};
    }
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) {
        return {};
    }
    return json.substr(pos + 1, end - pos - 1);
}

}  // namespace

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient()
{
    close();
}

std::size_t WebSocketClient::writeCallback(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) noexcept
{
    if (size != 0 && nmemb > std::numeric_limits<std::size_t>::max() / size) {
        return 0;
    }
    const std::size_t bytes = size * nmemb;
    if (bytes == 0) {
        return 0;
    }
    auto *response = static_cast<std::string *>(userdata);
    if (response == nullptr || bytes > kMaxCreateResponseBytes || response->size() > kMaxCreateResponseBytes - bytes ||
        (bytes != 0 && ptr == nullptr)) {
        return 0;
    }
    try {
        response->append(ptr, bytes);
    }
    catch (...) {
        return 0;
    }
    return bytes;
}

std::string WebSocketClient::connect(const std::string &host, const std::string &user_agent)
{
    if (thread_.joinable()) {
        close();
    }
    {
        std::scoped_lock lock(termination_mutex_);
        termination_ = {};
    }
    {
        std::scoped_lock lock(send_mutex_);
        send_queue_ = {};
        queued_send_bytes_ = 0;
        pending_send_.reset();
        pending_send_offset_ = 0;
    }
    host_ = host;
    user_agent_ = user_agent;

    // Step 1: Create a channel via HTTP GET.
    CURL *curl = curl_easy_init();
    if (!curl) {
        return {};
    }

    std::string url = "https://" + host + "/create";
    std::string response;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!user_agent.empty()) {
        std::string ua_header = "User-Agent: " + user_agent;
        headers = curl_slist_append(headers, ua_header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WebSocketClient::writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        return {};
    }

    channel_id_ = parseChannelKey(response);
    if (channel_id_.empty()) {
        return {};
    }

    // Step 2: Connect via WebSocket.
    local_close_requested_.store(false);
    running_.store(true);
    if (!startReceiveWorker()) {
        running_.store(false);
        return {};
    }

    // Give the connection a brief moment to establish.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!running_.load()) {
        thread_.join();
        return {};
    }

    return channel_id_;
}

bool WebSocketClient::startReceiveWorker()
{
    try {
        thread_ = std::thread([this] {
            try {
                runReceiveLoop();
            }
            catch (const std::exception &error) {
                recordTermination(TerminationKind::WorkerFailure, error.what());
            }
            catch (...) {
                recordTermination(TerminationKind::WorkerFailure, "unknown exception");
            }
            running_.store(false);
            notifyTermination();
        });
    }
    catch (...) {
        recordTermination(TerminationKind::WorkerFailure, "failed to start worker");
        return false;
    }
    return true;
}

void WebSocketClient::send(const std::string &message) noexcept
{
    try {
        enqueueSendJob(SendJob{.message = message, .accounted_bytes = message.size()});
    }
    catch (...) {
        recordSendFailure("outgoing WebSocket message allocation failed");
    }
}

bool WebSocketClient::sendDeferred(DeferredEncoder encoder, std::size_t accounted_input_bytes) noexcept
{
    if (!running_.load()) {
        return false;
    }
    if (!encoder) {
        recordSendFailure("deferred encoder missing");
        return false;
    }
    try {
        return enqueueSendJob(SendJob{.encoder = std::move(encoder), .accounted_bytes = accounted_input_bytes});
    }
    catch (...) {
        recordSendFailure("deferred WebSocket job allocation failed");
        return false;
    }
}

bool WebSocketClient::enqueueSendJob(SendJob job) noexcept
{
    if (!running_.load()) {
        return false;
    }
    bool rejected = false;
    bool allocation_failure = false;
    try {
        std::scoped_lock lock(send_mutex_);
        if (!running_.load()) {
            return false;
        }
        if ((!job.encoder && job.message.size() > kMaxOutgoingMessageBytes) ||
            job.accounted_bytes > kMaxQueuedSendBytes || send_queue_.size() >= kMaxQueuedSends ||
            queued_send_bytes_ > kMaxQueuedSendBytes ||
            job.accounted_bytes > kMaxQueuedSendBytes - queued_send_bytes_) {
            rejected = true;
        }
        else {
            try {
                send_queue_.push(std::move(job));
            }
            catch (...) {
                allocation_failure = true;
            }
            if (!allocation_failure) {
                queued_send_bytes_ += send_queue_.back().accounted_bytes;
            }
        }
    }
    catch (...) {
        allocation_failure = true;
    }
    if (rejected) {
        rejectSendQueue();
        return false;
    }
    if (allocation_failure) {
        recordSendFailure("outgoing WebSocket queue allocation failed");
        return false;
    }
    send_cv_.notify_one();
    return true;
}

void WebSocketClient::rejectSendQueue() noexcept
{
    recordSendFailure("outgoing WebSocket queue exceeded its limit");
    running_.store(false);
    send_cv_.notify_all();
}

void WebSocketClient::recordSendFailure(const char *detail) noexcept
{
    try {
        recordTermination(TerminationKind::SendError, detail);
    }
    catch (...) {
        running_.store(false);
    }
    running_.store(false);
    send_cv_.notify_all();
}

void WebSocketClient::close() noexcept
{
    if (running_.load()) {
        local_close_requested_.store(true);
        try {
            recordTermination(TerminationKind::LocalClose);
        }
        catch (...) {
            running_.store(false);
        }
    }
    running_.store(false);
    send_cv_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        try {
            thread_.join();
        }
        catch (...) {
            running_.store(false);
        }
    }
}

WebSocketClient::Termination WebSocketClient::termination() const
{
    std::scoped_lock lock(termination_mutex_);
    return termination_;
}

void WebSocketClient::recordTermination(TerminationKind kind, std::string detail)
{
    std::scoped_lock lock(termination_mutex_);
    if (termination_.kind == TerminationKind::None) {
        termination_ = {.kind = kind, .detail = std::move(detail)};
    }
}

void WebSocketClient::notifyTermination() noexcept
{
    if (!close_cb_) {
        return;
    }
    try {
        close_cb_(termination());
    }
    catch (...) {
        return;
    }
}

WebSocketClient::SendStep WebSocketClient::processNextSend(const SendFunction &send_function)
{
    if (!pending_send_) {
        SendJob job;
        {
            std::scoped_lock lock(send_mutex_);
            if (send_queue_.empty()) {
                return SendStep::Idle;
            }
            job = std::move(send_queue_.front());
            queued_send_bytes_ -= job.accounted_bytes;
            send_queue_.pop();
        }
        pending_send_offset_ = 0;

        if (job.encoder) {
            try {
                pending_send_ = job.encoder();
            }
            catch (const std::exception &error) {
                recordTermination(TerminationKind::SendError, error.what());
                return SendStep::Fatal;
            }
            catch (...) {
                recordTermination(TerminationKind::SendError, "deferred encoder failed");
                return SendStep::Fatal;
            }
        }
        else {
            pending_send_ = std::move(job.message);
        }
        if (pending_send_->size() > kMaxOutgoingMessageBytes) {
            pending_send_.reset();
            recordTermination(TerminationKind::SendError, "outgoing WebSocket message exceeded its limit");
            return SendStep::Fatal;
        }
    }

    const std::size_t remaining = pending_send_->size() - pending_send_offset_;
    const SendAttempt attempt = send_function(pending_send_->data() + pending_send_offset_, remaining);
    if (attempt.sent > remaining) {
        recordTermination(TerminationKind::SendError, "invalid send byte count");
        return SendStep::Fatal;
    }
    pending_send_offset_ += attempt.sent;

    if (attempt.code == CURLE_AGAIN) {
        return SendStep::Retry;
    }
    if (attempt.code != CURLE_OK) {
        recordTermination(TerminationKind::SendError, curl_easy_strerror(static_cast<CURLcode>(attempt.code)));
        return SendStep::Fatal;
    }
    if (pending_send_offset_ == pending_send_->size()) {
        pending_send_.reset();
        pending_send_offset_ = 0;
    }
    return SendStep::Progress;
}

bool WebSocketClient::drainLocalClose(const SendFunction &send_function)
{
    if (!local_close_requested_.exchange(false)) {
        return false;
    }
    for (std::size_t attempt = 0; attempt <= kMaxQueuedSends; ++attempt) {
        const SendStep send_step = processNextSend(send_function);
        if (send_step == SendStep::Idle || send_step == SendStep::Fatal || send_step == SendStep::Retry) {
            break;
        }
    }
    return true;
}

void WebSocketClient::handleReceiveFailure(int code)
{
    if (code == CURLE_GOT_NOTHING) {
        recordTermination(TerminationKind::RemoteClose);
    }
    else {
        recordTermination(TerminationKind::ReceiveError, curl_easy_strerror(static_cast<CURLcode>(code)));
    }
}

void WebSocketClient::runReceiveLoop()
{
    CurlEasyPtr curl(curl_easy_init(), CurlEasyDeleter{resource_cleanup_count_for_testing_});
    if (!curl) {
        recordTermination(TerminationKind::WorkerFailure, "curl initialization failed");
        return;
    }

    std::string ws_url = "wss://" + host_ + "/" + channel_id_;
    CurlHeaderList headers(resource_cleanup_count_for_testing_);
    if (!user_agent_.empty()) {
        std::string ua_header = "User-Agent: " + user_agent_;
        headers.append(ua_header.c_str());
    }

    curl_easy_setopt(curl.get(), CURLOPT_URL, ws_url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECT_ONLY, 2L);  // WebSocket mode

    if (!incoming_message_for_testing_.empty() && message_cb_) {
        message_cb_(incoming_message_for_testing_);
    }

    CURLcode rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK) {
        handleReceiveFailure(rc);
        return;
    }

    // Poll loop: receive messages and send queued messages.
    constexpr auto k_poll_interval_ms = 100L;
    char recv_buf[4096];
    std::string pending_recv;  // buffered partial message

    while (running_.load()) {
        const SendStep send_step = processNextSend([&curl](const char *data, std::size_t size) {
            std::size_t sent = 0;
            const CURLcode code = curl_ws_send(curl.get(), data, size, &sent, 0, CURLWS_TEXT);
            return SendAttempt{.code = code, .sent = sent};
        });
        if (send_step == SendStep::Fatal) {
            break;
        }

        // Try to receive.
        size_t recv = 0;
        const struct curl_ws_frame *frame = nullptr;
        CURLcode r = curl_ws_recv(curl.get(), recv_buf, sizeof(recv_buf), &recv, &frame);
        if (r == CURLE_OK && frame) {
            if ((frame->flags & CURLWS_CLOSE) != 0) {
                recordTermination(TerminationKind::RemoteClose);
                break;
            }
            if (frame->bytesleft < 0 || pending_recv.size() > kMaxIncomingMessageBytes ||
                recv > kMaxIncomingMessageBytes - pending_recv.size()) {
                recordTermination(TerminationKind::ReceiveError, "incoming WebSocket message exceeded its limit");
                break;
            }
            const std::size_t remaining_capacity = kMaxIncomingMessageBytes - pending_recv.size() - recv;
            if (std::cmp_greater(frame->bytesleft, remaining_capacity)) {
                recordTermination(TerminationKind::ReceiveError, "incoming WebSocket message exceeded its limit");
                break;
            }
            if (recv > 0) {
                pending_recv.append(recv_buf, recv);
                if (frame->bytesleft == 0) {
                    if (message_cb_ && !pending_recv.empty()) {
                        message_cb_(pending_recv);
                    }
                    pending_recv.clear();
                }
            }
        }
        else if (r == CURLE_AGAIN) {
            std::unique_lock<std::mutex> lock(send_mutex_);
            send_cv_.wait_for(lock, std::chrono::milliseconds(k_poll_interval_ms),
                              [this]() { return !running_.load() || (!pending_send_ && !send_queue_.empty()); });
        }
        else if (r == CURLE_OK) {
            recordTermination(TerminationKind::ReceiveError, "missing frame metadata");
            break;
        }
        else {
            handleReceiveFailure(r);
            break;
        }
    }

    const SendFunction send_function = [&curl](const char *data, std::size_t size) {
        std::size_t sent = 0;
        const CURLcode code = curl_ws_send(curl.get(), data, size, &sent, 0, CURLWS_TEXT);
        return SendAttempt{.code = code, .sent = sent};
    };
    if (drainLocalClose(send_function)) {
        size_t sent = 0;
        const char *close_msg = "";
        curl_ws_send(curl.get(), close_msg, 0, &sent, 0, CURLWS_CLOSE);
    }
}

}  // namespace spark
