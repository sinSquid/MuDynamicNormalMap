#pragma once
#include <string>
#include <string_view>

namespace PNS::SkinPaths {
    inline std::string Relative(std::string_view path) {
        std::string value(path);
        for (char& ch : value) {
            if (ch == '/') ch = '\\';
            if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        }
        if (value.starts_with("data\\")) value.erase(0, 5);
        if (value.starts_with("textures\\")) value.erase(0, 9);
        return value;
    }
    // Polling visits many ordinary skin slots. Match the directory without allocating.
    inline bool IsPackPath(std::string_view path) {
        auto consume = [&](std::string_view prefix) {
            if (path.size() < prefix.size()) return false;
            for (std::size_t i = 0; i < prefix.size(); ++i) {
                char c = path[i];
                if (c == '/') c = '\\';
                if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (c != prefix[i]) return false;
            }
            path.remove_prefix(prefix.size());
            return true;
        };
        consume("data\\");
        consume("textures\\");
        return consume("oskin\\");
    }

    // Compare the captured source of a background job with the current texture set.
    inline bool Stale(std::string_view captured, std::string_view current) {
        return (IsPackPath(captured) || IsPackPath(current)) && Relative(captured) != Relative(current);
    }
}

