#pragma once

#include <string>

bool ps4_keyboard_init();
bool ps4_keyboard_prompt(const std::string& title,
                         const std::string& initial,
                         const std::string& hint,
                         std::string& out);
