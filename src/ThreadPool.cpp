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

    ThreadPool_GPUTaskModule::ThreadPool_GPUTaskModule(std::uint32_t a_threadSize, std::uint64_t a_coreMask, std::clock_t a_taskQTick, bool a_directTaskQ)
        : coreMask(a_coreMask)
        , stop(false), taskQTick(a_taskQTick)
        , directTaskQ(a_directTaskQ)
    {
        std::uint32_t threadCount = std::max(1u, a_threadSize);
        lastTickTime.resize(threadCount, currentTime.load());
        runTask.resize(threadCount, true);
        running.resize(threadCount, false);
        for (std::uint32_t i = 0; i < threadCount; i++)
            workers.emplace_back([this, i] { workerLoop(i); });
        g_frameEventDispatcher.addListener(this);
    }

    ThreadPool_GPUTaskModule::~ThreadPool_GPUTaskModule() {
        {
            std::lock_guard lock(queueMutex);
            stop.store(true);
        }
        cv.notify_all();
        for (auto& t : workers)
            if (t.joinable()) t.join();
        g_frameEventDispatcher.removeListener(this);
    }

    void ThreadPool_GPUTaskModule::BeginDrain() {
        {
            std::lock_guard lock(queueMutex);
            draining = true;
        }
        // Actor computations may still submit follow-up GPU work while draining.
        cv.notify_all();
    }

    void ThreadPool_GPUTaskModule::workerLoop(std::uint32_t threadNum) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        if (coreMask != 0)
            SetThreadAffinityMask(GetCurrentThread(), coreMask);
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(queueMutex);
                cv.wait(lock, [this, threadNum] {
                    return stop.load() || (!tasks.empty() && (draining || runTask[threadNum] || directTaskQ));
                });
                runTask[threadNum] = false;
                running[threadNum] = true;
                if (stop.load() && tasks.empty())
                    return;
                task = std::move(tasks.front());
                tasks.pop();
            }
            task();
            {
                std::lock_guard lock(queueMutex);
                lastTickTime[threadNum] = currentTime.load();
                running[threadNum] = false;
            }
        }
    }

    void ThreadPool_GPUTaskModule::onEvent(const FrameEvent& e)
    {
        if (directTaskQ) return;
        std::lock_guard lock(queueMutex);
        if (tasks.empty()) return;
        std::uint32_t notified = 0;
        for (std::uint32_t i = 0; i < GetThreads(); i++)
        {
            if (running[i])
                continue;
            if (!directTaskQ) 
            {
                if (lastTickTime[i] + taskQTick > currentTime)
                    continue;
            }
            runTask[i] = true;
            notified++;
        }
        if (notified == 0)
            return;
        cv.notify_all();
    }
}
