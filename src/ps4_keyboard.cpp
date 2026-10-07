#include "ps4_keyboard.h"

#include <orbis/CommonDialog.h>
#include <orbis/ImeDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <vector>

#include "util.h"

namespace {
constexpr size_t kMaxText = 512;
bool g_ready = false;
int32_t g_user = 0;

void utf8_to_u16(const std::string& s, uint16_t* out, size_t cap) {
    size_t oi = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(s.c_str());
    while (*p && oi + 1 < cap) {
        uint32_t cp = '?';
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p >> 5) == 0x6 && p[1]) {
            cp = ((*p & 0x1f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else if ((*p >> 4) == 0xe && p[1] && p[2]) {
            cp = ((*p & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            p += 3;
        } else {
            ++p;
        }
        if (cp > 0xffff) cp = '?';
        out[oi++] = uint16_t(cp);
    }
    out[oi] = 0;
}

std::string u16_to_utf8(const uint16_t* in) {
    std::string out;
    for (size_t i = 0; in[i]; ++i) {
        uint32_t cp = in[i];
        if (cp < 0x80) out.push_back(char(cp));
        else if (cp < 0x800) {
            out.push_back(char(0xc0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(char(0xe0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(char(0x80 | (cp & 0x3f)));
        }
    }
    return out;
}
} // namespace

bool ps4_keyboard_init() {
    if (g_ready) return true;

    dlog("ps4 keyboard: init begin");

    int32_t rc = sceSysmoduleLoadModuleInternal(
        ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG);
    dlog("ps4 keyboard: load CommonDialog -> 0x%08x", unsigned(rc));

    rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
    dlog("ps4 keyboard: load ImeDialog -> 0x%08x", unsigned(rc));

    OrbisUserServiceInitializeParams user_params{};
    user_params.priority = ORBIS_KERNEL_PRIO_FIFO_NORMAL;
    rc = sceUserServiceInitialize(&user_params);
    dlog("ps4 keyboard: UserServiceInitialize -> 0x%08x", unsigned(rc));

    rc = sceUserServiceGetInitialUser(&g_user);
    dlog("ps4 keyboard: GetInitialUser -> 0x%08x user=%d",
         unsigned(rc), int(g_user));
    if (rc != 0) {
        dlog("ps4 keyboard: no initial user; IME disabled");
        return false;
    }

    rc = sceCommonDialogInitialize();
    dlog("ps4 keyboard: CommonDialogInitialize -> 0x%08x", unsigned(rc));

    g_ready = true;
    dlog("ps4 keyboard: ready");
    return true;
}

bool ps4_keyboard_prompt(const std::string& title,
                         const std::string& initial,
                         const std::string& hint,
                         std::string& out) {
    if (!g_ready && !ps4_keyboard_init()) return false;

    std::vector<uint16_t> text(kMaxText, 0);
    uint16_t title16[128] = {};
    uint16_t hint16[128] = {};
    utf8_to_u16(initial, text.data(), text.size());
    utf8_to_u16(title.empty() ? "Enter text" : title, title16, 128);
    utf8_to_u16(hint, hint16, 128);

    OrbisImeDialogSetting p;
    std::memset(&p, 0, sizeof(p));
    p.userId = uint32_t(g_user);
    p.type = ORBIS_TYPE_DEFAULT;
    p.supportedLanguages = 0;
    p.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    p.inputMethod = ORBIS__DEFAULT;
    p.filter = nullptr;
    p.option = 0;
    p.maxTextLength = uint32_t(kMaxText - 1);
    p.inputTextBuffer = reinterpret_cast<wchar_t*>(text.data());
    p.posx = 0.0f;
    p.posy = 0.0f;
    p.horizontalAlignment = ORBIS_H_CENTER;
    p.verticalAlignment = ORBIS_V_CENTER;
    p.placeholder = reinterpret_cast<const wchar_t*>(hint16);
    p.title = reinterpret_cast<const wchar_t*>(title16);

    int rc = sceImeDialogInit(&p, nullptr);
    if (rc != 0) {
        dlog("ps4 keyboard: init failed 0x%08x", unsigned(rc));
        return false;
    }

    timespec ts{0, 10 * 1000 * 1000};
    OrbisDialogStatus st;
    while ((st = sceImeDialogGetStatus()) == ORBIS_DIALOG_STATUS_RUNNING)
        nanosleep(&ts, nullptr);

    bool accepted = false;
    if (st == ORBIS_DIALOG_STATUS_STOPPED) {
        OrbisDialogResult res;
        std::memset(&res, 0, sizeof(res));
        sceImeDialogGetResult(&res);
        if (res.endstatus == ORBIS_DIALOG_OK) {
            out = u16_to_utf8(text.data());
            accepted = true;
        }
    }
    sceImeDialogTerm();
    return accepted;
}
