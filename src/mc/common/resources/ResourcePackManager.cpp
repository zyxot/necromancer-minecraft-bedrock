#include "pch.h"
#include "ResourcePackManager.h"
#include "util/Util.h"

#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>

namespace SDK {
    namespace detail {
        void iconDiag(char const* tag, std::string const& detail) {
            static std::mutex diagMutex;
            static std::set<std::pair<std::string, std::string>> logged;
            static std::ofstream file;
            static size_t lines = 0;

            std::lock_guard lock { diagMutex };
            if (!logged.emplace(tag, detail).second) return;
            if (lines >= 500) return;
            if (!file.is_open()) {
                std::error_code ec;
                auto dir = util::GetNecromancerPath() / "Logs";
                std::filesystem::create_directories(dir, ec);
                file.open(dir / "icon_diag.txt", std::ios::trunc);
            }
            if (!file.is_open()) return;
            ++lines;
            file << "[" << tag << "] " << detail << "\n";
            file.flush();
        }
    }

    bool loadPackResourceFrom(void* manager, std::string const& path, std::string& out) {
        out.clear();
        if (!manager || path.empty()) return false;
        if (!Signatures::ResourcePack_getResource.result) {
            detail::iconDiag("sig", "ResourcePack_getResource NOT RESOLVED");
            return false;
        }

        auto count = detail::validateLayout(manager);
        if (!count) {
            detail::iconDiag("stacks", "layout invalid mid-session");
            return false;
        }

        using Rpm = Signatures::FieldOffset::ResourcePackManager;
        auto* holder = *reinterpret_cast<void**>(static_cast<char*>(manager) + Rpm::stacksHolder);
        auto* begin = *reinterpret_cast<char**>(static_cast<char*>(holder) + Rpm::stacksBegin);

        size_t faults = 0;
        bool faulted = false;
        for (size_t i = 0; i < count; ++i) {
            void* stack = begin + i * Rpm::stackStride;
            if (!detail::readable(stack, Rpm::stackPackIndex + 4)) {
                ++faults;
                continue;
            }
            std::string buffer;
            if (detail::sehReadStack(stack, &path, &buffer, &faulted) && !buffer.empty()) {
                detail::iconDiag("walk", path + ": ok stack=" + std::to_string(i) +
                                             " bytes=" + std::to_string(buffer.size()));
                out = std::move(buffer);
                return true;
            }
            if (faulted) {
                detail::iconDiag("walk", path + ": SEH FAULT reading stack " + std::to_string(i));
                return false;
            }
            ++faults;
        }
        detail::iconDiag("walk", path + ": no stack carried it (count=" + std::to_string(count) +
                                     " faults=" + std::to_string(faults) + ")");
        return false;
    }

    void loadPackResourceEveryStack(void* manager, std::string const& path,
                                    std::function<void(std::string&&)> const& feed) {
        if (!manager || path.empty() || !Signatures::ResourcePack_getResource.result) return;

        auto count = detail::validateLayout(manager);
        if (!count) return;

        using Rpm = Signatures::FieldOffset::ResourcePackManager;
        auto* holder = *reinterpret_cast<void**>(static_cast<char*>(manager) + Rpm::stacksHolder);
        auto* begin = *reinterpret_cast<char**>(static_cast<char*>(holder) + Rpm::stacksBegin);

        bool faulted = false;
        for (size_t i = 0; i < count; ++i) {
            void* stack = begin + i * Rpm::stackStride;
            if (!detail::readable(stack, Rpm::stackPackIndex + 4)) continue;
            std::string buffer;
            if (detail::sehReadStack(stack, &path, &buffer, &faulted) && !buffer.empty()) {
                feed(std::move(buffer));
            }
            if (faulted) return;
        }
    }
}
