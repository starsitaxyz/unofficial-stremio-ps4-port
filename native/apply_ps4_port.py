#!/usr/bin/env python3
"""Apply the tested PS4/OpenOrbis port to a clean upstream checkout.

This wrapper first applies the base PS4 overlay and then the real-hardware
fixes discovered while building/testing on PS4:
- OpenOrbis/SDL2 pthread linker normalization
- lazy IME/CommonDialog startup
- SDL software renderer for correct RmlUi output
- OpenOrbis libc++ math compatibility
- correct libkernel declaration on PS4
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def fail(msg: str) -> None:
    raise SystemExit(f"apply_ps4_port.py: {msg}")


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    s = path.read_text(encoding="utf-8")
    n = s.count(old)
    if n != 1:
        fail(f"{label}: expected marker once in {path}, found {n}")
    path.write_text(s.replace(old, new, 1), encoding="utf-8")
    print(f"~ {path}: {label}")


def patch_cmake(root: Path) -> None:
    p = root / "CMakeLists.txt"
    s = p.read_text(encoding="utf-8")
    if "PS4_REAL_HW_PTHREAD_FIX" in s:
        return
    s += r'''

# PS4_REAL_HW_PTHREAD_FIX
# OpenOrbis links the final ELF with ld.lld. libpthread.a exists, but the
# clang-driver-only switch -pthread is rejected by ld.lld. PacBrew SDL2 can
# export that switch transitively, so strip it and link pthread normally.
if(PS4)
    if(TARGET Threads::Threads)
        set_property(TARGET Threads::Threads PROPERTY INTERFACE_COMPILE_OPTIONS "")
        set_property(TARGET Threads::Threads PROPERTY INTERFACE_LINK_OPTIONS "")
        set_property(TARGET Threads::Threads PROPERTY INTERFACE_LINK_LIBRARIES pthread)
    endif()
    foreach(_ps4_sdl_target PkgConfig::SDL2_PS4 SDL2::SDL2)
        if(TARGET ${_ps4_sdl_target})
            get_target_property(_ps4_link_opts ${_ps4_sdl_target} INTERFACE_LINK_OPTIONS)
            if(_ps4_link_opts AND NOT _ps4_link_opts MATCHES "NOTFOUND")
                list(REMOVE_ITEM _ps4_link_opts "-pthread")
                set_property(TARGET ${_ps4_sdl_target} PROPERTY INTERFACE_LINK_OPTIONS "${_ps4_link_opts}")
            endif()
            get_target_property(_ps4_link_libs ${_ps4_sdl_target} INTERFACE_LINK_LIBRARIES)
            if(_ps4_link_libs AND NOT _ps4_link_libs MATCHES "NOTFOUND")
                list(REMOVE_ITEM _ps4_link_libs "-pthread")
                set_property(TARGET ${_ps4_sdl_target} PROPERTY INTERFACE_LINK_LIBRARIES "${_ps4_link_libs}")
            endif()
        endif()
    endforeach()
endif()
'''
    p.write_text(s, encoding="utf-8")
    print(f"~ {p}: PS4 pthread linker fix")


def patch_main(root: Path) -> None:
    p = root / "src/main.cpp"
    s = p.read_text(encoding="utf-8")
    if "PS4_REAL_HW_SOFTWARE_RENDERER" in s:
        return

    old = '''#elif defined(PLATFORM_PS4)\n\tif (!ps4_runtime_init()) {\n\t\tdlog("PS4 network/runtime initialization failed");\n\t\tps4_runtime_exit();\n\t}\n\tps4_keyboard_init();\n\tHwDecoder::load_module();\n#endif\n'''
    new = '''#elif defined(PLATFORM_PS4)\n\tif (!ps4_runtime_init()) {\n\t\tdlog("PS4 network/runtime initialization failed");\n\t\tps4_runtime_exit();\n\t}\n\t// Initialise CommonDialog/IME lazily. Initialising it during startup caused\n\t// CE-34878-0 on the physical PS4 used for validation.\n\tdlog("startup: before hwdec");\n\tHwDecoder::load_module();\n\tdlog("startup: after hwdec");\n#endif\n#ifdef PLATFORM_PS4\n\tdlog("startup: before SDL");\n#endif\n'''
    if old not in s:
        fail("PS4 startup block not found")
    s = s.replace(old, new, 1)

    old = '\tif (SDL_Init(sdl_systems) != 0) {\n'
    new = '#ifdef PLATFORM_PS4\n\tdlog("startup: SDL_Init begin");\n#endif\n\tif (SDL_Init(sdl_systems) != 0) {\n'
    if old not in s:
        fail("SDL_Init marker not found")
    s = s.replace(old, new, 1)

    old = '\tUint32 wflags = SDL_WINDOW_SHOWN;\n'
    new = '#ifdef PLATFORM_PS4\n\tdlog("startup: SDL_Init OK");\n#endif\n\tUint32 wflags = SDL_WINDOW_SHOWN;\n'
    if old not in s:
        fail("window flags marker not found")
    s = s.replace(old, new, 1)

    old = '\tSDL_Window* window = SDL_CreateWindow("Stremio", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, wflags);\n'
    new = '#ifdef PLATFORM_PS4\n\tdlog("startup: creating SDL window");\n#endif\n' + old
    if old not in s:
        fail("SDL_CreateWindow marker not found")
    s = s.replace(old, new, 1)

    old = '''\tSDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);\n\tif (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);\n'''
    new = '''\tSDL_Renderer* renderer = nullptr;\n#ifdef PLATFORM_PS4\n\t// PS4_REAL_HW_SOFTWARE_RENDERER\n\t// Proven on real PS4 hardware. The OpenOrbis GLES2 renderer corrupted this\n\t// RmlUi backend (white quads, square glyphs and stray triangle edges).\n\tdlog("renderer: trying PS4 software renderer");\n\trenderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);\n\tif (!renderer) dlog("renderer: software failed: %s", SDL_GetError());\n\tif (!renderer) {\n\t\tdlog("renderer: falling back to accelerated renderer");\n\t\trenderer = SDL_CreateRenderer(window, -1, 0);\n\t}\n#else\n\trenderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);\n\tif (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);\n#endif\n'''
    if old not in s:
        fail("SDL renderer marker not found")
    s = s.replace(old, new, 1)

    p.write_text(s, encoding="utf-8")
    print(f"~ {p}: real-hardware startup + software renderer")


def patch_util(root: Path) -> None:
    p = root / "src/util.cpp"
    s = p.read_text(encoding="utf-8")
    # Base overlay broadens the PS5 guard to PS4, which makes the old manual
    # int declaration conflict with OpenOrbis' int32_t declaration.
    old = '''#if defined(PLATFORM_PS5) || defined(PLATFORM_PS4)\nextern "C" int sceKernelDebugOutText(int channel, const char* text);\n#endif\n'''
    new = '''#ifdef PLATFORM_PS5\nextern "C" int sceKernelDebugOutText(int channel, const char* text);\n#elif defined(PLATFORM_PS4)\n#include <orbis/libkernel.h>\n#endif\n'''
    if old in s:
        s = s.replace(old, new, 1)
    elif '#include <orbis/libkernel.h>' not in s:
        fail("util.cpp debug declaration marker not found")
    p.write_text(s, encoding="utf-8")
    print(f"~ {p}: OpenOrbis libkernel declaration")


def patch_yuv(root: Path) -> None:
    p = root / "src/yuv_convert.cpp"
    s = p.read_text(encoding="utf-8")
    if '#include <math.h>' not in s:
        old = '#include <cmath>\n#include <mutex>\n'
        new = '#include <math.h>\n#include <algorithm>\n#include <mutex>\n'
        if old not in s:
            fail("yuv_convert.cpp math include marker not found")
        s = s.replace(old, new, 1)
    s = s.replace('std::pow(', '::powf(')
    s = s.replace('std::sqrt(', '::sqrtf(')
    s = s.replace('std::lround(', '::lroundf(')
    p.write_text(s, encoding="utf-8")
    print(f"~ {p}: OpenOrbis float math")


def main() -> None:
    repo = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    subprocess.run([sys.executable, str(HERE / "apply_ps4_port_base.py"), str(repo)], check=True)
    patch_cmake(repo)
    patch_main(repo)
    patch_util(repo)
    patch_yuv(repo)
    print("\nReal-hardware PS4 fixes applied.")
    print("Next: bash ps4/build.sh")


if __name__ == "__main__":
    main()
