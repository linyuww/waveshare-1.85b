#pragma once
#include <cctype>
#include <cstring>

namespace xiaozhi_protocol {
inline bool deviceId(const char *mac, char (&output)[18])
{
    output[0] = 0;
    if (!mac || strlen(mac) != 17) return false;
    for (size_t index = 0; index < 17; ++index) {
        const auto byte = static_cast<unsigned char>(mac[index]);
        if (index % 3 == 2 ? byte != ':' : !std::isxdigit(byte)) return false;
    }
    for (size_t index = 0; index < 17; ++index) output[index] = static_cast<char>(std::tolower(static_cast<unsigned char>(mac[index])));
    output[17] = 0;
    return true;
}
}
