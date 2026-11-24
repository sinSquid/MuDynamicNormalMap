#include "../include/FutureBatch.h"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

static thread_local bool failAllocation = false;
void* operator new(std::size_t size) {
    if (failAllocation) { failAllocation = false; throw std::bad_alloc(); }
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

struct Pool {
    bool reject = false;
    std::vector<std::thread> workers;
    ~Pool() { for (auto& worker : workers) worker.join(); }
    template<class Work> std::future<void> submitAsync(Work&& work) {
        if (reject) throw std::runtime_error("submission");
        auto task = std::make_shared<std::packaged_task<void()>>(std::forward<Work>(work));
        auto future = task->get_future();
        workers.emplace_back([task] { (*task)(); });
        return future; // packaged-task future destruction does NOT join a thread.
    }
};
struct BorrowedScope {
    std::atomic<bool>& alive;
    explicit BorrowedScope(std::atomic<bool>& flag) : alive(flag) { alive = true; }
    ~BorrowedScope() { alive = false; }
};

int main() {
    using namespace std::chrono_literals;
    {
        Pool pool;
        std::atomic<bool> finished = false;
        Mus::FutureBatch batch;
        batch.Submit(pool, [] { throw std::runtime_error("first worker"); });
        batch.Submit(pool, [&] { std::this_thread::sleep_for(20ms); finished = true; throw std::runtime_error("last worker"); });
        try { batch.Join(); assert(false); }
        catch (const std::runtime_error& error) {
            assert(std::string(error.what()) == "first worker");
            assert(finished.load()); // First exception must not skip later joins.
        }
    }
    for (const bool rejectSubmission : {false, true}) {
        Pool pool;
        std::atomic<bool> alive = false, observedAlive = false, finished = false;
        try {
            BorrowedScope borrowed(alive); // Captures outlive the batch destructor.
            Mus::FutureBatch batch;
            batch.Submit(pool, [&] {
                std::this_thread::sleep_for(20ms);
                observedAlive = alive.load();
                finished = true;
                throw std::runtime_error("worker during unwind");
            });
            if (rejectSubmission) {
                pool.reject = true;
                batch.Submit(pool, [] {}); // An invalid tracking slot is harmless.
            } else throw std::runtime_error("caller");
            assert(false);
        } catch (const std::runtime_error& error) {
            assert(std::string(error.what()) == (rejectSubmission ? "submission" : "caller"));
            assert(finished.load() && observedAlive.load() && !alive.load());
        }
    }
    // Inject vector allocation failure with a submission service that allocates
    // nothing. If tracking growth fails, no new job may already be published.
    struct ReadyPool {
        std::array<std::future<void>, 64> ready;
        std::size_t submitted = 0;
        ReadyPool() {
            for (auto& future : ready) { std::promise<void> promise; future = promise.get_future(); promise.set_value(); }
        }
        std::future<void> submitAsync(void(*)()) { return std::move(ready[submitted++]); }
    } readyPool;
    Mus::FutureBatch readyBatch;
    readyBatch.Submit(readyPool, +[] {});
    bool allocationRejected = false;
    for (unsigned attempt = 0; attempt != 32 && !allocationRejected; ++attempt) {
        const auto before = readyPool.submitted;
        failAllocation = true;
        try { readyBatch.Submit(readyPool, +[] {}); failAllocation = false; }
        catch (const std::bad_alloc&) {
            failAllocation = false;
            allocationRejected = true;
            assert(readyPool.submitted == before);
        }
    }
    assert(allocationRejected);
    readyBatch.Join();
    std::cout << "PASS: worker/submission/caller failures drain before unwind; tracking allocation precedes dispatch\n";
}
