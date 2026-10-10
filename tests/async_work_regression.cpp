#include "../include/AsyncWork.h"
#include <cassert>
#include <stdexcept>
#include <thread>

static void Pump(Mus::FrameTaskQueue& queue) {
    auto ready = queue.TakeReady();
    for (auto& callback : ready) callback();
}

int main() {
    // Negative and zero delays run on the next pump; N runs on pump N+1.
    Mus::FrameTaskQueue queue;
    int tick = 0;
    int hits = 0;
    for (int delay : {-1, 0, 1, 3, 100}) {
        queue.Enqueue(static_cast<std::int16_t>(delay), [&, delay] {
            assert(tick == (delay > 0 ? delay : 0) + 1);
            ++hits;
        });
    }
    for (tick = 1; tick <= 101; ++tick) Pump(queue);
    assert(hits == 5);
    assert(queue.TakeReady().empty());

    // Reentrant enqueue must not deadlock or execute in the current snapshot.
    int nested = 0;
    queue.Enqueue(0, [&] { ++nested; queue.Enqueue(0, [&] { ++nested; }); });
    Pump(queue); assert(nested == 1);
    Pump(queue); assert(nested == 2);

    // Concurrent producers and a concurrent consumer: no lost/duplicate work.
    constexpr int producers = 8, perProducer = 2000;
    std::vector<std::atomic<int>> visited(producers * perProducer);
    std::atomic<int> completed = 0;
    std::atomic<bool> done = false;
    std::thread consumer([&] {
        while (!done.load()) { Pump(queue); std::this_thread::yield(); }
    });
    std::vector<std::thread> workers;
    for (int p = 0; p < producers; ++p) {
        workers.emplace_back([&, p] {
            for (int i = 0; i < perProducer; ++i) {
                const int id = p * perProducer + i;
                queue.Enqueue(i % 4, [&, id] { ++visited[id]; ++completed; });
            }
        });
    }
    for (auto& worker : workers) worker.join();
    done = true; consumer.join();
    for (int i = 0; i < 5; ++i) Pump(queue);
    assert(completed == producers * perProducer);
    for (const auto& count : visited) assert(count == 1);

    // Occupied at submission time; retained across copies; released on unwind.
    Mus::SingleFlightGate gate;
    auto ticket = gate.TryAcquire();
    assert(ticket && gate.Busy() && !gate.TryAcquire());
    auto queuedTicket = ticket;
    ticket.reset(); assert(gate.Busy());
    queuedTicket.reset(); assert(!gate.Busy());
    try {
        auto exceptionalTicket = gate.TryAcquire();
        assert(exceptionalTicket);
        throw std::runtime_error("submission failure");
    } catch (const std::runtime_error&) {}
    assert(!gate.Busy());

    // Contention: retain the winner so exactly one submission can acquire.
    std::atomic<int> winners = 0;
    std::vector<std::shared_ptr<Mus::SingleFlightGate::Ticket>> held(16);
    workers.clear();
    for (int i = 0; i < 16; ++i) workers.emplace_back([&, i] {
        held[i] = gate.TryAcquire();
        if (held[i]) ++winners;
    });
    for (auto& worker : workers) worker.join();
    assert(winners == 1);
    held.clear(); assert(!gate.Busy());
}
