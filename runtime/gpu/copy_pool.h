#pragma once
// Copies a renderer puts off: a draw's vertex and index bytes, copied into upload memory by helper threads while the
// render thread goes on to the next draws (a busy scene's frame copies megabytes, one draw's few kilobytes at a time).
//
// The render thread Adds copies, then Closes them as a ticket at the end of each batch; helpers take the copies in
// order and call the done callback with each ticket once it and every earlier one are copied (the render thread then
// tells the title those draws are done: it may rewrite their memory). Wait finishes every copy added so far, the
// calling thread helping: before a command list that reads the upload memory is sent.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace gpu {

class CopyPool {
public:
    // null when XBE_DEFER_COPIES=0 or the CPU has too few cores: copies are then done at once. Never freed (helpers run
    // to the process's end)
    static CopyPool* Make(std::function<void(uint64_t)> done);

    // the render thread only, as are Close and Wait
    void Add(void* dst, const void* src, size_t bytes);
    uint64_t Close();  // the copies added since the last Close; its ticket (done at once if it has none)
    void Wait();

private:
    struct Job {
        void* dst;
        const void* src;
        uint32_t bytes;
        uint32_t ticket;  // its ticket's slot
    };
    struct Ticket {
        std::atomic<uint32_t> left{0};  // copies not yet done
        uint64_t number = 0;
        bool closed = false;
    };
    static constexpr size_t kJobs = 1 << 16;     // copies in flight
    static constexpr size_t kTickets = 1 << 12;  // tickets in flight

    explicit CopyPool(std::function<void(uint64_t)> done, unsigned helpers);
    void Helper();
    bool TakeOne();  // false when no copy is waiting
    void Publish();
    void Advance();

    std::function<void(uint64_t)> done_;
    std::vector<Job> jobs_ = std::vector<Job>(kJobs);
    std::vector<Ticket> tickets_ = std::vector<Ticket>(kTickets);
    std::atomic<uint64_t> tail_{0};      // jobs published (the render thread)
    std::atomic<uint64_t> head_{0};      // jobs taken
    std::atomic<uint64_t> finished_{0};  // jobs done
    uint64_t added_ = 0;                 // jobs added, published at Close
    uint64_t open_ = 1;                  // the ticket being added to
    uint64_t done_ticket_ = 0;           // every ticket up to it done (under sh_->m)
    std::atomic<uint64_t> done_seen_{0}; // the same, read without the lock
    struct Shared;
    Shared* sh_;
};

}  // namespace gpu
