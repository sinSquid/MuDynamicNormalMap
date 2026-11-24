#include "ThreadPool.h"

namespace Mus
{
    std::atomic<std::shared_ptr<TBB_ThreadPool>> currentProcessingThreads;
    std::shared_ptr<TBB_ThreadPool> processingThreads;
    std::shared_ptr<TBB_ThreadPool> processingThreadsFull;

    std::atomic<std::shared_ptr<ThreadPool_ParallelModule>> currentActorThreads;
    std::shared_ptr<ThreadPool_ParallelModule> actorThreads;
    std::shared_ptr<ThreadPool_ParallelModule> actorThreadsFull;

    std::unique_ptr<ThreadPool_ParallelModule> backGroundWorkerThreads;

    ThreadPool_ParallelModule::ThreadPool_ParallelModule(std::uint32_t threadSize, std::uint64_t a_coreMask)
        : coreMask(a_coreMask), stop(false)
    {
        std::uint32_t coreCount = std::max(1u, threadSize);
        threadIDs.resize(coreCount);
        for (std::uint32_t i = 0; i < coreCount; i++) {
            workers.emplace_back([this, i] {
                threadIDs[i] = GetCurrentThreadId();
                workerLoop();
            });
        }
    }

    ThreadPool_ParallelModule::~ThreadPool_ParallelModule() {
        {
            // Publish stop under the wait predicate's mutex to avoid a lost wake.
            std::lock_guard lock(queueMutex);
            stop.store(true);
        }
        cv.notify_all();
        for (auto& t : workers)
            if (t.joinable()) t.join();
    }

    void ThreadPool_ParallelModule::workerLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        if (coreMask != 0)
            SetThreadAffinityMask(GetCurrentThread(), coreMask);
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(queueMutex);
                cv.wait(lock, [this] {
                    return stop.load() || !tasks.empty();
                });
                if (stop.load() && tasks.empty())
                    return;
                task = std::move(tasks.front());
                tasks.pop();
            }
            task();
        }
    }

    std::unique_ptr<ThreadPool_GPUTaskModule> gpuTask;

    ThreadPool_GPUTaskModule::ThreadPool_GPUTaskModule(std::uint32_t a_threadSize, std::uint64_t a_coreMask, std::chrono::milliseconds interval, bool a_directTaskQ)
        : coreMask(a_coreMask)
        , stop(false), cooldown(interval)
        , directTaskQ(a_directTaskQ)
    {
        std::uint32_t threadCount = std::max(1u, a_threadSize);
        try {
            workers.reserve(threadCount);
            for (std::uint32_t i = 0; i < threadCount; ++i)
                workers.emplace_back([this] { workerLoop(); });
        } catch (...) {
            {
                std::lock_guard lock(queueMutex);
                stop.store(true);
            }
            cv.notify_all();
            for (auto& worker : workers) worker.join();
            throw;
        }
    }

    ThreadPool_GPUTaskModule::~ThreadPool_GPUTaskModule() {
        {
            std::lock_guard lock(queueMutex);
            stop.store(true);
        }
        cv.notify_all();
        for (auto& t : workers)
            if (t.joinable()) t.join();
    }

    void ThreadPool_GPUTaskModule::BeginDrain() {
        {
            std::lock_guard lock(queueMutex);
            draining = true;
        }
        // Actor computations may still submit follow-up GPU work while draining.
        cv.notify_all();
    }

    void ThreadPool_GPUTaskModule::workerLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        if (coreMask != 0)
            SetThreadAffinityMask(GetCurrentThread(), coreMask);
        using Clock = std::chrono::steady_clock;
        auto readyAt = Clock::now();
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(queueMutex);
                cv.wait(lock, [this] { return stop.load() || !tasks.empty(); });
                if (!directTaskQ && !draining && !stop.load())
                    cv.wait_until(lock, readyAt, [this] { return draining || stop.load(); });
                // Another worker can consume the last queued job while we cool down.
                if (tasks.empty()) {
                    if (stop.load()) return;
                    continue;
                }
                task = std::move(tasks.front());
                tasks.pop();
            }
            task();
            readyAt = Clock::now() + cooldown;
        }
    }
}
