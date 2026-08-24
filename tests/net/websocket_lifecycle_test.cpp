#include <cassert>

#include "websocket_lifecycle_test_support.h"

int main()
{
    spark::WebSocketClient client;
    std::mutex mutex;
    std::condition_variable cv;
    bool exited = false;
    spark::WebSocketClientTestAccess::startExitedWorker(client, mutex, cv, exited);

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&exited]() { return exited; }));
    }

    assert(spark::WebSocketClientTestAccess::joinable(client));
    client.close();
    assert(!spark::WebSocketClientTestAccess::joinable(client));
    client.close();

    std::atomic<bool> close_attempted{false};
    spark::WebSocketClientTestAccess::startLocalCloseWorker(client, close_attempted);
    client.close();
    assert(close_attempted.load());
    assert(!spark::WebSocketClientTestAccess::joinable(client));
    client.close();

    std::atomic<std::uint64_t> cleanup_count{0};
    assert(spark::WebSocketClientTestAccess::startThrowingCallbackWorker(client, cleanup_count));
    assert(spark::websocket_lifecycle_test::waitForExit(client));
    assert(cleanup_count.load(std::memory_order_acquire) == 2);
    client.close();
    assert(!spark::WebSocketClientTestAccess::joinable(client));

    assert(spark::WebSocketClientTestAccess::startThrowingCallbackWorker(client, cleanup_count));
    assert(spark::websocket_lifecycle_test::waitForExit(client));
    assert(cleanup_count.load(std::memory_order_acquire) == 4);
    client.close();
    assert(!spark::WebSocketClientTestAccess::joinable(client));

    {
        spark::WebSocketClient full_send;
        spark::WebSocketClientTestAccess::enqueue(full_send, "alpha");
        assert(spark::WebSocketClientTestAccess::processSend(full_send, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        assert(spark::WebSocketClientTestAccess::pendingSend(full_send).empty());
        assert(spark::WebSocketClientTestAccess::queued(full_send) == 0);
        full_send.close();
    }

    {
        spark::WebSocketClient retry_send;
        spark::WebSocketClientTestAccess::enqueue(retry_send, "retry");
        assert(spark::WebSocketClientTestAccess::processSend(retry_send, [](std::string_view) {
                   return std::pair{CURLE_AGAIN, std::size_t{0}};
               }) == spark::WebSocketClientTestAccess::SendStep::Retry);
        assert(spark::WebSocketClientTestAccess::pendingSend(retry_send) == "retry");
        assert(spark::WebSocketClientTestAccess::processSend(retry_send, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        assert(spark::WebSocketClientTestAccess::pendingSend(retry_send).empty());
        retry_send.close();
    }

    {
        spark::WebSocketClient partial_send;
        spark::WebSocketClientTestAccess::enqueue(partial_send, "partial");
        assert(spark::WebSocketClientTestAccess::processSend(partial_send, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size() - 3};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        assert(spark::WebSocketClientTestAccess::pendingSend(partial_send) == "ial");
        assert(spark::WebSocketClientTestAccess::processSend(partial_send, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        assert(spark::WebSocketClientTestAccess::pendingSend(partial_send).empty());
        partial_send.close();
    }

    {
        spark::WebSocketClient ordered_send;
        spark::WebSocketClientTestAccess::enqueue(ordered_send, "AAAA");
        spark::WebSocketClientTestAccess::enqueue(ordered_send, "BB");
        spark::WebSocketClientTestAccess::enqueue(ordered_send, "C");
        std::vector<std::string> attempts;
        auto send = [&attempts](std::string_view data) {
            attempts.emplace_back(data);
            const std::size_t sent = attempts.size() == 1 ? 2 : data.size();
            return std::pair{CURLE_OK, sent};
        };
        while (spark::WebSocketClientTestAccess::processSend(ordered_send, send) !=
               spark::WebSocketClientTestAccess::SendStep::Idle) {
        }
        assert((attempts == std::vector<std::string>{"AAAA", "AA", "BB", "C"}));
        ordered_send.close();
    }

    {
        spark::WebSocketClient failed_send;
        spark::WebSocketClientTestAccess::enqueue(failed_send, "failure");
        assert(spark::WebSocketClientTestAccess::processSend(failed_send, [](std::string_view) {
                   return std::pair{CURLE_SEND_ERROR, std::size_t{0}};
               }) == spark::WebSocketClientTestAccess::SendStep::Fatal);
        const auto termination = failed_send.termination();
        assert(termination.kind == spark::WebSocketClient::TerminationKind::SendError);
        assert(!termination.detail.empty());
        failed_send.close();
    }

    {
        spark::WebSocketClient deferred;
        const std::thread::id caller = std::this_thread::get_id();
        std::atomic<bool> ran_on_worker{false};
        assert(spark::WebSocketClientTestAccess::enqueueDeferred(
            deferred,
            [&ran_on_worker, caller]() {
                ran_on_worker.store(std::this_thread::get_id() != caller);
                return std::string("deferred");
            },
            32));
        std::thread worker([&deferred]() {
            assert(spark::WebSocketClientTestAccess::processSend(deferred, [](std::string_view data) {
                       return std::pair{CURLE_OK, data.size()};
                   }) == spark::WebSocketClientTestAccess::SendStep::Progress);
            spark::WebSocketClientTestAccess::setRunning(deferred, false);
        });
        worker.join();
        assert(ran_on_worker.load());
        deferred.close();
    }

    {
        spark::WebSocketClient throwing;
        const bool accepted = spark::WebSocketClientTestAccess::enqueueDeferred(
            throwing, []() -> std::string { throw std::runtime_error("deferred failure"); }, 1);
        assert(accepted);
        assert(spark::WebSocketClientTestAccess::processSend(throwing, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Fatal);
        assert(throwing.termination().kind == spark::WebSocketClient::TerminationKind::SendError);
        throwing.close();
    }

    {
        spark::WebSocketClient missing_encoder;
        const bool accepted = spark::WebSocketClientTestAccess::enqueueDeferred(
            missing_encoder, spark::WebSocketClient::DeferredEncoder{}, 1);
        assert(!accepted);
        assert(missing_encoder.termination().kind == spark::WebSocketClient::TerminationKind::SendError);
        missing_encoder.close();
    }

    {
        spark::WebSocketClient exact;
        const std::size_t maximum = spark::WebSocketClientTestAccess::maximumOutgoingMessageBytes();
        assert(spark::WebSocketClientTestAccess::enqueueDeferred(
            exact, [maximum]() { return std::string(maximum, 'x'); }, 1));
        assert(spark::WebSocketClientTestAccess::processSend(exact, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        exact.close();

        spark::WebSocketClient direct_exact;
        spark::WebSocketClientTestAccess::enqueue(direct_exact, std::string(maximum, 'x'));
        assert(spark::WebSocketClientTestAccess::processSend(direct_exact, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        direct_exact.close();

        spark::WebSocketClient direct_oversized;
        spark::WebSocketClientTestAccess::enqueue(direct_oversized, std::string(maximum + 1, 'x'));
        assert(direct_oversized.termination().kind == spark::WebSocketClient::TerminationKind::SendError);
        direct_oversized.close();

        spark::WebSocketClient oversized;
        assert(spark::WebSocketClientTestAccess::enqueueDeferred(
            oversized, [maximum]() { return std::string(maximum + 1, 'x'); }, 1));
        assert(spark::WebSocketClientTestAccess::processSend(oversized, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Fatal);
        assert(oversized.termination().kind == spark::WebSocketClient::TerminationKind::SendError);
        oversized.close();
    }

    {
        spark::WebSocketClient bounded_send;
        for (std::size_t i = 0; i < spark::WebSocketClientTestAccess::maximumQueuedSends(); ++i) {
            spark::WebSocketClientTestAccess::enqueue(bounded_send, "queued");
        }
        spark::WebSocketClientTestAccess::enqueue(bounded_send, "overflow");
        assert(bounded_send.termination().kind == spark::WebSocketClient::TerminationKind::SendError);
        assert(!spark::WebSocketClientTestAccess::running(bounded_send));
        bounded_send.close();
    }

    {
        spark::WebSocketClient bounded_deferred;
        assert(!spark::WebSocketClientTestAccess::enqueueDeferred(
            bounded_deferred, [] { return std::string("not-run"); },
            spark::WebSocketClientTestAccess::maximumQueuedSendBytes() + 1));
        assert(spark::WebSocketClientTestAccess::queued(bounded_deferred) == 0);
        bounded_deferred.close();

        spark::WebSocketClient count_limited;
        for (std::size_t i = 0; i < spark::WebSocketClientTestAccess::maximumQueuedSends(); ++i) {
            assert(spark::WebSocketClientTestAccess::enqueueDeferred(
                count_limited, [] { return std::string("queued"); }, 1));
        }
        assert(spark::WebSocketClientTestAccess::queuedBytes(count_limited) ==
               spark::WebSocketClientTestAccess::maximumQueuedSends());
        assert(!spark::WebSocketClientTestAccess::enqueueDeferred(
            count_limited, [] { return std::string("overflow"); }, 1));
        count_limited.close();
    }

    {
        spark::WebSocketClient bounded_send_bytes;
        const std::string maximum_message(spark::WebSocketClientTestAccess::maximumOutgoingMessageBytes(), 'x');
        const std::size_t accepted = spark::WebSocketClientTestAccess::maximumQueuedSendBytes() /
                                     spark::WebSocketClientTestAccess::maximumOutgoingMessageBytes();
        for (std::size_t i = 0; i < accepted; ++i) {
            spark::WebSocketClientTestAccess::enqueue(bounded_send_bytes, maximum_message);
        }
        spark::WebSocketClientTestAccess::enqueue(bounded_send_bytes, "overflow");
        assert(bounded_send_bytes.termination().kind == spark::WebSocketClient::TerminationKind::SendError);
        assert(!spark::WebSocketClientTestAccess::running(bounded_send_bytes));
        bounded_send_bytes.close();
    }

    {
        spark::WebSocketClient failed_receive;
        spark::WebSocketClientTestAccess::receiveFailure(failed_receive, CURLE_RECV_ERROR);
        const auto termination = failed_receive.termination();
        assert(termination.kind == spark::WebSocketClient::TerminationKind::ReceiveError);
        assert(!termination.detail.empty());
    }

    {
        std::string response;
        const std::size_t maximum = spark::WebSocketClientTestAccess::maximumCreateResponseBytes();
        std::string exact(maximum, 'x');
        assert(spark::WebSocketClientTestAccess::writeResponse(exact.data(), 1, exact.size(), response) == maximum);
        assert(response.size() == maximum);
        assert(spark::WebSocketClientTestAccess::writeResponse(exact.data(), 1, 1, response) == 0);
        assert(spark::WebSocketClientTestAccess::writeResponse(nullptr, std::numeric_limits<std::size_t>::max(), 2,
                                                               response) == 0);
    }

    spark::websocket_lifecycle_test::runViewerLifecycleTests();
    return 0;
}
