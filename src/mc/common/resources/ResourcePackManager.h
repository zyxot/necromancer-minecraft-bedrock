#pragma once

#include <functional>
#include <string>

#include "util/memory.h"
#include <mc/Addresses.h>

namespace SDK {
    class ClientInstance;

    // Walks the manager's pack stacks and reads `path` through the game's own
    // ResourcePack::getResource. Forward order (vanilla first): each miss costs
    // a synchronous game call, so first-hit-wins from the bottom keeps the call
    // volume tiny — walking all 59+ stacks per texture in reverse wedged the
    // resource system during gameplay. Server overrides are not chased for
    // icons; only vanilla-missing textures fall through to later stacks.
    bool loadPackResourceFrom(void* manager, std::string const& path, std::string& out);

    // Same walk, but feeds the file from EVERY stack that carries it, vanilla
    // first and each higher pack after — the game's own merge order for the
    // texture tables (higher packs override keys).
    void loadPackResourceEveryStack(void* manager, std::string const& path,
                                    std::function<void(std::string&&)> const& feed);

    namespace detail {
        void iconDiag(char const* tag, std::string const& detail);

        inline bool readable(void const* addr, size_t size) {
            if (!addr) return false;
            MEMORY_BASIC_INFORMATION mbi {};
            if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return false;
            if (mbi.State != MEM_COMMIT) return false;
            if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
            auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            return reinterpret_cast<uintptr_t>(addr) + size <= regionEnd;
        }

        // Largest span at `base` that is safe to read, capped to `want`.
        inline size_t readableSpan(void const* base, size_t want) {
            if (!base) return 0;
            MEMORY_BASIC_INFORMATION mbi {};
            if (VirtualQuery(base, &mbi, sizeof(mbi)) == 0) return 0;
            if (mbi.State != MEM_COMMIT) return 0;
            if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
            auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            auto avail = regionEnd - reinterpret_cast<uintptr_t>(base);
            return static_cast<size_t>(avail < want ? avail : want);
        }

        // Layout probe: validates the stacks holder (manager+0xB8 → begin/end,
        // 296-byte stacks, sane count). Returns the stack count, or 0 when the
        // layout does not match.
        inline size_t validateLayout(void* manager) {
            using Rpm = Signatures::FieldOffset::ResourcePackManager;
            if (!manager || !readable(manager, Rpm::stacksHolder + 8)) return 0;
            auto* holder = *reinterpret_cast<void**>(static_cast<char*>(manager) + Rpm::stacksHolder);
            if (!holder || !readable(holder, 0x20)) return 0;
            auto* begin = *reinterpret_cast<char**>(static_cast<char*>(holder) + Rpm::stacksBegin);
            auto* end = *reinterpret_cast<char**>(static_cast<char*>(holder) + Rpm::stacksEnd);
            if (!begin || end < begin) return 0;
            auto span = static_cast<size_t>(end - begin);
            if (span % Rpm::stackStride != 0) return 0;
            auto count = span / Rpm::stackStride;
            if (count < 1 || count > Rpm::maxStacks) return 0;
            return count;
        }

        inline bool sehReadStack(void* stack, std::string const* path, std::string* out, bool* faulted) {
            void* pack = *reinterpret_cast<void**>(static_cast<char*>(stack) +
                                                   Signatures::FieldOffset::ResourcePackManager::stackPack);
            if (!pack || !readable(pack, 0x200)) return false;
            auto index = *reinterpret_cast<uint32_t*>(static_cast<char*>(stack) +
                                                      Signatures::FieldOffset::ResourcePackManager::stackPackIndex);
            using GetResourceFn = bool (__fastcall*)(void*, std::string const*, std::string*, int);
            auto* fn = reinterpret_cast<GetResourceFn>(Signatures::ResourcePack_getResource.result);
            if (!fn) return false;
            __try {
                return fn(pack, path, out, static_cast<int>(index));
            } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                        : EXCEPTION_CONTINUE_SEARCH) {
                *faulted = true;
                return false;
            }
        }
    }
}
