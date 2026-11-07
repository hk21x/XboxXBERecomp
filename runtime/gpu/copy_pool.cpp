#include "gpu/copy_pool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "core/log.h"

namespace gpu {

namespace {
constexpr size_t kPiece = 64 * 1024;       // a long copy split into pieces this size, shared out
constexpr size_t kInline = 256 * 1024;     // fewer bytes than this pending: copied on the calling thread
}  // namespace

struct CopyPool::Shared {
    std::mutex m;
    std::condition_variable cv;
    uint64_t gen = 0;        // a new Run's work
    bool stop = false;
    std::atomic<size_t> next{0};
    std::atomic<size_t> left{0};
    std::atomic<int> running{0};  // workers inside a Run's work
    const Job* jobs = nullptr;
    std::atomic<size_t> count{0};
    std::vector<std::thread> threads;
};

CopyPool::CopyPool() : sh_(new Shared) {
    const char* e = std::getenv("XBE_DEFER_COPIES");
    on_ = !(e && *e == '0');
    unsigned hw = std::thread::hardware_concurrency();
    // the title's and render threads keep theirs; at most 4 helpers
    unsigned n = hw > 3 ? std::min(hw - 2, 4u) : 0;
    if (const char* t = std::getenv("XBE_COPY_THREADS")) n = unsigned(std::strtoul(t, nullptr, 10));
    if (!on_) n = 0;
    LOGR("gpu: vertex copies %s, %u helper threads", on_ ? "put off and done together" : "done at once (XBE_DEFER_COPIES=0)", n);
    for (unsigned i = 0; i < n; i++) sh_->threads.emplace_back([this] { Work(); });
}

CopyPool::~CopyPool() {
    {
        std::lock_guard lock(sh_->m);
        sh_->stop = true;
    }
    sh_->cv.notify_all();
    for (auto& t : sh_->threads) t.join();
    delete sh_;
}

void CopyPool::Add(void* dst, const void* src, size_t bytes) {
    if (!bytes) return;
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    bytes_ += bytes;
    while (bytes > kPiece) {
        jobs_.push_back({d, s, kPiece});
        d += kPiece, s += kPiece, bytes -= kPiece;
    }
    jobs_.push_back({d, s, bytes});
}

// takes jobs until none are left
void CopyPool::Take() {
    for (;;) {
        size_t i = sh_->next.fetch_add(1, std::memory_order_relaxed);
        if (i >= sh_->count) return;
        const Job& j = sh_->jobs[i];
        std::memcpy(j.dst, j.src, j.bytes);
        sh_->left.fetch_sub(1, std::memory_order_release);
    }
}

void CopyPool::Work() {
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock lock(sh_->m);
            sh_->cv.wait(lock, [&] { return sh_->stop || sh_->gen != seen; });
            if (sh_->stop) return;
            seen = sh_->gen;
            if (!sh_->count) continue;  // woken after that Run ended
            sh_->running.fetch_add(1, std::memory_order_acquire);
        }
        Take();
        sh_->running.fetch_sub(1, std::memory_order_release);
    }
}

void CopyPool::Run() {
    if (jobs_.empty()) return;
    if (bytes_ < kInline || sh_->threads.empty()) {
        for (const Job& j : jobs_) std::memcpy(j.dst, j.src, j.bytes);
    } else {
        {
            std::lock_guard lock(sh_->m);
            sh_->jobs = jobs_.data();
            sh_->count = jobs_.size();
            sh_->next.store(0, std::memory_order_relaxed);
            sh_->left.store(jobs_.size(), std::memory_order_relaxed);
            sh_->gen++;
        }
        sh_->cv.notify_all();
        Take();
        while (sh_->left.load(std::memory_order_acquire)) std::this_thread::yield();
        // no worker starts on this Run once count is 0; one that started is waited for (it still reads next)
        {
            std::lock_guard lock(sh_->m);
            sh_->count = 0;
        }
        while (sh_->running.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    jobs_.clear();
    bytes_ = 0;
}

}  // namespace gpu
