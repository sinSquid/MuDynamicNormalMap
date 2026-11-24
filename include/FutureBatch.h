#pragma once
#include <exception>
#include <future>
#include <utility>
#include <vector>

namespace Mus {
    // Declare after everything captured by reference in the submitted jobs.
    // No throwing container operation may happen after dispatch but before its
    // future is retained. Submission itself must not throw after publishing work.
    class FutureBatch {
    public:
        FutureBatch() = default;
        FutureBatch(const FutureBatch&) = delete;
        FutureBatch& operator=(const FutureBatch&) = delete;
        ~FutureBatch() noexcept {
            for (auto& job : jobs)
                if (job.valid()) job.wait();
        }

        template <class Pool, class Work>
        void Submit(Pool& pool, Work&& work) {
            jobs.emplace_back(); // Allocate tracking before publishing the callable.
            jobs.back() = pool.submitAsync(std::forward<Work>(work));
        }

        void Join() {
            std::exception_ptr failure;
            for (auto& job : jobs) {
                if (!job.valid()) continue;
                try { job.get(); }
                catch (...) { if (!failure) failure = std::current_exception(); }
            }
            jobs.clear();
            if (failure) std::rethrow_exception(failure);
        }
    private:
        std::vector<std::future<void>> jobs;
    };
}
