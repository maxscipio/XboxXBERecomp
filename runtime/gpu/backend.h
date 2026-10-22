#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

#include "gpu/state.h"

namespace gpu {

// D3DCLEAR flags as the XDK defines them
enum : uint32_t {
    kClearTarget = 0xF0,  // ARGB channels
    kClearZ = 0x01,
    kClearStencil = 0x02,
};

// The video overlay (D3DDevice_EnableOverlay, D3DDevice_UpdateOverlay): a surface the console's video scaler showed
// over the frame at scan-out, where titles play movies (Bink in YUY2). Rectangles are left, top, right, bottom; the
// source in surface pixels, the destination in the frame's (back buffer's) pixels.
struct Overlay {
    bool enabled = false;
    Surface surface;
    int32_t src[4] = {};
    int32_t dst[4] = {};
    bool color_key = false;  // show the overlay only where the frame holds key
    uint32_t key = 0;
};

class Backend {
public:
    virtual ~Backend() = default;
    // the title's back buffer size, the output window may differ
    virtual bool Init(uint32_t width, uint32_t height) = 0;
    // rects: count D3DRECTs (4 x int32), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices, null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    // src_kx, src_ky: the source's guest pixels per pixel of its image, above 1 where the console's frame buffer is
    // multisampled or drawn scaled down
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va, float src_kx, float src_ky) = 0;
    // show false: the frame is finished as usual but not put on screen (a title shown at half its step rate)
    virtual void Present(const Surface& front, bool show) = 0;
    // the CPU is about to touch this resource's memory: write GPU results back first. size: the surface's bytes in
    // guest memory, 0 when unknown; a read-back that would write more than size is refused
    virtual void Sync(uint32_t data_va, uint32_t size) = 0;
    // a lock that does not wait for the GPU (NOOVERWRITE): a result a frame or two old is what the console gives too
    virtual void SyncLate(uint32_t data_va, uint32_t size) = 0;
    // SyncLate in two for the render thread: DeliverLate (any thread) writes the newest finished copy to guest memory,
    // false when there is none recent; RecordLate (in order with the draws) starts a copy for a later lock
    virtual bool DeliverLate(uint32_t /*data_va*/, uint32_t /*size*/) { return false; }
    virtual void RecordLate(uint32_t) {}
    // IsBusy's read-back of a surface the GPU drew, which never waits on the GPU. Fetching (the title's thread): true
    // while one is on its way; the first call after a draw starts it, the call that finds it made has written it to
    // guest memory. In two for the render thread: StartFetch (in order with the draws) copies what is drawn so far,
    // Fetched (any thread) writes the copy once the GPU has made it, false until then.
    virtual bool Fetching(uint32_t /*data*/) { return false; }
    virtual void StartFetch(uint32_t) {}
    virtual bool Fetched(uint32_t) { return true; }
    virtual void Flush() = 0;
    virtual void SetGamma(const uint8_t* ramp /*3 x 256*/) = 0;
    // the overlay shown from the next present on
    virtual void SetOverlay(const Overlay& /*overlay*/) {}
    // visibility tests: draws count passing pixels while simple[kMCountEnable / 4] is set, ClearReport zeroes the
    // count, GetReport writes it to a 16 byte report once the GPU is past those draws; without them it stays pending
    virtual void ClearReport() {}
    virtual void GetReport(uint32_t /*report*/) {}

    // the XDK's fences and busy checks against the render thread; a backend that draws at once is never behind
    virtual uint32_t Fence() { return ++fence_; }
    virtual bool FencePending(uint32_t) { return false; }
    virtual void WaitFence(uint32_t) {}
    virtual bool Busy(uint32_t /*resource*/) { return false; }
    virtual void WaitResource(uint32_t /*resource*/) {}
    // until the GPU has drawn present number n, counted from 0; any thread
    virtual void WaitGpuFrame(uint64_t /*n*/) {}

private:
    uint32_t fence_ = 0;
};

// XBE_BACKEND=vulkan|d3d12 picks one (Direct3D 12 by default); the others are what each backend file provides
std::unique_ptr<Backend> CreateBackend();
std::unique_ptr<Backend> CreateD3D12Backend();
std::unique_ptr<Backend> CreateVulkanBackend();
// the same backend driven from a thread of its own: calls return once recorded
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);

// perf diagnostic: the title's thread recording for the render thread, and waiting on it
inline std::atomic<uint64_t> g_perf_capture_ns{0}, g_perf_wait_ns{0}, g_perf_waits{0};
inline std::atomic<uint64_t> g_perf_wait_kind_ns[6];  // read-back, resource, fence, flush, copy, next frame
inline std::atomic<uint64_t> g_perf_sync_late{0}, g_perf_sync_block{0};  // read-backs the title waited for, by cause

// F3: save the next frame, every draw and target, under diag::CaptureDir
inline std::atomic<bool> g_capture{false};

}  // namespace gpu
