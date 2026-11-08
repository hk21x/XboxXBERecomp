#pragma once
// A draw's commands as the D3D12 backend decided them (docs/render-parallel.md, stage A): written here in order
// instead of to the command list, then replayed into a list. Today the render thread replays them itself, before any
// command it writes to the list directly; stage B replays runs of them on helper threads, each into a list of its
// own. Everything a command points at (resources, pipelines) lives at least until the list that replays it has run.
// Included by d3d12.cpp only, after <d3d12.h>.
#include <cstdint>
#include <cstring>
#include <vector>

namespace gpu {

class D3D12Rec {
public:
    enum Op : uint8_t {
        kPso, kRootCbv, kRootTable, kTopology, kVertexBuffers, kIndexBuffer, kDraw, kDrawIndexed, kViewport, kScissor,
        kBlendFactor, kStencilRef, kTargets, kBarrier, kSrv, kMark,
    };

    bool Empty() const { return bytes_.empty(); }
    // another recorder's commands after these, in order; it is left empty
    void Append(D3D12Rec& o) {
        if (o.bytes_.empty()) return;
        Pad();
        bytes_.insert(bytes_.end(), o.bytes_.begin(), o.bytes_.end());
        draws_ += o.draws_;
        o.bytes_.clear();
        o.draws_ = 0;
    }
    uint32_t Draws() const { return draws_; }

    void Pso(ID3D12PipelineState* p) { Put(kPso, p); }
    void RootCbv(uint32_t i, D3D12_GPU_VIRTUAL_ADDRESS va) { Put(kRootCbv, Pair<D3D12_GPU_VIRTUAL_ADDRESS>{i, va}); }
    void RootTable(uint32_t i, D3D12_GPU_DESCRIPTOR_HANDLE h) { Put(kRootTable, Pair<D3D12_GPU_DESCRIPTOR_HANDLE>{i, h}); }
    void Topology(D3D_PRIMITIVE_TOPOLOGY t) { Put(kTopology, t); }
    void VertexBuffers(uint32_t n, const D3D12_VERTEX_BUFFER_VIEW* v) {
        Head(kVertexBuffers);
        Raw(&n, sizeof n);
        Raw(v, sizeof(*v) * n);
    }
    void IndexBuffer(const D3D12_INDEX_BUFFER_VIEW& v) { Put(kIndexBuffer, v); }
    void Draw(uint32_t count) {
        Put(kDraw, count);
        draws_++;
    }
    void DrawIndexed(uint32_t count, int32_t base) {
        Put(kDrawIndexed, Pair<int32_t>{count, base});
        draws_++;
    }
    void Viewport(const D3D12_VIEWPORT& v) { Put(kViewport, v); }
    void Scissor(const D3D12_RECT& r) { Put(kScissor, r); }
    void BlendFactor(const float* f) {
        Head(kBlendFactor);
        Raw(f, 16);
    }
    void StencilRef(uint32_t r) { Put(kStencilRef, r); }
    void Targets(const D3D12_CPU_DESCRIPTOR_HANDLE* rt, const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
        TargetsArgs t{rt != nullptr, ds != nullptr, rt ? *rt : D3D12_CPU_DESCRIPTOR_HANDLE{}, ds ? *ds : D3D12_CPU_DESCRIPTOR_HANDLE{}};
        Put(kTargets, t);
    }
    void Barrier(const D3D12_RESOURCE_BARRIER& b) { Put(kBarrier, b); }
    // a view written into a shader visible slot: the GPU reads it only once the list has been sent
    void Srv(ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC& d, D3D12_CPU_DESCRIPTOR_HANDLE at) {
        Put(kSrv, SrvArgs{r, d, at});
    }
    void Mark(uint64_t v) { Put(kMark, v); }

    // every command so far into cl, in order; mark: Aftermath's marker call (kMark)
    template <typename MarkFn>
    void Replay(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, MarkFn mark) {
        const uint8_t* p = bytes_.data();
        const uint8_t* end = p + bytes_.size();
        while (p < end) {
            Op op = Op(*p);
            p += kAlign;
            switch (op) {
                case kPso: cl->SetPipelineState(Get<ID3D12PipelineState*>(p)); break;
                case kRootCbv: {
                    auto a = Get<Pair<D3D12_GPU_VIRTUAL_ADDRESS>>(p);
                    cl->SetGraphicsRootConstantBufferView(a.i, a.v);
                    break;
                }
                case kRootTable: {
                    auto a = Get<Pair<D3D12_GPU_DESCRIPTOR_HANDLE>>(p);
                    cl->SetGraphicsRootDescriptorTable(a.i, a.v);
                    break;
                }
                case kTopology: cl->IASetPrimitiveTopology(Get<D3D_PRIMITIVE_TOPOLOGY>(p)); break;
                case kVertexBuffers: {
                    uint32_t n;
                    std::memcpy(&n, p, sizeof n);
                    D3D12_VERTEX_BUFFER_VIEW v[16];
                    std::memcpy(v, p + sizeof n, sizeof(v[0]) * n);
                    p += Size(sizeof n + sizeof(v[0]) * n);
                    cl->IASetVertexBuffers(0, n, v);
                    break;
                }
                case kIndexBuffer: {
                    auto v = Get<D3D12_INDEX_BUFFER_VIEW>(p);
                    cl->IASetIndexBuffer(&v);
                    break;
                }
                case kDraw: cl->DrawInstanced(Get<uint32_t>(p), 1, 0, 0); break;
                case kDrawIndexed: {
                    auto a = Get<Pair<int32_t>>(p);
                    cl->DrawIndexedInstanced(a.i, 1, 0, a.v, 0);
                    break;
                }
                case kViewport: {
                    auto v = Get<D3D12_VIEWPORT>(p);
                    cl->RSSetViewports(1, &v);
                    break;
                }
                case kScissor: {
                    auto r = Get<D3D12_RECT>(p);
                    cl->RSSetScissorRects(1, &r);
                    break;
                }
                case kBlendFactor: {
                    float f[4];
                    std::memcpy(f, p, 16);
                    p += Size(16);
                    cl->OMSetBlendFactor(f);
                    break;
                }
                case kStencilRef: cl->OMSetStencilRef(Get<uint32_t>(p)); break;
                case kTargets: {
                    auto t = Get<TargetsArgs>(p);
                    cl->OMSetRenderTargets(t.has_rt ? 1 : 0, t.has_rt ? &t.rt : nullptr, FALSE, t.has_ds ? &t.ds : nullptr);
                    break;
                }
                case kBarrier: {
                    auto b = Get<D3D12_RESOURCE_BARRIER>(p);
                    cl->ResourceBarrier(1, &b);
                    break;
                }
                case kSrv: {
                    auto a = Get<SrvArgs>(p);
                    dev->CreateShaderResourceView(a.res, &a.desc, a.at);
                    break;
                }
                case kMark: mark(Get<uint64_t>(p)); break;
            }
        }
        bytes_.clear();
        draws_ = 0;
    }
    D3D12Rec() { bytes_.reserve(1 << 20); }

private:
    static constexpr size_t kAlign = 8;
    template <typename V>
    struct Pair {
        uint32_t i;
        V v;
    };
    struct TargetsArgs {
        bool has_rt, has_ds;
        D3D12_CPU_DESCRIPTOR_HANDLE rt, ds;
    };
    struct SrvArgs {
        ID3D12Resource* res;
        D3D12_SHADER_RESOURCE_VIEW_DESC desc;
        D3D12_CPU_DESCRIPTOR_HANDLE at;
    };
    static size_t Size(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }
    void Head(Op op) {
        Pad();
        size_t o = bytes_.size();
        bytes_.resize(o + kAlign);
        bytes_[o] = op;
    }
    void Raw(const void* v, size_t n) {
        size_t o = bytes_.size();
        bytes_.resize(o + n);
        std::memcpy(bytes_.data() + o, v, n);
    }
    // a command's arguments end on kAlign bytes: Raw parts are padded by the next Head or Put
    template <typename T>
    void Put(Op op, const T& v) {
        Pad();
        size_t o = bytes_.size();
        bytes_.resize(o + kAlign + Size(sizeof(T)));
        bytes_[o] = op;
        std::memcpy(bytes_.data() + o + kAlign, &v, sizeof(T));
    }
    void Pad() { bytes_.resize(Size(bytes_.size())); }
    template <typename T>
    static T Get(const uint8_t*& p) {
        T v;
        std::memcpy(&v, p, sizeof(T));
        p += Size(sizeof(T));
        return v;
    }
    std::vector<uint8_t> bytes_;
    uint32_t draws_ = 0;
};

}  // namespace gpu
