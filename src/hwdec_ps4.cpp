// PS4 hardware video decoding for Stremio using libSceVideodec2.
// Initial target: H.264/AVC up to 1080p. HEVC/VP9 fall back to FFmpeg.
// ABI and memory strategy are based on the console-validated Moonlight-PS4
// Videodec2 implementation (GPL-3.0-or-later).

#include "hwdec_ps5.h" // generic HwDecoder interface retained from upstream
#include "orbis/videodec2_ps4.h"
#include "util.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

// FFmpeg < 7 exposes codec profile constants as FF_PROFILE_*; newer
// releases also provide AV_PROFILE_* aliases. Normalize both APIs here.
#ifndef AV_PROFILE_UNKNOWN
#define AV_PROFILE_UNKNOWN FF_PROFILE_UNKNOWN
#endif
#ifndef AV_PROFILE_H264_HIGH
#define AV_PROFILE_H264_HIGH FF_PROFILE_H264_HIGH
#endif

#include <orbis/libkernel.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <vector>
#include <sys/types.h>
#include <emmintrin.h>
#include <smmintrin.h>

extern "C" {
int32_t sceKernelAllocateDirectMemory(off_t, off_t, size_t, size_t, int32_t, off_t*);
int32_t sceKernelMapDirectMemory(void**, size_t, int32_t, int32_t, off_t, size_t);
int32_t sceKernelMapDirectMemory2(void**, size_t, int32_t, int32_t, int32_t, off_t, size_t);
int32_t sceKernelReleaseDirectMemory(off_t, size_t);
int32_t sceKernelMunmap(void*, size_t);
size_t sceKernelGetDirectMemorySize(void);
void sceGnmFlushGarlic(void);
}

namespace {
constexpr int kMaxW = 1920;
constexpr int kMaxDisplayH = 1080;
constexpr int kPipelineDepth = 2;
// AU buffers only need to cover decoder pipeline latency, but decoded frames
// stay queued for PTS reordering. Keep more framebuffer slots so the decoder
// cannot overwrite a picture before Player::receive() consumes it.
constexpr int kAuSlots = 4;
constexpr int kFrameSlots = 8;
constexpr int kReorderDepth = 4;
constexpr int kOutSlots = kReorderDepth + 2;
constexpr int kPtsSlots = 24;
constexpr size_t kInitialAu = 1u << 20;

Ps4Vd2Api g_api;
bool g_loaded = false;
std::mutex g_open_m;
bool g_owned = false;

size_t align_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }

struct Dmem {
    void* ptr = nullptr;
    off_t off = -1;
    size_t size = 0;
    int type = PS4_DMEM_ONION;

    bool alloc(size_t bytes, int mem_type, size_t alignment = PS4_DMEM_ALIGN, bool map2 = false) {
        reset();
        if (alignment < PS4_DMEM_ALIGN) alignment = PS4_DMEM_ALIGN;
        bytes = align_up(std::max(bytes, alignment), alignment);
        int rc = sceKernelAllocateDirectMemory(0, off_t(sceKernelGetDirectMemorySize()),
                                               bytes, alignment, mem_type, &off);
        if (rc < 0) return false;
        type = mem_type;
        if (map2) {
            rc = sceKernelMapDirectMemory2(&ptr, bytes, PS4_DMEM_ONION,
                                           PS4_DMEM_PROT_RW, 0, off, alignment);
            if (rc == 0 && ptr) {
                type = PS4_DMEM_ONION;
                size = bytes;
                return true;
            }
        }
        rc = sceKernelMapDirectMemory(&ptr, bytes, PS4_DMEM_PROT_RW, 0, off, alignment);
        if (rc < 0 || !ptr) {
            sceKernelReleaseDirectMemory(off, bytes);
            off = -1;
            ptr = nullptr;
            return false;
        }
        size = bytes;
        return true;
    }

    void reset() {
        if (ptr && size) sceKernelMunmap(ptr, size);
        if (off >= 0 && size) sceKernelReleaseDirectMemory(off, size);
        ptr = nullptr;
        off = -1;
        size = 0;
    }

    ~Dmem() { reset(); }
};

void cache_invalidate(void* p, size_t n) {
    if (!p || !n) return;
    uintptr_t a = uintptr_t(p) & ~uintptr_t(63);
    const uintptr_t end = uintptr_t(p) + n;
    for (; a < end; a += 64) _mm_clflush(reinterpret_cast<void*>(a));
    _mm_mfence();
}

__attribute__((target("sse4.1")))
void wc_copy(void* dstv, const void* srcv, size_t n) {
    auto* dst = static_cast<uint8_t*>(dstv);
    auto* src = static_cast<const uint8_t*>(srcv);
    size_t i = 0;
    for (; i < n && (uintptr_t(src + i) & 15); ++i) dst[i] = src[i];
    for (; i + 64 <= n; i += 64) {
        __m128i a = _mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<uint8_t*>(src + i + 0)));
        __m128i b = _mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<uint8_t*>(src + i + 16)));
        __m128i c = _mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<uint8_t*>(src + i + 32)));
        __m128i d = _mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<uint8_t*>(src + i + 48)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 0), a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 16), b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 32), c);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 48), d);
    }
    for (; i < n; ++i) dst[i] = src[i];
}

// If MapDirectMemory2 cannot give the decoder framebuffer a cacheable alias,
// CPU reads come from WC_GARLIC. A normal memcpy is extremely slow there on
// Jaguar. Use the MOVNTDQA path above and split 1080p copies over four
// persistent workers, following the hardware-tested Moonlight-PS4 approach.
constexpr int kBounceWorkers = 4;
struct BounceJob { void* dst = nullptr; const void* src = nullptr; size_t n = 0; };
pthread_t g_bounce_threads[kBounceWorkers]{};
pthread_mutex_t g_bounce_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_bounce_cv = PTHREAD_COND_INITIALIZER;
BounceJob g_bounce_jobs[kBounceWorkers];
bool g_bounce_alive = false;
int g_bounce_seq = 0, g_bounce_done = 0, g_bounce_njobs = 0;

void* bounce_worker(void* arg) {
    const int id = int(uintptr_t(arg));
    int seen = 0;
    for (;;) {
        pthread_mutex_lock(&g_bounce_mtx);
        while (g_bounce_alive && g_bounce_seq == seen)
            pthread_cond_wait(&g_bounce_cv, &g_bounce_mtx);
        if (!g_bounce_alive) {
            pthread_mutex_unlock(&g_bounce_mtx);
            return nullptr;
        }
        seen = g_bounce_seq;
        BounceJob job = g_bounce_jobs[id];
        pthread_mutex_unlock(&g_bounce_mtx);
        if (job.n) wc_copy(job.dst, job.src, job.n);
        pthread_mutex_lock(&g_bounce_mtx);
        ++g_bounce_done;
        if (g_bounce_done >= g_bounce_njobs) pthread_cond_broadcast(&g_bounce_cv);
        pthread_mutex_unlock(&g_bounce_mtx);
    }
}

bool bounce_start() {
    if (g_bounce_alive) return true;
    g_bounce_alive = true;
    g_bounce_seq = g_bounce_done = 0;
    for (int i = 0; i < kBounceWorkers; ++i) {
        if (pthread_create(&g_bounce_threads[i], nullptr, bounce_worker,
                           reinterpret_cast<void*>(uintptr_t(i))) != 0) {
            pthread_mutex_lock(&g_bounce_mtx);
            g_bounce_alive = false;
            pthread_cond_broadcast(&g_bounce_cv);
            pthread_mutex_unlock(&g_bounce_mtx);
            for (int j = 0; j < i; ++j) pthread_join(g_bounce_threads[j], nullptr);
            dlog("ps4 hwdec: WC bounce worker creation failed; using one core");
            return false;
        }
    }
    dlog("ps4 hwdec: WC bounce pool ready (%d threads)", kBounceWorkers);
    return true;
}

void bounce_stop() {
    if (!g_bounce_alive) return;
    pthread_mutex_lock(&g_bounce_mtx);
    g_bounce_alive = false;
    pthread_cond_broadcast(&g_bounce_cv);
    pthread_mutex_unlock(&g_bounce_mtx);
    for (auto& t : g_bounce_threads) pthread_join(t, nullptr);
}

void wc_copy_mt(void* dst, const void* src, size_t n) {
    if (n < 256 * 1024 || !g_bounce_alive) {
        wc_copy(dst, src, n);
        return;
    }
    const size_t chunk = (n / kBounceWorkers) & ~size_t(15);
    if (!chunk) { wc_copy(dst, src, n); return; }
    pthread_mutex_lock(&g_bounce_mtx);
    g_bounce_njobs = kBounceWorkers;
    g_bounce_done = 0;
    size_t off = 0;
    for (int i = 0; i < kBounceWorkers; ++i) {
        const size_t len = (i == kBounceWorkers - 1) ? n - off : chunk;
        g_bounce_jobs[i] = {static_cast<uint8_t*>(dst) + off,
                            static_cast<const uint8_t*>(src) + off, len};
        off += len;
    }
    ++g_bounce_seq;
    pthread_cond_broadcast(&g_bounce_cv);
    while (g_bounce_done < g_bounce_njobs)
        pthread_cond_wait(&g_bounce_cv, &g_bounce_mtx);
    pthread_mutex_unlock(&g_bounce_mtx);
}

} // namespace

struct HwDecoder::Impl {
    Ps4Vd2Decoder decoder = nullptr;
    Ps4Vd2DecoderConfigInfo cfg{};
    Ps4Vd2DecoderMemoryInfo mem{};
    Dmem cpu, gpu, cpugpu, frames;
    Dmem au[kAuSlots];
    size_t frame_stride = 0;
    bool frame_cacheable = false;
    int frame_index = 0;
    int au_index = 0;
    int display_w = 0, display_h = 0;
    bool fatal = false;
    bool flushing = false;
    unsigned calls = 0, pictures = 0, errors = 0;

    AVBSFContext* bsf = nullptr;
    AVCodecParameters* bsf_par = nullptr;
    AVPacket* in_pkt = nullptr;
    AVPacket* out_pkt = nullptr;
    std::string extradata;

    int64_t pts[kPtsSlots]{};
    int pts_n = 0;

    struct Out {
        bool used = false;
        HwDecoder::Picture pic;
        std::vector<uint8_t> bounce;
    } out[kOutSlots];
    int out_n = 0;

    void pts_push(int64_t v) {
        if (pts_n == kPtsSlots) {
            std::memmove(pts, pts + 1, sizeof(int64_t) * (kPtsSlots - 1));
            pts_n--;
        }
        pts[pts_n++] = v;
    }
    int64_t pts_take() {
        if (!pts_n) return INT64_MIN;
        int best = 0;
        for (int i = 1; i < pts_n; ++i) {
            if (pts[best] == INT64_MIN || (pts[i] != INT64_MIN && pts[i] < pts[best])) best = i;
        }
        int64_t v = pts[best];
        pts[best] = pts[--pts_n];
        return v;
    }

    bool build_bsf() {
        const AVBitStreamFilter* filter = av_bsf_get_by_name("h264_mp4toannexb");
        if (!filter) return false;
        if (bsf) av_bsf_free(&bsf);
        if (av_bsf_alloc(filter, &bsf) < 0) return false;
        if (avcodec_parameters_copy(bsf->par_in, bsf_par) < 0) return false;
        bsf->time_base_in = AVRational{1, 1000000};
        return av_bsf_init(bsf) >= 0;
    }

    bool ensure_au(size_t need) {
        Dmem& a = au[au_index];
        if (a.ptr && a.size >= need) return true;
        size_t cap = std::max(kInitialAu, align_up(need + need / 2, PS4_DMEM_ALIGN));
        return a.alloc(cap, PS4_DMEM_ONION);
    }

    void harvest(const Ps4Vd2OutputInfo& oi) {
        Out* slot = nullptr;
        for (auto& x : out) if (!x.used) { slot = &x; break; }
        if (!slot || !oi.frameBuffer) return;

        uint32_t pitch = oi.framePitchInBytes ? oi.framePitchInBytes : oi.framePitch;
        int padded_h = int(oi.frameHeight ? oi.frameHeight : display_h);
        int w = std::min(display_w, int(oi.frameWidth ? oi.frameWidth : display_w));
        int h = std::min(display_h, padded_h);
        if (pitch == 0 || w <= 0 || h <= 0) return;

        const uint8_t* base = static_cast<const uint8_t*>(oi.frameBuffer);
        size_t bytes = size_t(pitch) * size_t(padded_h) * 3 / 2;
        if (bytes > frame_stride) bytes = frame_stride;

        sceGnmFlushGarlic();
        if (frame_cacheable) {
            cache_invalidate(const_cast<uint8_t*>(base), bytes);
            slot->pic.y = base;
            slot->pic.uv = base + size_t(pitch) * size_t(padded_h);
        } else {
            slot->bounce.resize(bytes);
            wc_copy_mt(slot->bounce.data(), base, bytes);
            slot->pic.y = slot->bounce.data();
            slot->pic.uv = slot->bounce.data() + size_t(pitch) * size_t(padded_h);
        }
        slot->pic.pitch = int(pitch);
        slot->pic.width = w;
        slot->pic.height = h;
        slot->pic.ten_bit = false;
        slot->pic.pts_us = pts_take();
        slot->used = true;
        out_n++;
        pictures++;
    }

    int decode_one(const uint8_t* data, int size, int64_t pts_us, bool present) {
        if (!data || size <= 0) return 0;
        if (!ensure_au(size_t(size))) return -1;
        Dmem& a = au[au_index];
        std::memcpy(a.ptr, data, size_t(size));
        au_index = (au_index + 1) % kAuSlots;

        uint8_t* fbptr = static_cast<uint8_t*>(frames.ptr) + size_t(frame_index) * frame_stride;
        frame_index = (frame_index + 1) % kFrameSlots;

        Ps4Vd2InputData in{};
        in.thisSize = sizeof(in);
        in.auData = a.ptr;
        in.auSize = uint64_t(size);
        in.ptsData = pts_us == INT64_MIN ? UINT64_MAX : uint64_t(pts_us);
        in.dtsData = UINT64_MAX;

        Ps4Vd2FrameBuffer fb{};
        fb.thisSize = sizeof(fb);
        fb.frameBuffer = fbptr;
        fb.frameBufferSize = frame_stride;

        Ps4Vd2OutputInfo oi{};
        oi.thisSize = sizeof(oi);

        int rc = g_api.Decode(decoder, &in, &fb, &oi);
        calls++;
        if (rc < 0) {
            errors++;
            dlog("ps4 hwdec: Decode #%u -> 0x%08x (%d bytes)", calls, unsigned(rc), size);
            return -1;
        }
        if (present) pts_push(pts_us);
        if (oi.isValid && oi.pictureCount) harvest(oi);
        return 0;
    }

    int feed_filtered() {
        while (av_bsf_receive_packet(bsf, out_pkt) == 0) {
            int64_t p = out_pkt->pts == AV_NOPTS_VALUE ? INT64_MIN : out_pkt->pts;
            int rc = decode_one(out_pkt->data, out_pkt->size, p, true);
            av_packet_unref(out_pkt);
            if (rc < 0) return rc;
        }
        return 0;
    }

    void drain() {
        for (int i = 0; i < 32 && out_n < kOutSlots - 1; ++i) {
            uint8_t* fbptr = static_cast<uint8_t*>(frames.ptr) + size_t(frame_index) * frame_stride;
            frame_index = (frame_index + 1) % kFrameSlots;
            Ps4Vd2FrameBuffer fb{};
            fb.thisSize = sizeof(fb);
            fb.frameBuffer = fbptr;
            fb.frameBufferSize = frame_stride;
            Ps4Vd2OutputInfo oi{};
            oi.thisSize = sizeof(oi);
            int rc = g_api.Flush(decoder, &fb, &oi);
            if (rc < 0 || !oi.isValid || !oi.pictureCount) break;
            harvest(oi);
        }
    }

    bool recreate() {
        if (decoder) g_api.DeleteDecoder(decoder);
        decoder = nullptr;
        Ps4Vd2DecoderMemoryInfo m = mem;
        int rc = g_api.CreateDecoder(&cfg, &m, &decoder);
        if (rc < 0 || !decoder) {
            dlog("ps4 hwdec: recreate -> 0x%08x", unsigned(rc));
            return false;
        }
        if (g_api.Reset && g_api.Reset(decoder) < 0) return false;
        return true;
    }

    ~Impl() {
        bounce_stop();
        if (decoder && g_api.DeleteDecoder) g_api.DeleteDecoder(decoder);
        decoder = nullptr;
        if (bsf) av_bsf_free(&bsf);
        if (bsf_par) avcodec_parameters_free(&bsf_par);
        if (in_pkt) av_packet_free(&in_pkt);
        if (out_pkt) av_packet_free(&out_pkt);
    }
};

void HwDecoder::load_module() {
    Ps4Vd2Api a;
    g_loaded = ps4_vd2_load(a);
    if (g_loaded) g_api = a;
    dlog("ps4 hwdec: Videodec2 %s", g_loaded ? "loaded" : "unavailable");
}

HwDecoder* HwDecoder::open(const AVCodecParameters* par, std::string* why) {
    auto refuse = [&](const std::string& s) -> HwDecoder* { if (why) *why = s; return nullptr; };
    if (!g_loaded) return refuse("PS4 Videodec2 is not loaded");
    if (!par || par->codec_id != AV_CODEC_ID_H264) return refuse("PS4 hardware path currently supports H.264 only");
    if (par->width <= 0 || par->height <= 0) return refuse("unknown H.264 dimensions");
    if (par->width > kMaxW || par->height > kMaxDisplayH) return refuse("H.264 is larger than the PS4 1080p hardware path");
    if (par->bits_per_raw_sample > 8) return refuse("10-bit H.264");
    if (par->profile != AV_PROFILE_UNKNOWN && par->profile > AV_PROFILE_H264_HIGH)
        return refuse("unsupported H.264 profile " + std::to_string(par->profile));

    std::lock_guard<std::mutex> lock(g_open_m);
    if (g_owned) return refuse("PS4 hardware decoder busy");
    if (!ps4_vd2_ensure_queue(g_api)) return refuse("could not allocate PS4 Videodec2 compute queue");

    auto* n = new Impl();
    n->display_w = par->width;
    n->display_h = par->height;

    n->cfg.thisSize = sizeof(n->cfg);
    n->cfg.resourceType = PS4_VD2_RESOURCE_EMBEDDED;
    n->cfg.codecType = PS4_VD2_CODEC_AVC;
    n->cfg.profile = PS4_VD2_PROFILE_HIGH;
    n->cfg.maxLevel = PS4_VD2_LEVEL_51;
    n->cfg.maxFrameWidth = par->width;
    n->cfg.maxFrameHeight = (par->height + 15) & ~15;
    n->cfg.maxDpbFrameCount = 4;
    n->cfg.decodePipelineDepth = kPipelineDepth;
    n->cfg.computeQueue = g_api.queue;
    n->cfg.cpuAffinityMask = PS4_VD2_AFFINITY_ALL;
    n->cfg.cpuThreadPriority = PS4_VD2_THREAD_PRIO;
    n->cfg.optimizeProgressiveVideo = true;
    n->cfg.checkMemoryType = false;

    n->mem.thisSize = sizeof(n->mem);
    int rc = g_api.QueryDecoderMemoryInfo(&n->cfg, &n->mem);
    if (rc < 0) {
        delete n;
        return refuse("QueryDecoderMemoryInfo failed (0x" + std::to_string(unsigned(rc)) + ")");
    }

    if (n->mem.cpuMemorySize && !n->cpu.alloc(size_t(n->mem.cpuMemorySize), PS4_DMEM_ONION)) { delete n; return refuse("CPU decoder memory allocation failed"); }
    if (n->mem.gpuMemorySize && !n->gpu.alloc(size_t(n->mem.gpuMemorySize), PS4_DMEM_GARLIC)) { delete n; return refuse("GPU decoder memory allocation failed"); }
    if (n->mem.cpuGpuMemorySize && !n->cpugpu.alloc(size_t(n->mem.cpuGpuMemorySize), PS4_DMEM_ONION)) { delete n; return refuse("CPU/GPU decoder memory allocation failed"); }
    n->mem.cpuMemory = n->cpu.ptr;
    n->mem.gpuMemory = n->gpu.ptr;
    n->mem.cpuGpuMemory = n->cpugpu.ptr;

    n->frame_stride = align_up(size_t(n->mem.maxFrameBufferSize),
                               std::max<size_t>(n->mem.frameBufferAlignment, PS4_DMEM_ALIGN));
    if (!n->frame_stride) { delete n; return refuse("Videodec2 returned a zero framebuffer size"); }

    // Physical framebuffer memory is Garlic. Map2 as ONION if firmware allows
    // it, which makes the decoder output fast to read from the CPU. Otherwise
    // use WC Garlic and stream-copy each decoded frame to normal RAM.
    if (!n->frames.alloc(n->frame_stride * kFrameSlots, PS4_DMEM_GARLIC,
                         std::max<size_t>(n->mem.frameBufferAlignment, PS4_DMEM_ALIGN), true)) {
        delete n;
        return refuse("framebuffer direct-memory allocation failed");
    }
    n->frame_cacheable = n->frames.type == PS4_DMEM_ONION || n->frames.type == PS4_DMEM_WB_GARLIC;
    if (!n->frame_cacheable) bounce_start();

    for (int i = 0; i < kAuSlots; ++i) {
        if (!n->au[i].alloc(kInitialAu, PS4_DMEM_ONION)) {
            delete n;
            return refuse("access-unit buffer allocation failed");
        }
    }

    Ps4Vd2DecoderMemoryInfo m = n->mem;
    rc = g_api.CreateDecoder(&n->cfg, &m, &n->decoder);
    if (rc < 0 || !n->decoder) {
        delete n;
        return refuse("CreateDecoder failed");
    }
    if (g_api.Reset) {
        rc = g_api.Reset(n->decoder);
        if (rc < 0) {
            delete n;
            return refuse("Reset after CreateDecoder failed");
        }
    }

    const bool avcc = par->extradata && par->extradata_size >= 4 && par->extradata[0] == 1;
    if (avcc) {
        n->bsf_par = avcodec_parameters_alloc();
        if (!n->bsf_par || avcodec_parameters_copy(n->bsf_par, par) < 0 || !n->build_bsf()) {
            delete n;
            return refuse("h264_mp4toannexb setup failed");
        }
    } else if (par->extradata && par->extradata_size > 0) {
        n->extradata.assign(reinterpret_cast<const char*>(par->extradata), size_t(par->extradata_size));
    }
    n->in_pkt = av_packet_alloc();
    n->out_pkt = av_packet_alloc();
    if (!n->in_pkt || !n->out_pkt) {
        delete n;
        return refuse("FFmpeg packet allocation failed");
    }

    if (!n->extradata.empty())
        n->decode_one(reinterpret_cast<const uint8_t*>(n->extradata.data()), int(n->extradata.size()), INT64_MIN, false);

    auto* h = new HwDecoder();
    h->d_ = n;
    g_owned = true;
    dlog("ps4 hwdec: H.264 %dx%d ready, fb=%zu KB x%d cacheable=%d",
         par->width, par->height, n->frame_stride >> 10, kFrameSlots, n->frame_cacheable ? 1 : 0);
    return h;
}

HwDecoder::~HwDecoder() {
    if (!d_) return;
    Impl* n = d_;
    dlog("ps4 hwdec: closed after %u calls, %u pictures, %u errors", n->calls, n->pictures, n->errors);
    delete n;
    d_ = nullptr;
    std::lock_guard<std::mutex> lock(g_open_m);
    g_owned = false;
}

int HwDecoder::send(const uint8_t* data, int size, int64_t pts_us) {
    Impl* n = d_;
    if (!n || n->fatal || !n->decoder) return -1;
    if (!data || size <= 0) {
        if (n->bsf) {
            av_bsf_send_packet(n->bsf, nullptr);
            n->feed_filtered();
        }
        n->drain();
        n->flushing = true;
        return 0;
    }
    if (n->out_n > kReorderDepth) return 1;

    int rc = 0;
    if (n->bsf) {
        av_packet_unref(n->in_pkt);
        if (av_new_packet(n->in_pkt, size) < 0) return -1;
        std::memcpy(n->in_pkt->data, data, size_t(size));
        n->in_pkt->pts = pts_us == INT64_MIN ? AV_NOPTS_VALUE : pts_us;
        n->in_pkt->dts = AV_NOPTS_VALUE;
        if (av_bsf_send_packet(n->bsf, n->in_pkt) < 0) return -1;
        rc = n->feed_filtered();
    } else {
        rc = n->decode_one(data, size, pts_us, true);
    }
    if (rc < 0) n->fatal = true;
    return rc < 0 ? -1 : 0;
}

int HwDecoder::receive(Picture* out) {
    Impl* n = d_;
    if (!n || n->fatal) return -1;
    if (n->out_n <= (n->flushing ? 0 : kReorderDepth)) return 0;
    Impl::Out* best = nullptr;
    for (auto& x : n->out) {
        if (!x.used) continue;
        if (!best || best->pic.pts_us == INT64_MIN ||
            (x.pic.pts_us != INT64_MIN && x.pic.pts_us < best->pic.pts_us)) best = &x;
    }
    if (!best) return 0;
    *out = best->pic;
    best->used = false;
    n->out_n--;
    return 1;
}

void HwDecoder::flush() {
    Impl* n = d_;
    if (!n) return;
    for (auto& x : n->out) x.used = false;
    n->out_n = 0;
    n->pts_n = 0;
    n->frame_index = 0;
    n->au_index = 0;
    n->fatal = false;
    n->flushing = false;
    if (n->bsf_par && !n->build_bsf()) {
        n->fatal = true;
        return;
    }
    if (!n->recreate()) {
        n->fatal = true;
        return;
    }
    if (!n->extradata.empty())
        n->decode_one(reinterpret_cast<const uint8_t*>(n->extradata.data()), int(n->extradata.size()), INT64_MIN, false);
}

const char* HwDecoder::name() const { return "H.264 (PS4 Videodec2)"; }
