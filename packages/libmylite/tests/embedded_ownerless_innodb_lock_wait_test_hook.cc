#include "tpool.h"

#include <chrono>
#include <thread>

namespace {

void unused_task(void *) {}

} // namespace

extern "C" int mylite_test_waitable_task_deadline() {
    tpool::waitable_task task(unused_task, nullptr);
    task.add_ref();
    std::thread releaser([&task] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        task.release();
    });

    const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
    const bool completed_early = task.wait_until(first_deadline);
    const auto final_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    const bool completed = task.wait_until(final_deadline);
    releaser.join();
    return !completed_early && completed ? 1 : 0;
}
