#include <future>
#include <thread>
#include <chrono>

#include "../common/log.hpp"

int main() {
    std::packaged_task<int()> task([]() {
        LOG(INFO) << "[worker] starting task..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1)); // simulate work

        LOG(INFO) << "[worker] task done; about to return 42 (writes into the future)" << std::endl;
        int result = 123;

        // !! After return, the thread has not exited yet -- there are 2 more seconds of cleanup.
        // Simulate thread cleanup (destroying locals, releasing resources, etc.).
        // Note: return first writes 42 into the future's shared state, then runs destructors/cleanup.
        // Here we use an RAII object to simulate "there is still work to do after return".
        struct Cleanup {
            ~Cleanup() {
                LOG(INFO) << "[worker] starting cleanup (simulating 2 s)..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(2));
                LOG(INFO) << "[worker] cleanup finished; thread is about to exit" << std::endl;
            }
        } cleanup; // Destruction happens after return, after the future has been written.

        return result; // <- 42 written to the future; f.get() unblocks here.
                       // <- then cleanup is destructed, another 2 s pass, then the thread truly exits.
    });

    std::future<int> future_result = task.get_future();
    std::thread worker_thread(std::move(task));

    LOG(INFO) << "[main] task submitted, waiting for the result..." << std::endl;

    int result = future_result.get(); // Blocks until "return 42" writes into the future.
    LOG(INFO) << "[main] future.get() returned! got result: " << result << std::endl;
    LOG(INFO) << "[main] but the worker has not exited yet; calling join() to wait for it..." << std::endl;

    worker_thread.join(); // Keeps blocking until the worker thread truly exits.
    LOG(INFO) << "[main] join() returned! worker thread has fully exited" << std::endl;

    return 0;
}