#include "ps4_runtime.h"

#ifdef PLATFORM_PS4

#include "util.h"

// OpenOrbis orbis/Net.h uses size_t without including stddef itself.
#include <stddef.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>

#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <cstdlib>

namespace {
constexpr int kNetPoolSize = 1024 * 1024;
int g_net_pool = -1;
bool g_netctl = false;

[[noreturn]] void hang_forever() {
    for (;;) sceKernelUsleep(1000 * 1000);
}
}

bool ps4_runtime_init() {
    // OpenOrbis homebrew needs libSceNet initialised explicitly before the
    // POSIX socket wrappers used by curl/FFmpeg/torrents are reliable.
    uint32_t mod = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    dlog("ps4 runtime: LoadModuleInternal(NET) -> 0x%08x", unsigned(mod));
    mod = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);
    dlog("ps4 runtime: LoadModuleInternal(NETCTL) -> 0x%08x", unsigned(mod));
    mod = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    dlog("ps4 runtime: LoadModuleInternal(SYSTEM_SERVICE) -> 0x%08x", unsigned(mod));

    int rc = sceNetInit();
    dlog("ps4 runtime: sceNetInit -> 0x%08x", unsigned(rc));
    // Already-initialised is harmless; verify the pool/socket path below.

    if (g_net_pool < 0) {
        g_net_pool = sceNetPoolCreate("stremio", kNetPoolSize, 0);
        dlog("ps4 runtime: sceNetPoolCreate(%d KB) -> 0x%08x",
             kNetPoolSize >> 10, unsigned(g_net_pool));
        if (g_net_pool < 0) return false;
    }

    rc = sceNetCtlInit();
    dlog("ps4 runtime: sceNetCtlInit -> 0x%08x", unsigned(rc));
    if (rc == 0) g_netctl = true;

    // PS4's socket shim may reject protocol=IPPROTO_UDP while accepting 0.
    // The torrent engine already creates UDP with protocol 0, but test the
    // POSIX bridge here so failures are visible immediately in log.txt.
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        dlog("ps4 runtime: UDP socket test FAILED errno=%d", errno);
        return false;
    }
    close(fd);
    dlog("ps4 runtime: network ready");
    return true;
}

[[noreturn]] void ps4_runtime_exit() {
    if (g_netctl) {
        sceNetCtlTerm();
        g_netctl = false;
    }
    if (g_net_pool >= 0) {
        sceNetPoolDestroy(g_net_pool);
        g_net_pool = -1;
    }

    // Returning from main() can reach an unsupported process-exit syscall on
    // OpenOrbis titles. Ask SystemService to close us, then stay alive if the
    // request is refused on a particular firmware/homebrew environment.
    int rc = sceSystemServiceLoadExec("exit", nullptr);
    dlog("ps4 runtime: sceSystemServiceLoadExec(exit) -> 0x%08x", unsigned(rc));
    hang_forever();
}


// STREMIO_PS4_CXA_THREAD_ATEXIT
// OpenOrbis libc++abi can emit calls to __cxa_thread_atexit_impl for
// thread_local objects with non-trivial destructors, but the PS4 stubs do not
// provide that symbol. Maintain a LIFO destructor list per pthread.
struct Ps4ThreadDtor {
    void (*destructor)(void*);
    void* object;
    Ps4ThreadDtor* next;
};

static pthread_key_t g_ps4_thread_dtor_key;
static pthread_once_t g_ps4_thread_dtor_once = PTHREAD_ONCE_INIT;
static int g_ps4_thread_dtor_key_result = -1;

static void ps4_run_thread_dtors(void* ptr) {
    Ps4ThreadDtor* node = static_cast<Ps4ThreadDtor*>(ptr);
    while (node) {
        Ps4ThreadDtor* next = node->next;
        if (node->destructor) node->destructor(node->object);
        std::free(node);
        node = next;
    }
}

static void ps4_create_thread_dtor_key() {
    g_ps4_thread_dtor_key_result =
        pthread_key_create(&g_ps4_thread_dtor_key, ps4_run_thread_dtors);
}

extern "C" int __cxa_thread_atexit_impl(
    void (*destructor)(void*), void* object, void* dso_symbol) {
    (void)dso_symbol;
    if (pthread_once(&g_ps4_thread_dtor_once, ps4_create_thread_dtor_key) != 0)
        return -1;
    if (g_ps4_thread_dtor_key_result != 0)
        return -1;

    auto* node = static_cast<Ps4ThreadDtor*>(std::malloc(sizeof(Ps4ThreadDtor)));
    if (!node) return -1;
    node->destructor = destructor;
    node->object = object;
    node->next = static_cast<Ps4ThreadDtor*>(
        pthread_getspecific(g_ps4_thread_dtor_key));
    if (pthread_setspecific(g_ps4_thread_dtor_key, node) != 0) {
        std::free(node);
        return -1;
    }
    return 0;
}

#endif
