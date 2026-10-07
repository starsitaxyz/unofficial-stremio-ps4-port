#pragma once

#include <functional>
#include <cstdint>

#include "app.h"

// Direct DualShock 4 input for OpenOrbis. SDL's PS4 controller path varies
// between builds, so Stremio reads libScePad directly just like the PS5 port.
class PadPS4 {
public:
    bool init();
    ~PadPS4();

    void poll(const std::function<void(Btn)>& down,
              const std::function<void(Btn)>& up);

private:
    void release_all(const std::function<void(Btn)>& up);

    int handle_ = -1;
    int user_ = -1;
    uint32_t held_ = 0;
};
