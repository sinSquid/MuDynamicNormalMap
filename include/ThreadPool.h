#pragma once 
#include <tbb/task_arena.h>
#include <tbb/task_scheduler_observer.h>

namespace Mus {
    class TBB_CoreMasking : public tbb::task_scheduler_observer {
        struct AffinityFrame {
            const TBB_CoreMasking* owner;
            DWORD_PTR mask;
            std::size_t depth = 1;
        };
        struct ThreadAffinity {
            HANDLE thread;
            DWORD_PTR original;
            std::vector<AffinityFrame> frames;
            ThreadAffinity(HANDLE handle, DWORD_PTR previous) : thread(handle), original(previous) {}
            ~ThreadAffinity() { CloseHandle(thread); }
            ThreadAffinity(const ThreadAffinity&) = delete;
            ThreadAffinity& operator=(const ThreadAffinity&) = delete;
        };
        DWORD_PTR mask;
        // Arenas share TBB workers. A retiring outer arena must not restore over
        // another arena's active mask or become that arena's later restore target.
        struct AffinityRegistry {
            std::mutex mutex;
            std::unordered_map<DWORD, ThreadAffinity> threads;
        };
        static std::shared_ptr<AffinityRegistry> SharedRegistry() {
            static auto registry = std::make_shared<AffinityRegistry>();
            return registry;
        }
        // Keep registry storage alive even if its function-static owner is
        // destroyed before the plugin's global thread-pool shared_ptrs.
        std::shared_ptr<AffinityRegistry> affinityState;
        static DWORD_PTR ActiveMask(const ThreadAffinity& state) {
            for (auto it = state.frames.rbegin(); it != state.frames.rend(); ++it)
                if (it->mask) return it->mask;
            return state.original;
        }
        static void RestoreIfOwned(ThreadAffinity& state, DWORD_PTR previous) {
            const auto target = ActiveMask(state);
            if (target == previous) return;
            GROUP_AFFINITY current{};
            if (GetThreadGroupAffinity(state.thread, &current) && current.Mask == previous)
                SetThreadAffinityMask(state.thread, target);
        }

    public:
        TBB_CoreMasking(tbb::task_arena& arena, DWORD_PTR m)
            : tbb::task_scheduler_observer(arena), mask(m), affinityState(SharedRegistry()) {
            observe(true);
        }
        ~TBB_CoreMasking() override {
            observe(false); // Drain callbacks before touching their shared state.
            std::lock_guard lock(affinityState->mutex);
            auto& savedAffinity = affinityState->threads;
            for (auto it = savedAffinity.begin(); it != savedAffinity.end();) {
                auto& state = it->second;
                const auto previous = ActiveMask(state);
                if (std::erase_if(state.frames, [this](const auto& frame) { return frame.owner == this; }))
                    RestoreIfOwned(state, previous);
                if (state.frames.empty()) it = savedAffinity.erase(it);
                else ++it;
            }
        }

        void on_scheduler_entry(bool worker) override {
            if (!worker) return;
            HANDLE unownedHandle = nullptr;
            try {
                std::lock_guard lock(affinityState->mutex);
                auto& savedAffinity = affinityState->threads;
                const auto id = GetCurrentThreadId();
                auto found = savedAffinity.find(id);
                if (found != savedAffinity.end() && WaitForSingleObject(found->second.thread, 0) == WAIT_OBJECT_0) {
                    savedAffinity.erase(found); // A terminated worker's ID was reused.
                    found = savedAffinity.end();
                }
                if (found == savedAffinity.end()) {
                    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &unownedHandle, THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, 0)) return;
                    GROUP_AFFINITY current{};
                    if (!GetThreadGroupAffinity(unownedHandle, &current)) {
                        CloseHandle(unownedHandle);
                        return;
                    }
                    found = savedAffinity.try_emplace(id, unownedHandle, current.Mask).first;
                    unownedHandle = nullptr;
                }
                auto& state = found->second;
                if (!state.frames.empty() && state.frames.back().owner == this) {
                    ++state.frames.back().depth;
                    return;
                }
                // Allocate before changing affinity. A failed allocation leaves
                // a different/no top owner, so its matching exit is a no-op.
                state.frames.push_back({this, 0});
                const auto previous = SetThreadAffinityMask(GetCurrentThread(), mask);
                state.frames.back().mask = previous ? mask : 0;
                if (previous && state.frames.size() == 1) state.original = previous;
            } catch (...) {
                if (unownedHandle) CloseHandle(unownedHandle);
            }
        }
        void on_scheduler_exit(bool worker) override {
            if (!worker) return;
            std::lock_guard lock(affinityState->mutex);
            auto& savedAffinity = affinityState->threads;
            const auto found = savedAffinity.find(GetCurrentThreadId());
            if (found == savedAffinity.end()) return;
            auto& state = found->second;
            if (!state.frames.empty() && state.frames.back().owner == this) {
                if (--state.frames.back().depth) return;
                const auto previous = ActiveMask(state);
                state.frames.pop_back();
                RestoreIfOwned(state, previous);
            }
            if (state.frames.empty()) savedAffinity.erase(found);
        }
    };

    class TBB_ThreadPool
    {
    public:
        TBB_ThreadPool() = delete;
        TBB_ThreadPool(std::uint32_t a_threadSize, std::uint64_t a_coreMask)
            : workers(std::make_unique<tbb::task_arena>(a_threadSize)) {
            if (a_coreMask != 0)
                observer = std::make_unique<TBB_CoreMasking>(*workers, a_coreMask);
        }

        template <typename F>
        void Execute(F&& f) {
            workers->execute(std::forward<F>(f));
        }

        template <typename F>
        void Enqueue(F&& f) {
            workers->enqueue(std::forward<F>(f));
        }

        std::int32_t GetThreadSize() const { return workers->max_concurrency(); }
    private:
        std::unique_ptr<tbb::task_arena> workers;
        std::unique_ptr<TBB_CoreMasking> observer;
    };
    extern std::atomic<std::shared_ptr<TBB_ThreadPool>> currentProcessingThreads;
    extern std::shared_ptr<TBB_ThreadPool> processingThreads;
    extern std::shared_ptr<TBB_ThreadPool> processingThreadsFull;

    class ThreadPool_ParallelModule
    {
    public:
        ThreadPool_ParallelModule() = delete;
        ThreadPool_ParallelModule(std::uint32_t threadSize, std::uint64_t a_coreMask);
        ~ThreadPool_ParallelModule();

        template<typename F, typename... Args>
        auto submitAsync(F&& f, Args&&... args)
            -> std::future<std::invoke_result_t<F, Args...>>
        {
            using RetType = std::invoke_result_t<F, Args...>;
            auto task = std::make_shared<std::packaged_task<RetType()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...)
            );
            std::future<RetType> res = task->get_future();
            {
                std::unique_lock lock(queueMutex);
                tasks.emplace([task]() { (*task)(); });
            }
            cv.notify_one();
            return res;
        }

        inline std::size_t GetThreads() const { return workers.size(); };
        inline std::int32_t GetThreadIndex(const std::uint64_t threadID) const {
            const auto it = std::find(threadIDs.cbegin(), threadIDs.cend(), threadID);
            return it != threadIDs.cend() ? std::distance(threadIDs.cbegin(), it) : -1;
        };
    private:
        const std::uint64_t coreMask = 0;

        std::vector<std::thread> workers;
        std::vector<std::uint64_t> threadIDs;
        std::queue<std::function<void()>> tasks;
        std::mutex queueMutex;
        std::condition_variable cv;
        std::atomic<bool> stop;

        void workerLoop();
    };
    extern std::atomic<std::shared_ptr<ThreadPool_ParallelModule>> currentActorThreads;
    extern std::shared_ptr<ThreadPool_ParallelModule> actorThreads;
    extern std::shared_ptr<ThreadPool_ParallelModule> actorThreadsFull;

    extern std::unique_ptr<ThreadPool_ParallelModule> backGroundWorkerThreads;

    class ThreadPool_GPUTaskModule
        : public IEventListener<FrameEvent>
    {
    public:
        ThreadPool_GPUTaskModule() = delete;
        ThreadPool_GPUTaskModule(std::uint32_t a_threadSize, std::uint64_t a_coreMask, std::clock_t a_taskQTick, bool a_directTaskQ);
        ~ThreadPool_GPUTaskModule();
        void BeginDrain();

        template<typename F, typename... Args>
        auto submitAsync(F&& f, Args&&... args)
            -> std::future<std::invoke_result_t<F, Args...>>
        {
            using RetType = std::invoke_result_t<F, Args...>;
            auto task = std::make_shared<std::packaged_task<RetType()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...)
            );
            std::future<RetType> res = task->get_future();
            {
                std::unique_lock lock(queueMutex);
                tasks.emplace([task]() { (*task)(); });
            }
            cv.notify_one();
            return res;
        }

        inline std::size_t GetThreads() const { return workers.size(); };
    protected:
        void onEvent(const FrameEvent& e) override;

    private:
        const std::uint64_t coreMask = 0;

        std::vector<std::thread> workers;
        std::queue<std::function<void()>> tasks;
        std::mutex queueMutex;
        std::condition_variable cv;
        std::atomic<bool> stop;

        const std::clock_t taskQTick = 0;
        const bool directTaskQ = false;
        bool draining = false; // Protected by queueMutex; quit ignores frame pacing.

        std::vector<std::clock_t> lastTickTime;
        std::vector<bool> runTask;
        std::vector<bool> running;

        void workerLoop(std::uint32_t threadNum);
    };
    extern std::unique_ptr<ThreadPool_GPUTaskModule> gpuTask;
}
