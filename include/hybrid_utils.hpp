#pragma once
#include <string>
#include <algorithm>

static std::string convertOkxToBinance(const std::string& okxSymbol) {
    std::string s = okxSymbol;
    // Удаляем суффикс "-SWAP"
    size_t pos = s.find("-SWAP");
    if (pos != std::string::npos) s.erase(pos, 5);
    // Удаляем оставшиеся дефисы
    s.erase(std::remove(s.begin(), s.end(), '-'), s.end());
    return s;
}