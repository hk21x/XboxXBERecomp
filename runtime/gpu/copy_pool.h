#pragma once
// Copies a renderer may put off until its work is sent: the vertex bytes a draw reads from guest memory, copied into
// upload memory. Done in Run, together and across several threads (a busy scene's frame copies about 25 MB, one
// draw's few kilobytes at a time). A caller runs them before the GPU reads the upload memory, and before the title
// is told the draws are done (the title may then rewrite the guest memory).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gpu {

class CopyPool {
public:
    CopyPool();
    ~CopyPool();
    // false when XBE_DEFER_COPIES=0: callers copy at once
    bool On() const { return on_; }
    void Add(void* dst, const void* src, size_t bytes);
    size_t Pending() const { return bytes_; }
    // every copy added so far, done before it returns
    void Run();

private:
    struct Job {
        void* dst;
        const void* src;
        size_t bytes;
    };
    void Work();
    void Take();
    std::vector<Job> jobs_;
    size_t bytes_ = 0;
    bool on_ = true;
    struct Shared;
    Shared* sh_;
};

}  // namespace gpu
