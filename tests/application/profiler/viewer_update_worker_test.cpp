#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "application/profiler/viewer_update_worker.h"

namespace {

using namespace std::chrono_literals;

struct Probe {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<spark::ViewerUpdateWorker::WorkType> executed;
    std::optional<spark::ViewerUpdateWorker::Completion> completion;
    bool block_execution = false;
    bool execution_entered = false;
    bool release_execution = false;
    bool fail_next = false;
};

template <typename Predicate>
bool waitFor(Probe &probe, Predicate predicate)
{
    std::unique_lock lock(probe.mutex);
    return probe.cv.wait_for(lock, 2s, std::move(predicate));
}

void resetExecution(Probe &probe, bool block)
{
    std::scoped_lock lock(probe.mutex);
    probe.block_execution = block;
    probe.execution_entered = false;
    probe.release_execution = false;
}

}  // namespace

int main()
{
    auto probe = std::make_shared<Probe>();
    spark::ViewerUpdateWorker worker(
        [probe](const spark::ViewerUpdateWorker::WorkItem &work) {
            std::unique_lock lock(probe->mutex);
            probe->executed.push_back(work.type);
            probe->execution_entered = true;
            probe->cv.notify_all();
            if (probe->block_execution) {
                probe->cv.wait(lock, [&probe] { return probe->release_execution; });
            }
            const bool fail = probe->fail_next;
            probe->fail_next = false;
            lock.unlock();
            if (fail) {
                throw std::runtime_error("injected viewer worker failure");
            }
            return work.type == spark::ViewerUpdateWorker::WorkType::Open ? std::string("viewer-url")
                                                                          : std::string("update-key");
        },
        [probe](spark::ViewerUpdateWorker::Completion completion) {
            std::scoped_lock lock(probe->mutex);
            probe->completion = std::move(completion);
            probe->cv.notify_all();
        });

    assert(worker.start());
    assert(worker.start());

    resetExecution(*probe, true);
    const auto first_generation = worker.enqueueOpen({}, {}, "Console");
    assert(first_generation.has_value());
    assert(waitFor(*probe, [&] { return probe->execution_entered; }));
    assert(!worker.enqueueOpen({}, {}, "Second"));
    assert(!worker.enqueueUpdate({}, {}, *first_generation));

    {
        std::scoped_lock lock(probe->mutex);
        probe->release_execution = true;
    }
    probe->cv.notify_all();
    assert(waitFor(*probe, [&] { return probe->completion.has_value(); }));
    {
        std::scoped_lock lock(probe->mutex);
        assert(probe->completion->type == spark::ViewerUpdateWorker::WorkType::Open);
        assert(probe->completion->generation == *first_generation);
        assert(probe->completion->url == "viewer-url");
        assert(probe->completion->sender_name == "Console");
    }
    assert(worker.openPending());
    assert(!worker.enqueueOpen({}, {}, "Still pending"));
    assert(worker.completeOpen(*first_generation));
    assert(!worker.openPending());

    resetExecution(*probe, true);
    const auto second_generation = worker.generation();
    assert(worker.enqueueUpdate({}, {}, second_generation));
    assert(waitFor(*probe, [&] { return probe->execution_entered; }));
    worker.invalidate();
    const auto invalidated_generation = worker.generation();
    assert(invalidated_generation == second_generation + 1);
    assert(!worker.current(second_generation));
    assert(worker.current(invalidated_generation));
    assert(!worker.completeOpen(second_generation));
    assert(!worker.enqueueUpdate({}, {}, second_generation));
    {
        std::scoped_lock lock(probe->mutex);
        probe->release_execution = true;
    }
    probe->cv.notify_all();
    worker.stop();
    assert(worker.available());

    assert(worker.start());
    resetExecution(*probe, false);
    std::size_t executed_before_update = 0;
    {
        std::scoped_lock lock(probe->mutex);
        executed_before_update = probe->executed.size();
    }
    assert(worker.enqueueUpdate({}, {}, invalidated_generation));
    assert(waitFor(*probe, [&] { return probe->executed.size() > executed_before_update; }));
    {
        std::scoped_lock lock(probe->mutex);
        assert(probe->executed.back() == spark::ViewerUpdateWorker::WorkType::Update);
    }
    worker.stop();
    assert(worker.available());

    assert(worker.start());
    {
        std::scoped_lock lock(probe->mutex);
        probe->fail_next = true;
    }
    assert(worker.enqueueUpdate({}, {}, invalidated_generation));
    assert(waitFor(*probe, [&] { return probe->executed.size() > executed_before_update + 1; }));
    worker.stop();
    assert(!worker.current(invalidated_generation));
    assert(worker.consumeFailure());
    assert(!worker.consumeFailure());

    assert(worker.start());
    assert(worker.current(invalidated_generation));
    resetExecution(*probe, false);
    assert(worker.enqueueUpdate({}, {}, invalidated_generation));
    assert(waitFor(*probe, [&] { return probe->executed.size() > executed_before_update + 2; }));
    worker.stop();
    assert(worker.available());
    assert(!worker.current(invalidated_generation));
    assert(!worker.enqueueUpdate({}, {}, invalidated_generation));
    return 0;
}
