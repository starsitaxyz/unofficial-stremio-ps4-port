#!/usr/bin/env python3
"""Apply the PS4/OpenOrbis overlay to Sp9nky/unofficial-stremio-ps5-port.

Tested against the upstream layout observed at commit
89c0e6227cfb6549cc6e144a0a77fa852b8e316c (2026-10-06).
The script is intentionally marker-based and aborts instead of guessing when
upstream changed around a patch point.
"""
from __future__ import annotations

import argparse
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parent
PS4_SRC = PROJECT_ROOT / "src"
PS4_BUILD = HERE / "ps4"


def die(msg: str) -> None:
    raise SystemExit(f"apply_ps4_port.py: {msg}")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    n = text.count(old)
    if n != 1:
        die(f"{label}: expected marker exactly once, found {n}")
    return text.replace(old, new, 1)


def patch_file(path: Path, fn) -> None:
    if not path.is_file():
        die(f"missing upstream file: {path}")
    original = path.read_text(encoding="utf-8")
    updated = fn(original)
    if updated == original:
        print(f"= {path.relative_to(path.parents[1])}: already patched/no changes")
        return
    path.write_text(updated, encoding="utf-8")
    print(f"~ {path}")


def patch_cmake(s: str) -> str:
    if "PS4_V05_FREETYPE_TARGET" in s:
        return s
    if "PLATFORM_PS4=1" in s or "RMLUI_PS4_SOURCE" in s:
        die("CMakeLists.txt already contains an older PS4 overlay; apply v0.5 to a clean upstream checkout")

    s = replace_once(
        s,
        'option(PS5_NATIVE "build the native PS5 app (linked by native/build.sh)" OFF)\n\n'
        'find_package(SDL2 REQUIRED)\n'
        'find_package(RmlUi REQUIRED)\n'
        'find_package(PkgConfig REQUIRED)\n'
        'find_package(Threads REQUIRED)\n',
        'option(PS5_NATIVE "build the native PS5 app (linked by native/build.sh)" OFF)\n'
        'option(PS4_NATIVE "build the native PS4 app with OpenOrbis" OFF)\n'
        'if(PS4_NATIVE)\n'
        '    set(PS4 TRUE)\n'
        'endif()\n\n'
        'find_package(PkgConfig REQUIRED)\n'
        'if(PS4)\n'
        '    # PacBrew exposes SDL2 and the media stack through its own pkg-config.\n'
        '    pkg_check_modules(SDL2_PS4 REQUIRED IMPORTED_TARGET sdl2)\n'
        '    pkg_check_modules(FREETYPE2 REQUIRED IMPORTED_TARGET freetype2)\n'
        '    if(NOT TARGET SDL2::SDL2)\n'
        '        add_library(SDL2::SDL2 INTERFACE IMPORTED)\n'
        '        set_property(TARGET SDL2::SDL2 PROPERTY INTERFACE_LINK_LIBRARIES PkgConfig::SDL2_PS4)\n'
        '    endif()\n'
        '    # PS4_V05_FREETYPE_TARGET: RmlUi calls find_package(Freetype) as a subdirectory.\n'
        '    # Provide the canonical target up front from PacBrew pkg-config so\n'
        '    # CMake never accidentally finds the host FreeType while cross-building.\n'
        '    if(NOT TARGET Freetype::Freetype)\n'
        '        add_library(Freetype::Freetype INTERFACE IMPORTED)\n'
        '        set_property(TARGET Freetype::Freetype PROPERTY INTERFACE_LINK_LIBRARIES PkgConfig::FREETYPE2)\n'
        '        set_property(TARGET Freetype::Freetype PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${FREETYPE2_INCLUDE_DIRS}")\n'
        '    endif()\n'
        '    if(NOT TARGET Threads::Threads)\n'
        '        add_library(Threads::Threads INTERFACE IMPORTED)\n'
        '    endif()\n\n'
        '    # Build RmlUi in-tree with the same OpenOrbis toolchain.\n'
        '    set(RMLUI_PS4_SOURCE "${CMAKE_SOURCE_DIR}/.ps4-deps/RmlUi" CACHE PATH "RmlUi 6.2 source for PS4")\n'
        '    if(NOT EXISTS "${RMLUI_PS4_SOURCE}/CMakeLists.txt")\n'
        '        message(FATAL_ERROR "RmlUi source missing at ${RMLUI_PS4_SOURCE}; run ps4/build.sh")\n'
        '    endif()\n'
        '    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)\n'
        '    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)\n'
        '    set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)\n'
        '    set(RMLUI_TESTS OFF CACHE BOOL "" FORCE)\n'
        '    set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)\n'
        '    set(RMLUI_BACKEND native CACHE STRING "" FORCE)\n'
        '    set(RMLUI_FONT_ENGINE freetype CACHE STRING "" FORCE)\n'
        '    set(RMLUI_SVG_PLUGIN OFF CACHE BOOL "" FORCE)\n'
        '    set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)\n'
        '    add_subdirectory("${RMLUI_PS4_SOURCE}" "${CMAKE_BINARY_DIR}/rmlui" EXCLUDE_FROM_ALL)\n'
        'else()\n'
        '    find_package(SDL2 REQUIRED)\n'
        '    find_package(RmlUi REQUIRED)\n'
        '    find_package(Threads REQUIRED)\n'
        'endif()\n',
        "CMake dependency block",
    )
    s = replace_once(
        s,
        'pkg_check_modules(FREETYPE2 REQUIRED IMPORTED_TARGET freetype2)\n'
        'pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET\n',
        'if(NOT PS4)\n'
        '    pkg_check_modules(FREETYPE2 REQUIRED IMPORTED_TARGET freetype2)\n'
        'endif()\n'
        'pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET\n',
        "CMake PS4 FreeType ownership",
    )

    src_end = '''    src/torrent/torrent_stream.cpp
)

# PS5 is set by the payload SDK's toolchain file (toolchain/prospero.cmake).
'''
    src_new = '''    src/torrent/torrent_stream.cpp
)

# PS4 keeps the portable app/player/torrent code and swaps only the console
# glue + hardware decoder.
if(PS4)
    list(REMOVE_ITEM STREMIO_SOURCES src/hwdec_ps5.cpp)
    list(APPEND STREMIO_SOURCES
        src/hwdec_ps4.cpp
        src/orbis/videodec2_ps4.cpp
    )
endif()

# PS5 is set by the payload SDK's toolchain file (toolchain/prospero.cmake).
'''
    s = replace_once(s, src_end, src_new, "CMake PS4 source switch")

    old_select = '''if(PS5 AND PS5_NATIVE)
    add_library(stremio STATIC ${STREMIO_SOURCES} src/pad_ps5.cpp)
'''
    new_select = '''if(PS4)
    if(NOT PS4_NATIVE)
        message(FATAL_ERROR "PS4 builds require -DPS4_NATIVE=ON")
    endif()
    add_executable(stremio ${STREMIO_SOURCES}
        src/pad_ps4.cpp
        src/ps4_keyboard.cpp
        src/ps4_runtime.cpp
    )
    pkg_check_modules(FRIBIDI REQUIRED IMPORTED_TARGET fribidi)
    target_compile_definitions(stremio PRIVATE PLATFORM_PS4=1 HAVE_FRIBIDI=1)
elseif(PS5 AND PS5_NATIVE)
    add_library(stremio STATIC ${STREMIO_SOURCES} src/pad_ps5.cpp)
'''
    s = replace_once(s, old_select, new_select, "CMake target selection")

    s = replace_once(
        s,
        'if(TARGET SDL2::SDL2main AND NOT PS5_NATIVE)\n',
        'if(TARGET SDL2::SDL2main AND NOT PS5_NATIVE AND NOT PS4)\n',
        "CMake SDL2main",
    )
    s = replace_once(
        s,
        'if(NOT PS5_NATIVE)\n    # The torrent engine hashes with OpenSSL; it uses the DHT when libdht is there.\n',
        'if(NOT PS5_NATIVE AND NOT PS4)\n    # Desktop torrent hashing uses OpenSSL; PS4 uses libavutil SHA instead.\n',
        "CMake desktop-only dependencies",
    )

    s += '''

if(PS4)
    # All PacBrew ports are static. Pull their pkg-config --static dependency
    # lists into the final executable (zlib/bzip2/mbedTLS/libass/etc.), not
    # just the top-level imported targets. Everything lives in OpenOrbis usr/lib.
    set(PS4_PORTLIB_STATIC_DEPS
        ${SDL2_PS4_STATIC_LIBRARIES}
        ${FREETYPE2_STATIC_LIBRARIES}
        ${FFMPEG_STATIC_LIBRARIES}
        ${CURL_STATIC_LIBRARIES}
        ${WEBP_STATIC_LIBRARIES}
        ${FRIBIDI_STATIC_LIBRARIES}
    )
    list(REMOVE_DUPLICATES PS4_PORTLIB_STATIC_DEPS)
    list(REMOVE_ITEM PS4_PORTLIB_STATIC_DEPS pthread)
    target_link_libraries(stremio PRIVATE
        PkgConfig::FRIBIDI
        ${PS4_PORTLIB_STATIC_DEPS}
        -lkernel
        -lScePad
        -lSceUserService
        -lSceSystemService
        -lSceSysmodule
        -lSceCommonDialog
        -lSceImeDialog
        -lSceGnmDriver
        -lSceNet
        -lSceNetCtl
    )

    # PacBrew's helper also supplies the auth-info block expected by current
    # OpenOrbis homebrew, so do not hand-roll create-fself here.
    if(NOT COMMAND add_self)
        message(FATAL_ERROR "OpenOrbis add_self() helper is unavailable; configure with openorbis-cmake")
    endif()
    add_self(stremio)
    add_custom_target(ps4_pkg
        COMMAND ${CMAKE_COMMAND} -E env OO_PS4_TOOLCHAIN=$ENV{OO_PS4_TOOLCHAIN}
                ${CMAKE_SOURCE_DIR}/ps4/make_pkg.sh
                ${CMAKE_BINARY_DIR}/eboot.bin ${CMAKE_BINARY_DIR}
        COMMENT "Packaging Stremio PS4 .pkg"
        VERBATIM
    )
    add_dependencies(ps4_pkg stremio_self)
endif()
'''
    return s

def patch_main(s: str) -> str:
    if "using ConsolePad = PadPS4" in s:
        return s
    s = replace_once(
        s,
        '#include "app.h"\n#ifdef PLATFORM_PS5\n#include "pad_ps5.h"\n#endif\n',
        '#include "app.h"\n#ifdef PLATFORM_PS5\n#include "pad_ps5.h"\nusing ConsolePad = PadPS5;\n#elif defined(PLATFORM_PS4)\n#include "pad_ps4.h"\n#include "ps4_keyboard.h"\n#include "ps4_runtime.h"\nusing ConsolePad = PadPS4;\n#endif\n',
        "main pad includes",
    )
    s = replace_once(
        s,
        '#ifdef PLATFORM_PS5\n\t(void)argv0;\n\treturn "/app0";  // the title\'s own folder, as every app sees it\n#else\n',
        '#if defined(PLATFORM_PS5) || defined(PLATFORM_PS4)\n\t(void)argv0;\n\treturn "/app0";  // the title\'s own folder\n#else\n',
        "main base dir",
    )
    s = replace_once(
        s,
        '#ifdef PLATFORM_PS5\n\t// The app\'s own storage (param.json downloadDataSize); /data is outside\n\t// the sandbox.\n\treturn "/download0/stremio";\n#else\n',
        '#ifdef PLATFORM_PS5\n\t// The app\'s own storage (param.json downloadDataSize); /data is outside\n\t// the sandbox.\n\treturn "/download0/stremio";\n#elif defined(PLATFORM_PS4)\n\t// Homebrew-friendly persistent storage.\n\treturn "/data/stremio";\n#else\n',
        "main data dir",
    )
    s = replace_once(
        s,
        '#ifndef PLATFORM_PS5  // on the console the system runs one copy of an app\n',
        '#if !defined(PLATFORM_PS5) && !defined(PLATFORM_PS4)  // console launches are single-instance\n',
        "main single instance",
    )
    s = replace_once(
        s,
        'dlog("Stremio for PS5 starting; app files in %s, data in %s", base.c_str(), data.c_str());\n\n#ifdef PLATFORM_PS5_NATIVE\n\tps5_load_modules();  // the on-screen keyboard, before SDL polls it\n\tHwDecoder::load_module();  // the hardware video decoder\n#endif\n',
        '#ifdef PLATFORM_PS4\n\tdlog("Stremio for PS4 starting; app files in %s, data in %s", base.c_str(), data.c_str());\n#else\n\tdlog("Stremio for PS5 starting; app files in %s, data in %s", base.c_str(), data.c_str());\n#endif\n\n#ifdef PLATFORM_PS5_NATIVE\n\tps5_load_modules();  // the on-screen keyboard, before SDL polls it\n\tHwDecoder::load_module();  // the hardware video decoder\n#elif defined(PLATFORM_PS4)\n\tif (!ps4_runtime_init()) {\n\t\tdlog("PS4 network/runtime initialization failed");\n\t\tps4_runtime_exit();\n\t}\n\tps4_keyboard_init();\n\tHwDecoder::load_module();\n#endif\n',
        "main startup",
    )
    s = replace_once(
        s,
        '#ifdef PLATFORM_PS5\n\t// The controller is read by PadPS5, not SDL (see pad_ps5.h).\n\tconst Uint32 sdl_systems = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS;\n#else\n',
        '#if defined(PLATFORM_PS5) || defined(PLATFORM_PS4)\n\t// Console pad is read directly; SDL handles video/audio/events only.\n\tconst Uint32 sdl_systems = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS;\n#else\n',
        "main SDL systems",
    )
    s = replace_once(s, '#ifdef PLATFORM_PS5\n\twflags |= SDL_WINDOW_FULLSCREEN;\n#else\n',
                        '#if defined(PLATFORM_PS5) || defined(PLATFORM_PS4)\n\twflags |= SDL_WINDOW_FULLSCREEN;\n#else\n', "main fullscreen")
    s = replace_once(s, '#ifndef PLATFORM_PS5\n\tww = kWidth * 2 / 3;\n\twh = kHeight * 2 / 3;\n#endif\n',
                        '#if !defined(PLATFORM_PS5) && !defined(PLATFORM_PS4)\n\tww = kWidth * 2 / 3;\n\twh = kHeight * 2 / 3;\n#endif\n', "main console dimensions")
    s = replace_once(
        s,
        '#ifdef PLATFORM_PS5\n\tPadPS5 pad;\n\tbool have_pad = pad.init();\n#endif\n\n\twhile (running && !app.wants_exit()) {\n#ifdef PLATFORM_PS5\n\t\tif (have_pad)\n\t\t\tpad.poll([&](Btn b) { press(app, rep, b); }, [&](Btn b) { release(rep, b); });\n#endif\n',
        '#if defined(PLATFORM_PS5) || defined(PLATFORM_PS4)\n\tConsolePad pad;\n\tbool have_pad = pad.init();\n#endif\n\n\twhile (running && !app.wants_exit()) {\n#if defined(PLATFORM_PS5) || defined(PLATFORM_PS4)\n\t\tif (have_pad)\n\t\t\tpad.poll([&](Btn b) { press(app, rep, b); }, [&](Btn b) { release(rep, b); });\n#endif\n',
        "main console pad loop",
    )
    s = replace_once(
        s,
        'SDL_GetWindowSizeInPixels(window, &sw, &sh);',
        '#ifdef PLATFORM_PS4\n\t\tSDL_GetWindowSize(window, &sw, &sh);\n#else\n\t\tSDL_GetWindowSizeInPixels(window, &sw, &sh);\n#endif',
        "main SDL 2.0.18 window size",
    )
    s = replace_once(
        s,
        '#ifdef PLATFORM_PS5_NATIVE\n\t// On 11.60 an app can\'t end itself: exit() is reported as a crash\n\t// (SIGSYS) and sceSystemServiceLoadExec("exit") fails. Ask the system to\n\t// close us, as the PS menu\'s Close does, and wait for it.\n\tint app_id = sceSystemServiceGetAppIdOfRunningBigApp();\n\tdlog("closing app 0x%x: 0x%x", app_id, sceSystemServiceKillApp(app_id, -1, 0, 0));\n\tfor (;;) SDL_Delay(1000);\n#endif\n\treturn 0;\n',
        '#ifdef PLATFORM_PS5_NATIVE\n\t// On 11.60 an app can\'t end itself: exit() is reported as a crash\n\t// (SIGSYS) and sceSystemServiceLoadExec("exit") fails. Ask the system to\n\t// close us, as the PS menu\'s Close does, and wait for it.\n\tint app_id = sceSystemServiceGetAppIdOfRunningBigApp();\n\tdlog("closing app 0x%x: 0x%x", app_id, sceSystemServiceKillApp(app_id, -1, 0, 0));\n\tfor (;;) SDL_Delay(1000);\n#elif defined(PLATFORM_PS4)\n\tps4_runtime_exit();\n#endif\n\treturn 0;\n',
        "main PS4 clean exit",
    )
    return s


def patch_settings(s: str) -> str:
    if '#include "ps4_keyboard.h"' not in s:
        s = replace_once(s, '#include "app.h"\n', '#include "app.h"\n#ifdef PLATFORM_PS4\n#include "ps4_keyboard.h"\n#endif\n', "settings keyboard include")
    if "PS4 uses SceImeDialog directly" in s:
        return s
    start = s.find('// ---------------------------------------------------------------------------\n// Text entry\n')
    if start < 0:
        die("settings text-entry section not found")
    old = s[start:]
    # This is the final section in current upstream; fail if unexpected material follows it.
    if 'void App::input_poll()' not in old:
        die("settings input_poll not found")
    new = r'''// ---------------------------------------------------------------------------
// Text entry

#ifdef PLATFORM_PS4
// PS4 uses SceImeDialog directly.  The wrapper is synchronous: while the
// system keyboard owns the screen/controller, Stremio waits here and then
// receives the completed UTF-8 string.
void App::open_input(const std::string& title, const std::string& value, const std::string& hint,
                     std::function<void(const std::string&)> done) {
    std::string result;
    if (ps4_keyboard_prompt(title, value, hint, result) && done) done(result);
    dirty_all();
}
void App::input_finish(bool) {}
void App::input_submit() {}
void App::input_poll() {}
#else
// PS5 SDL opens the system IME. It starts empty; Done sends SDL_TEXTINPUT and
// Return, while cancel simply closes the dialog.
void App::open_input(const std::string& title, const std::string& value, const std::string& hint,
                     std::function<void(const std::string&)> done) {
    (void)title;
    (void)value;
    (void)hint;
    input_value.clear();
    input_done_ = done;
    input_visible_ = true;
    input_shown_ = false;
    input_opened_ = now_seconds();
    SDL_StartTextInput();
}

void App::input_finish(bool ok) {
    input_visible_ = false;
    SDL_StopTextInput();
    auto fn = input_done_;
    input_done_ = nullptr;
    std::string v = input_value;
    dirty_all();
    if (ok && fn) fn(v);
}

void App::input_submit() {
    if (input_visible_) input_finish(true);
}

void App::input_poll() {
    if (!input_visible_) return;
    SDL_Window* w = SDL_RenderGetWindow(renderer_);
    bool shown = w && SDL_IsScreenKeyboardShown(w);
    if (shown) {
        input_shown_ = true;
    } else if (input_shown_ || now_seconds() - input_opened_ > 3) {
        input_finish(false);
    }
}
#endif
'''
    return s[:start] + new


def patch_http(s: str) -> str:
    if "Stremio-PS4/1.0" in s:
        return s
    return replace_once(
        s,
        'const char* kUserAgent = "Mozilla/5.0 (PlayStation; PlayStation 5/1.00) Stremio-PS5/1.0";\n',
        '#ifdef PLATFORM_PS4\nconst char* kUserAgent = "Mozilla/5.0 (PlayStation 4) Stremio-PS4/1.0";\n#else\nconst char* kUserAgent = "Mozilla/5.0 (PlayStation; PlayStation 5/1.00) Stremio-PS5/1.0";\n#endif\n',
        "HTTP user agent",
    )



def patch_util(s: str) -> str:
    if "defined(PLATFORM_PS5) || defined(PLATFORM_PS4)" in s:
        return s
    n = s.count('#ifdef PLATFORM_PS5')
    if n != 2:
        die(f"util logging: expected 2 PLATFORM_PS5 guards, found {n}")
    return s.replace('#ifdef PLATFORM_PS5', '#if defined(PLATFORM_PS5) || defined(PLATFORM_PS4)')

def patch_torrent(s: str) -> str:
    if "av_sha_alloc" in s and "PLATFORM_PS4" in s[:1500]:
        return s
    s = replace_once(
        s,
        '#include <openssl/rand.h>\n#include <openssl/sha.h>\n',
        '#ifdef PLATFORM_PS4\nextern "C" {\n#include <libavutil/mem.h>\n#include <libavutil/random_seed.h>\n#include <libavutil/sha.h>\n}\n#else\n#include <openssl/rand.h>\n#include <openssl/sha.h>\n#endif\n',
        "torrent SHA includes",
    )
    # sha1_digest lives in namespace bt (not the anonymous helper namespace),
    # so the DHT callbacks defined later in the translation unit can call it.
    s = replace_once(
        s,
        'namespace bt {\n\nnamespace {\n',
        'namespace bt {\n\nvoid sha1_digest(const void* data, size_t n, unsigned char out[20]) {\n#ifdef PLATFORM_PS4\n\tAVSHA* ctx = av_sha_alloc();\n\tif (!ctx) { memset(out, 0, 20); return; }\n\tav_sha_init(ctx, 160);\n\tav_sha_update(ctx, static_cast<const uint8_t*>(data), n);\n\tav_sha_final(ctx, out);\n\tav_free(ctx);\n#else\n\tSHA1(static_cast<const unsigned char*>(data), n, out);\n#endif\n}\n\nnamespace {\n',
        "torrent SHA helper",
    )
    old_random = '''void random_bytes(void* buf, size_t n) {\n\tif (RAND_bytes(static_cast<unsigned char*>(buf), int(n)) != 1) {\n\t\tauto* p = static_cast<uint8_t*>(buf);\n\t\tfor (size_t i = 0; i < n; i++) p[i] = uint8_t(rand());\n\t}\n}\n'''
    new_random = '''void random_bytes(void* buf, size_t n) {\n#ifdef PLATFORM_PS4\n\tauto* p = static_cast<uint8_t*>(buf);\n\twhile (n) {\n\t\tuint32_t v = av_get_random_seed();\n\t\tsize_t k = std::min(n, sizeof(v));\n\t\tmemcpy(p, &v, k);\n\t\tp += k;\n\t\tn -= k;\n\t}\n#else\n\tif (RAND_bytes(static_cast<unsigned char*>(buf), int(n)) != 1) {\n\t\tauto* p = static_cast<uint8_t*>(buf);\n\t\tfor (size_t i = 0; i < n; i++) p[i] = uint8_t(rand());\n\t}\n#endif\n}\n'''
    s = replace_once(s, old_random, new_random, "torrent random helper")
    # Replace the four SHA1 calls used by the torrent engine.  Keep the
    # OpenSSL call inside sha1_digest() itself untouched.
    exact_calls = {
        'SHA1(reinterpret_cast<const unsigned char*>(t->meta.data()), t->meta.size(), digest);':
            'sha1_digest(t->meta.data(), t->meta.size(), digest);',
        'SHA1(reinterpret_cast<const unsigned char*>(pc.data.data()), pc.data.size(), digest);':
            'sha1_digest(pc.data.data(), pc.data.size(), digest);',
        'SHA1(reinterpret_cast<const unsigned char*>(saved.data()), saved.size(), digest);':
            'sha1_digest(saved.data(), saved.size(), digest);',
        'SHA1(reinterpret_cast<const unsigned char*>(all.data()), all.size(), d);':
            'bt::sha1_digest(all.data(), all.size(), d);',
    }
    for old, new in exact_calls.items():
        if s.count(old) != 1:
            die(f"torrent SHA call marker expected once: {old[:48]}")
        s = s.replace(old, new, 1)
    return s


def patch_player(s: str) -> str:
    if "PS4_FFMPEG_LEGACY_CHANNEL_LAYOUT" in s:
        return s

    s = replace_once(
        s,
        '#include <libavutil/time.h>\n',
        '#include <libavutil/time.h>\n#include <libavutil/version.h>\n',
        "player libavutil version include",
    )
    s = replace_once(
        s,
        '}\n\n#include <algorithm>\n',
        '}\n\n#if defined(PLATFORM_PS4) && LIBAVUTIL_VERSION_INT < AV_VERSION_INT(57, 24, 100)\n#define PS4_FFMPEG_LEGACY_CHANNEL_LAYOUT 1\n#endif\n\n#include <algorithm>\n',
        "player FFmpeg legacy API selector",
    )

    old = '''\tif (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {\n\t\t// The PS5 has 8 Zen 2 cores; FFmpeg can't count them here, so say it.\n\t\tctx->thread_count = 8;\n\t\tctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;\n\t}\n'''
    new = '''\tif (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {\n#ifdef PLATFORM_PS4\n\t\t// PS4 has 8 Jaguar cores; leave some CPU for UI/audio/network work.\n\t\tctx->thread_count = 6;\n#else\n\t\t// The PS5 has 8 Zen 2 cores; FFmpeg can't count them here, so say it.\n\t\tctx->thread_count = 8;\n#endif\n\t\tctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;\n\t}\n'''
    s = replace_once(s, old, new, "player FFmpeg thread count")

    # Older PacBrew images ship FFmpeg 5.0; modern ones can ship FFmpeg 7.x. Stremio upstream uses the channel
    # layout API introduced at libavutil 57.24.100, so select the old API only
    # when the PS4 sysroot is actually older than that.
    s = replace_once(
        s,
        '''\tint swr_rate = 0, swr_fmt = -1;\n\tAVChannelLayout swr_layout;\n\tmemset(&swr_layout, 0, sizeof(swr_layout));\n''',
        '''\tint swr_rate = 0, swr_fmt = -1;\n#ifdef PS4_FFMPEG_LEGACY_CHANNEL_LAYOUT\n// PS4_FFMPEG_LEGACY_CHANNEL_LAYOUT\n\tuint64_t swr_layout = 0;\n#else\n\tAVChannelLayout swr_layout;\n\tmemset(&swr_layout, 0, sizeof(swr_layout));\n#endif\n''',
        "player legacy FFmpeg swr state",
    )

    old_audio = '''\t\t\tAVChannelLayout in_layout;\n\t\t\tif (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || frame->ch_layout.nb_channels == 0)\n\t\t\t\tav_channel_layout_default(&in_layout, frame->ch_layout.nb_channels ? frame->ch_layout.nb_channels : 2);\n\t\t\telse\n\t\t\t\tav_channel_layout_copy(&in_layout, &frame->ch_layout);\n\t\t\tif (!swr_ || swr_rate != frame->sample_rate || swr_fmt != frame->format ||\n\t\t\t    av_channel_layout_compare(&swr_layout, &in_layout) != 0) {\n\t\t\t\tif (swr_) swr_free(&swr_);\n\t\t\t\tAVChannelLayout stereo;\n\t\t\t\tav_channel_layout_default(&stereo, 2);\n\t\t\t\tswr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_S16, kOutRate, &in_layout,\n\t\t\t\t                    AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr);\n\t\t\t\tif (!swr_ || swr_init(swr_) < 0) {\n\t\t\t\t\tdlog("player: can't resample audio");\n\t\t\t\t\tif (swr_) swr_free(&swr_);\n\t\t\t\t\tav_channel_layout_uninit(&in_layout);\n\t\t\t\t\tav_frame_unref(frame);\n\t\t\t\t\tcontinue;\n\t\t\t\t}\n\t\t\t\tswr_rate = frame->sample_rate;\n\t\t\t\tswr_fmt = frame->format;\n\t\t\t\tav_channel_layout_uninit(&swr_layout);\n\t\t\t\tav_channel_layout_copy(&swr_layout, &in_layout);\n\t\t\t}\n\t\t\tav_channel_layout_uninit(&in_layout);\n'''
    new_audio = '''#ifdef PS4_FFMPEG_LEGACY_CHANNEL_LAYOUT\n\t\t\tint in_channels = frame->channels > 0 ? frame->channels : 2;\n\t\t\tuint64_t in_layout = frame->channel_layout;\n\t\t\tif (!in_layout) in_layout = uint64_t(av_get_default_channel_layout(in_channels));\n\t\t\tif (!swr_ || swr_rate != frame->sample_rate || swr_fmt != frame->format || swr_layout != in_layout) {\n\t\t\t\tif (swr_) swr_free(&swr_);\n\t\t\t\tswr_ = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, kOutRate,\n\t\t\t\t                          int64_t(in_layout), AVSampleFormat(frame->format),\n\t\t\t\t                          frame->sample_rate, 0, nullptr);\n\t\t\t\tif (!swr_ || swr_init(swr_) < 0) {\n\t\t\t\t\tdlog("player: can't resample audio");\n\t\t\t\t\tif (swr_) swr_free(&swr_);\n\t\t\t\t\tav_frame_unref(frame);\n\t\t\t\t\tcontinue;\n\t\t\t\t}\n\t\t\t\tswr_rate = frame->sample_rate;\n\t\t\t\tswr_fmt = frame->format;\n\t\t\t\tswr_layout = in_layout;\n\t\t\t}\n#else\n\t\t\tAVChannelLayout in_layout;\n\t\t\tif (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || frame->ch_layout.nb_channels == 0)\n\t\t\t\tav_channel_layout_default(&in_layout, frame->ch_layout.nb_channels ? frame->ch_layout.nb_channels : 2);\n\t\t\telse\n\t\t\t\tav_channel_layout_copy(&in_layout, &frame->ch_layout);\n\t\t\tif (!swr_ || swr_rate != frame->sample_rate || swr_fmt != frame->format ||\n\t\t\t    av_channel_layout_compare(&swr_layout, &in_layout) != 0) {\n\t\t\t\tif (swr_) swr_free(&swr_);\n\t\t\t\tAVChannelLayout stereo;\n\t\t\t\tav_channel_layout_default(&stereo, 2);\n\t\t\t\tswr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_S16, kOutRate, &in_layout,\n\t\t\t\t                    AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr);\n\t\t\t\tif (!swr_ || swr_init(swr_) < 0) {\n\t\t\t\t\tdlog("player: can't resample audio");\n\t\t\t\t\tif (swr_) swr_free(&swr_);\n\t\t\t\t\tav_channel_layout_uninit(&in_layout);\n\t\t\t\t\tav_frame_unref(frame);\n\t\t\t\t\tcontinue;\n\t\t\t\t}\n\t\t\t\tswr_rate = frame->sample_rate;\n\t\t\t\tswr_fmt = frame->format;\n\t\t\t\tav_channel_layout_uninit(&swr_layout);\n\t\t\t\tav_channel_layout_copy(&swr_layout, &in_layout);\n\t\t\t}\n\t\t\tav_channel_layout_uninit(&in_layout);\n#endif\n'''
    s = replace_once(s, old_audio, new_audio, "player legacy FFmpeg audio layout")

    s = replace_once(
        s,
        '''\tav_channel_layout_uninit(&swr_layout);\n\tif (pkt) av_packet_free(&pkt);\n''',
        '''#ifndef PS4_FFMPEG_LEGACY_CHANNEL_LAYOUT\n\tav_channel_layout_uninit(&swr_layout);\n#endif\n\tif (pkt) av_packet_free(&pkt);\n''',
        "player legacy FFmpeg swr cleanup",
    )

    s = replace_once(
        s,
        '''\tif (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {\n\t\tint ch = st->codecpar->ch_layout.nb_channels;\n''',
        '''\tif (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {\n#ifdef PS4_FFMPEG_LEGACY_CHANNEL_LAYOUT\n\t\tint ch = st->codecpar->channels;\n#else\n\t\tint ch = st->codecpar->ch_layout.nb_channels;\n#endif\n''',
        "player legacy FFmpeg track channels",
    )
    return s

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("repo", nargs="?", default=".", help="path to upstream Stremio PS5 repository")
    args = ap.parse_args()
    root = Path(args.repo).resolve()
    if not (root / "CMakeLists.txt").is_file() or not (root / "src" / "main.cpp").is_file():
        die(f"{root} does not look like Sp9nky/unofficial-stremio-ps5-port")

    # Copy PS4-specific source files and native build tooling. Existing files
    # are replaced so reapplying an updated port is deterministic.
    for src in sorted(PS4_SRC.rglob("*")):
        if not src.is_file():
            continue
        rel = Path("src") / src.relative_to(PS4_SRC)
        dst = root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        print(f"+ {rel}")

    for src in sorted(PS4_BUILD.rglob("*")):
        if not src.is_file():
            continue
        rel = Path("ps4") / src.relative_to(PS4_BUILD)
        dst = root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        print(f"+ {rel}")

    patch_file(root / "CMakeLists.txt", patch_cmake)
    patch_file(root / "src" / "main.cpp", patch_main)
    patch_file(root / "src" / "app_settings.cpp", patch_settings)
    patch_file(root / "src" / "http.cpp", patch_http)
    patch_file(root / "src" / "util.cpp", patch_util)
    patch_file(root / "src" / "torrent" / "engine.cpp", patch_torrent)
    patch_file(root / "src" / "player.cpp", patch_player)

    for p in (root / "ps4").glob("*.sh"):
        p.chmod(p.stat().st_mode | 0o111)

    print("\nPS4 overlay applied.")
    print("Next: bash ps4/build.sh")


if __name__ == "__main__":
    main()
