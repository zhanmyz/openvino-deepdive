#include <future>
#include <thread>
#include <chrono>

#include "../common/log.hpp"

int main() {
    std::promise<int> promise;  // manually control when the value is written
    std::future<int> future_result = promise.get_future();

    std::thread worker_thread([p = std::move(promise)]() mutable {
        LOG(INFO) << "[worker] starting task..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));

        LOG(INFO) << "[worker] manually writing result 123 into the future!" << std::endl;
        p.set_value(123);  // <- the write here immediately unblocks the main thread

        // The thread keeps running for a while after the write!
        LOG(INFO) << "[worker] result written, but I still have 2 s of work before exiting..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        LOG(INFO) << "[worker] cleanup finished; thread is about to exit" << std::endl;
    });

    LOG(INFO) << "[main] task submitted, waiting for the result..." << std::endl;

    int result = future_result.get();
    LOG(INFO) << "[main] future.get() returned! got result: " << result << std::endl;
    LOG(INFO) << "[main] but the worker has not exited yet; calling join() to wait for it..." << std::endl;

    worker_thread.join();
    LOG(INFO) << "[main] join() returned! worker thread has fully exited" << std::endl;

    return 0;
}
