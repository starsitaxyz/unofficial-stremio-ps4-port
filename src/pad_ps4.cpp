#include "pad_ps4.h"

#include <orbis/Pad.h>
#include <orbis/UserService.h>

#include <cstring>

#include "util.h"

namespace {

struct ButtonMap {
    uint32_t mask;
    Btn btn;
};

static const ButtonMap kButtons[] = {
    {ORBIS_PAD_BUTTON_CROSS, Btn::Cross},
    {ORBIS_PAD_BUTTON_CIRCLE, Btn::Circle},
    {ORBIS_PAD_BUTTON_SQUARE, Btn::Square},
    {ORBIS_PAD_BUTTON_TRIANGLE, Btn::Triangle},
    {ORBIS_PAD_BUTTON_OPTIONS, Btn::Options},
    {ORBIS_PAD_BUTTON_L1, Btn::L1},
    {ORBIS_PAD_BUTTON_R1, Btn::R1},
    {ORBIS_PAD_BUTTON_L2, Btn::L2},
    {ORBIS_PAD_BUTTON_R2, Btn::R2},
    {ORBIS_PAD_BUTTON_L3, Btn::L3},
    {ORBIS_PAD_BUTTON_R3, Btn::R3},
    {ORBIS_PAD_BUTTON_TOUCH_PAD, Btn::Touchpad},
    {ORBIS_PAD_BUTTON_UP, Btn::Up},
    {ORBIS_PAD_BUTTON_DOWN, Btn::Down},
    {ORBIS_PAD_BUTTON_LEFT, Btn::Left},
    {ORBIS_PAD_BUTTON_RIGHT, Btn::Right},
};

inline uint32_t bit(Btn b) { return 1u << unsigned(b); }

bool axis_pressed(uint8_t value, bool negative, bool held) {
    const int d = negative ? 128 - int(value) : int(value) - 128;
    return d > (held ? 50 : 80);
}

} // namespace

bool PadPS4::init() {
    int rc = sceUserServiceInitialize(nullptr);
    if (rc < 0) dlog("ps4 pad: sceUserServiceInitialize: 0x%08x", unsigned(rc));

    int32_t uid = 0;
    rc = sceUserServiceGetInitialUser(&uid);
    if (rc < 0) {
        dlog("ps4 pad: sceUserServiceGetInitialUser: 0x%08x", unsigned(rc));
        return false;
    }
    user_ = uid;

    rc = scePadInit();
    if (rc < 0) {
        dlog("ps4 pad: scePadInit: 0x%08x", unsigned(rc));
        return false;
    }

    handle_ = scePadOpen(uid, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    if (handle_ < 0) {
        dlog("ps4 pad: scePadOpen: 0x%08x", unsigned(handle_));
        return false;
    }
    dlog("ps4 pad: opened handle %d user %d", handle_, user_);
    return true;
}

PadPS4::~PadPS4() {
    if (handle_ >= 0) scePadClose(handle_);
}

void PadPS4::release_all(const std::function<void(Btn)>& up) {
    for (unsigned i = 0; i < 32; ++i) {
        if (held_ & (1u << i)) up(Btn(i));
    }
    held_ = 0;
}

void PadPS4::poll(const std::function<void(Btn)>& down,
                  const std::function<void(Btn)>& up) {
    if (handle_ < 0) return;

    OrbisPadData pad;
    std::memset(&pad, 0, sizeof(pad));
    const int rc = scePadReadState(handle_, &pad);
    if (rc < 0) {
        release_all(up);
        return;
    }

    uint32_t want = 0;
    for (const auto& m : kButtons)
        if (pad.buttons & m.mask) want |= bit(m.btn);

    auto held = [&](Btn b) { return (held_ & bit(b)) != 0; };
    if (axis_pressed(pad.leftStick.y, true, held(Btn::Up)) ||
        axis_pressed(pad.rightStick.y, true, held(Btn::Up))) want |= bit(Btn::Up);
    if (axis_pressed(pad.leftStick.y, false, held(Btn::Down)) ||
        axis_pressed(pad.rightStick.y, false, held(Btn::Down))) want |= bit(Btn::Down);
    if (axis_pressed(pad.leftStick.x, true, held(Btn::Left)) ||
        axis_pressed(pad.rightStick.x, true, held(Btn::Left))) want |= bit(Btn::Left);
    if (axis_pressed(pad.leftStick.x, false, held(Btn::Right)) ||
        axis_pressed(pad.rightStick.x, false, held(Btn::Right))) want |= bit(Btn::Right);

    const uint32_t changed = want ^ held_;
    for (unsigned i = 0; i < 32; ++i) {
        if (!(changed & (1u << i))) continue;
        Btn b = Btn(i);
        if (want & (1u << i)) down(b);
        else up(b);
    }
    held_ = want;
}
