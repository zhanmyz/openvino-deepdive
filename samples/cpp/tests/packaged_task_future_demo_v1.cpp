#include <iostream>
#include <future>
#include <thread>

#include "../common/log.hpp"

int main() {
    // ========== Step 1: create a "task with a receipt" ==========
    // Imagine: you hand a package to a courier; packaged_task is the "package + receipt" combo.
    std::packaged_task<int()> task([]() {
        LOG(INFO) << "Task started..., worker thread ID: " << std::this_thread::get_id() << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));  // simulate a long-running computation
        LOG(INFO) << "Task finished, returning result..." << std::endl;
        return 123;  // computation result
    });

    // ========== Step 2: obtain the "claim ticket" ==========
    // The future is the tracking number; you use it later to fetch the result -- the read end of the channel.
    std::future<int> future_result = task.get_future();

    // ========== Step 3: hand the task to another thread for execution ==========
    // The courier (the new thread) takes the package away.
    std::thread worker_thread(std::move(task));  // Note: packaged_task is non-copyable; it can only be moved.

    LOG(INFO) << "Main thread keeps doing other work..." << std::endl;

    // ========== Step 4: wait for and retrieve the result ==========
    // Show the claim ticket to pick up the parcel; if it has not arrived, keep waiting.
    // The main thread can keep working until it actually needs the result.
    int result = future_result.get();  // Block until the task finishes and return its result.
    LOG(INFO) << "Main thread received the result: " << result << std::endl;

    worker_thread.join();  // Wait for the worker thread to finish.

    return 0;
}