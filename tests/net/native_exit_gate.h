#ifndef SPARK_TEST_NATIVE_EXIT_GATE_H
#define SPARK_TEST_NATIVE_EXIT_GATE_H

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace spark::test {

struct NativeExitGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    void block()
    {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [this] { return release; });
    }
    void waitEntered()
    {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(3), [this] { return entered; }));
    }
    void unblock()
    {
        std::scoped_lock lock(mutex);
        release = true;
        cv.notify_all();
    }
};

inline void holdNativeThreadExit(NativeExitGate &gate)
{
    struct Exit {
        NativeExitGate *gate = nullptr;
        ~Exit()
        {
            if (gate) {
                gate->block();
            }
        }
    };
    thread_local Exit exit;
    exit.gate = &gate;
}

}  // namespace spark::test

#endif
