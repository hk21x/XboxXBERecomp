#include "gpu/copy_pool.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <immintrin.h>

#include "core/log.h"

namespace gpu {

namespace {
constexpr size_t kPiece = 64 * 1024;  // a long copy split into pieces this size, shared out
constexpr int kSpin = 4000;           // a helper with nothing to do checks this many times (some tens of us) before it sleeps
}  // namespace

struct CopyPool::Shared {
    std::mutex m;  // tickets' done order, sleeping helpers
    std::condition_variable cv;
    int sleeping = 0;
};

CopyPool* CopyPool::Make(std::function<void(uint64_t)> done) {
    const char* e = std::getenv("XBE_DEFER_COPIES");
    unsigned hw = std::thread::hardware_concurrency();
    // the title's and render threads keep theirs; at most 3 helpers
    unsigned n = hw > 3 ? std::min(hw - 2, 3u) : 0;
    if (const char* t = std::getenv("XBE_COPY_THREADS")) n = unsigned(std::strtoul(t, nullptr, 10));
    if ((e && *e == '0') || !n) {
        LOGR("gpu: vertex and index copies done at once (%s)", n ? "XBE_DEFER_COPIES=0" : "no helper threads");
        return nullptr;
    }
    LOGR("gpu: vertex and index copies done by %u helper threads", n);
    return new CopyPool(std::move(done), n);
}

CopyPool::CopyPool(std::function<void(uint64_t)> done, unsigned helpers) : done_(std::move(done)), sh_(new Shared) {
    for (unsigned i = 0; i < helpers; i++) std::thread([this] { Helper(); }).detach();
}

// the render thread: the open ticket's copies added so far made visible to the helpers
void CopyPool::Publish() {
    uint64_t t = tail_.load(std::memory_order_relaxed);
    if (t == added_) return;
    tickets_[open_ % kTickets].left.fetch_add(uint32_t(added_ - t), std::memory_order_relaxed);
    tail_.store(added_, std::memory_order_release);
}

void CopyPool::Add(void* dst, const void* src, size_t bytes) {
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    uint32_t slot = uint32_t(open_ % kTickets);
    while (bytes) {
        // the ring full: help until a slot is free (a helper copies its job out as soon as it takes it; the slot is
        // reused a ring's length of copies later)
        while (added_ - finished_.load(std::memory_order_acquire) >= kJobs) {
            Publish();
            if (!TakeOne()) std::this_thread::yield();
        }
        uint32_t n = uint32_t(std::min(bytes, kPiece));
        jobs_[added_ % kJobs] = {d, s, n, slot};
        added_++;
        d += n, s += n, bytes -= n;
    }
}

uint64_t CopyPool::Close() {
    uint64_t t = open_;
    uint32_t slot = uint32_t(t % kTickets);
    {
        std::lock_guard lock(sh_->m);
        bool any = tail_.load(std::memory_order_relaxed) != added_;
        Publish();
        Ticket& k = tickets_[slot];
        k.number = t;
        k.closed = true;
        if (k.left.load(std::memory_order_acquire) == 0) Advance();  // nothing in it, or all already copied
        if (sh_->sleeping && any) sh_->cv.notify_all();
    }
    open_++;
    // a ticket's slot is reused only once that ticket is done
    while (open_ > kTickets && done_seen_.load(std::memory_order_acquire) < open_ - kTickets)
        if (!TakeOne()) std::this_thread::yield();
    return t;
}

void CopyPool::Wait() {
    if (tail_.load(std::memory_order_relaxed) != added_) {
        std::lock_guard lock(sh_->m);
        Publish();
        if (sh_->sleeping) sh_->cv.notify_all();
    }
    while (finished_.load(std::memory_order_acquire) < added_)
        if (!TakeOne()) _mm_pause();
}

bool CopyPool::TakeOne() {
    uint64_t h = head_.load(std::memory_order_relaxed);
    for (;;) {
        if (h >= tail_.load(std::memory_order_acquire)) return false;
        if (head_.compare_exchange_weak(h, h + 1, std::memory_order_acq_rel)) break;
    }
    Job j = jobs_[h % kJobs];
    std::memcpy(j.dst, j.src, j.bytes);
    if (tickets_[j.ticket].left.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard lock(sh_->m);
        if (tickets_[j.ticket].closed) Advance();
    }
    finished_.fetch_add(1, std::memory_order_release);
    return true;
}

// under sh_->m: the tickets done in order from the oldest, and the callback told the newest
void CopyPool::Advance() {
    uint64_t was = done_ticket_;
    for (;;) {
        Ticket& k = tickets_[(done_ticket_ + 1) % kTickets];
        if (!k.closed || k.number != done_ticket_ + 1 || k.left.load(std::memory_order_acquire)) break;
        k.closed = false;
        done_ticket_++;
    }
    if (done_ticket_ != was) {
        done_seen_.store(done_ticket_, std::memory_order_release);
        done_(done_ticket_);
    }
}

void CopyPool::Helper() {
    for (;;) {
        if (TakeOne()) continue;
        bool got = false;
        for (int i = 0; i < kSpin && !got; i++) {
            _mm_pause();
            got = head_.load(std::memory_order_relaxed) < tail_.load(std::memory_order_acquire);
        }
        if (got) continue;
        std::unique_lock lock(sh_->m);
        sh_->sleeping++;
        sh_->cv.wait(lock, [&] { return head_.load(std::memory_order_relaxed) < tail_.load(std::memory_order_acquire); });
        sh_->sleeping--;
    }
}

}  // namespace gpu
