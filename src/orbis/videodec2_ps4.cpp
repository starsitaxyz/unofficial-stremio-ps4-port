#include "videodec2_ps4.h"

#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>

#include <cstring>
#include <initializer_list>
#include <mutex>
#include <sys/types.h>

#include "../util.h"

extern "C" {
int32_t sceKernelAllocateDirectMemory(off_t, off_t, size_t, size_t, int32_t, off_t*);
int32_t sceKernelMapDirectMemory(void**, size_t, int32_t, int32_t, off_t, size_t);
int32_t sceKernelReleaseDirectMemory(off_t, size_t);
int32_t sceKernelMunmap(void*, size_t);
size_t sceKernelGetDirectMemorySize(void);
}

namespace {
std::mutex g_m;
int g_mod = -1;
Ps4Vd2Api g_api;
Ps4Vd2ComputeQueue g_queue = nullptr;
void* g_compute_mem = nullptr;
off_t g_compute_off = 0;
size_t g_compute_size = 0;

size_t align16k(size_t n) { return (n + PS4_DMEM_ALIGN - 1) & ~(PS4_DMEM_ALIGN - 1); }

int load_module(const char* path) {
    int rc = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
    dlog("ps4 vd2: LoadStartModule(%s) -> 0x%08x", path, unsigned(rc));
    return rc;
}

bool sym(int mod, const char* name, void** out) {
    int rc = sceKernelDlsym(mod, name, out);
    if (rc != 0 || !*out) {
        dlog("ps4 vd2: Dlsym %s -> 0x%08x", name, unsigned(rc));
        return false;
    }
    return true;
}

bool resolve(Ps4Vd2Api& a, int mod) {
    a.module = mod;
#define VD2SYM(member, name) if (!sym(mod, name, reinterpret_cast<void**>(&a.member))) return false
    VD2SYM(QueryComputeMemoryInfo, "sceVideodec2QueryComputeMemoryInfo");
    VD2SYM(AllocateComputeQueue, "sceVideodec2AllocateComputeQueue");
    VD2SYM(ReleaseComputeQueue, "sceVideodec2ReleaseComputeQueue");
    VD2SYM(QueryDecoderMemoryInfo, "sceVideodec2QueryDecoderMemoryInfo");
    VD2SYM(CreateDecoder, "sceVideodec2CreateDecoder");
    VD2SYM(DeleteDecoder, "sceVideodec2DeleteDecoder");
    VD2SYM(Decode, "sceVideodec2Decode");
    VD2SYM(Flush, "sceVideodec2Flush");
    VD2SYM(Reset, "sceVideodec2Reset");
#undef VD2SYM
    return true;
}

bool alloc_queue(Ps4Vd2Api& a) {
    if (g_queue) {
        a.queue = g_queue;
        return true;
    }

    Ps4Vd2ComputeMemoryInfo mi{};
    mi.thisSize = sizeof(mi);
    int rc = a.QueryComputeMemoryInfo(&mi);
    if (rc < 0) {
        dlog("ps4 vd2: QueryComputeMemoryInfo -> 0x%08x", unsigned(rc));
        return false;
    }

    g_compute_size = align16k(size_t(mi.cpuGpuMemorySize));
    rc = sceKernelAllocateDirectMemory(0, off_t(sceKernelGetDirectMemorySize()),
                                       g_compute_size, PS4_DMEM_ALIGN,
                                       PS4_DMEM_ONION, &g_compute_off);
    if (rc < 0) {
        dlog("ps4 vd2: compute AllocateDirectMemory -> 0x%08x", unsigned(rc));
        return false;
    }
    rc = sceKernelMapDirectMemory(&g_compute_mem, g_compute_size, PS4_DMEM_PROT_RW,
                                  0, g_compute_off, PS4_DMEM_ALIGN);
    if (rc < 0) {
        dlog("ps4 vd2: compute MapDirectMemory -> 0x%08x", unsigned(rc));
        sceKernelReleaseDirectMemory(g_compute_off, g_compute_size);
        g_compute_off = 0;
        g_compute_size = 0;
        return false;
    }
    mi.cpuGpuMemory = g_compute_mem;

    static const struct { uint16_t pipe, queue; } tries[] = {
        {0,0}, {0,1}, {1,0}, {1,1}, {2,0}, {3,0}, {4,0}
    };
    for (auto t : tries) {
        Ps4Vd2ComputeConfigInfo ci{};
        ci.thisSize = sizeof(ci);
        ci.computePipeId = t.pipe;
        ci.computeQueueId = t.queue;
        ci.checkMemoryType = true;
        Ps4Vd2ComputeQueue q = nullptr;
        rc = a.AllocateComputeQueue(&ci, &mi, &q);
        dlog("ps4 vd2: AllocateComputeQueue %u/%u -> 0x%08x q=%p",
             t.pipe, t.queue, unsigned(rc), q);
        if (rc == 0 && q) {
            g_queue = q;
            a.queue = q;
            g_api.queue = q;
            return true;
        }
    }
    // Do not strand direct memory if every pipe/queue is unavailable.
    if (g_compute_mem && g_compute_size) sceKernelMunmap(g_compute_mem, g_compute_size);
    if (g_compute_size) sceKernelReleaseDirectMemory(g_compute_off, g_compute_size);
    g_compute_mem = nullptr;
    g_compute_off = 0;
    g_compute_size = 0;
    return false;
}
} // namespace

bool ps4_vd2_load(Ps4Vd2Api& api) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_mod >= 0) {
        api = g_api;
        api.queue = g_queue;
        return true;
    }

    // Dependencies used by the hardware decoder. Firmware may already have
    // some loaded; failures here are non-fatal and Videodec2 is tried anyway.
    for (const char* p : {
             "/system/common/lib/libSceVdecCore.sprx",
             "/system/common/lib/libSceVdecSavc.sprx",
             "/system/common/lib/libSceVdecSavc2.sprx",
             "/system/common/lib/libSceVdecwrap.sprx"}) {
        load_module(p);
    }
    // Moonlight-PS4 also asks the internal sysmodule loader for VDECCORE;
    // keeping both paths makes this work across more firmware/module states.
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VDECCORE);

    int mod = load_module("/system/common/lib/libSceVideodec2.sprx");
    if (mod < 0) mod = load_module("libSceVideodec2.sprx");
    if (mod < 0) return false;

    Ps4Vd2Api a;
    if (!resolve(a, mod)) return false;
    g_mod = mod;
    g_api = a;
    api = a;
    dlog("ps4 vd2: module and symbols ready");
    return true;
}

bool ps4_vd2_ensure_queue(Ps4Vd2Api& api) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_queue) {
        api.queue = g_queue;
        return true;
    }
    return alloc_queue(api);
}
