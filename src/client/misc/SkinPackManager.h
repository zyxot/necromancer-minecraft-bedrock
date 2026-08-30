#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace SkinPack {
    struct SkinEntry {
        std::string fileName;
        std::string localization;
        std::string geometry;
        std::filesystem::path fullPath;
        std::filesystem::path geometrySource;
        uint64_t fileSize = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool slim = false;
    };

    std::filesystem::path getSkinsFolder();
    std::filesystem::path findMojangDir();
    void ensureFolder();
    std::vector<SkinEntry> scan();
    bool sync(std::string& report);
    void openFolder();
}
