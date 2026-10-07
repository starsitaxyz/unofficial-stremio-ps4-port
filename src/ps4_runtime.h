#pragma once

#ifdef PLATFORM_PS4
bool ps4_runtime_init();
[[noreturn]] void ps4_runtime_exit();
#endif
