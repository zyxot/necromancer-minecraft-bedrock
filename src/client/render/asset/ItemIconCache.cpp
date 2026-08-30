#include "pch.h"
#include "ItemIconCache.h"
#include "ItemIconRegistry.h"
#include "client/Necromancer.h"
#include "client/render/Renderer.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/resources/ResourcePackManager.h"
#include "mc/common/world/Item.h"
#include "mc/common/world/ItemStack.h"
#include "util/DrawContext.h"

#include <Shlwapi.h>
#include <chrono>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
using Microsoft::WRL::ComPtr;

namespace {
    long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    long long nowSteady() { return nowMs(); }

    struct LastDrawn {
        std::string lastId;
        std::string pendingId;
        long long pendingSince = 0;
    };

    LastDrawn& lastDrawnId() {
        static LastDrawn s;
        return s;
    }

    std::string aliasTextureName(std::string const& name) {
        static const std::unordered_map<std::string, std::string> exact = {
            { "golden_apple", "apple_golden" },     { "appleenchanted", "apple_golden" },
            { "golden_carrot", "carrot_golden" },   { "cooked_beef", "beef_cooked" },
            { "beef", "beef_raw" },                 { "cooked_porkchop", "porkchop_cooked" },
            { "porkchop", "porkchop_raw" },         { "cooked_chicken", "chicken_cooked" },
            { "chicken", "chicken_raw" },           { "cooked_mutton", "mutton_cooked" },
            { "mutton", "mutton_raw" },             { "cooked_rabbit", "rabbit_cooked" },
            { "rabbit", "rabbit_raw" },             { "cooked_salmon", "fish_salmon_cooked" },
            { "salmon", "fish_salmon_raw" },        { "cooked_cod", "fish_cooked" },
            { "cod", "fish_raw" },                  { "clownfish", "fish_clownfish_raw" },
            { "pufferfish", "fish_pufferfish_raw" }, { "wooden_door", "door_wood" },
            { "leather_helmet", "helmet" },         { "leather_chestplate", "chestplate" },
            { "leather_leggings", "leggings" },     { "leather_boots", "boots" },
            { "grass_block", "grass_carried" },
        };

        if (auto it = exact.find(name); it != exact.end()) return it->second;
        if (name.starts_with("golden_")) return "gold_" + name.substr(7);
        if (name.starts_with("wooden_")) return "wood_" + name.substr(7);
        return name;
    }

    std::string shortNameOf(std::string const& itemId) {
        auto sep = itemId.find(':');
        return sep == std::string::npos ? itemId : itemId.substr(sep + 1);
    }

    const ItemIconRegistry::Entry* findEntry(std::string const& id) {
        auto entries = ItemIconRegistry::entries();
        size_t lo = 0;
        size_t hi = entries.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            int cmp = id.compare(entries[mid].id);
            if (cmp == 0) return &entries[mid];
            if (cmp < 0) hi = mid;
            else lo = mid + 1;
        }
        return nullptr;
    }

    std::mutex packMutex;
    std::unordered_map<std::string, std::vector<std::string>> itemTable;
    std::unordered_map<std::string, std::vector<std::string>> terrainTable;
    bool packParsed = false;
    bool packBroken = false;

    using TextureTable = std::unordered_map<std::string, std::vector<std::string>>;

    void mergeTextureJson(TextureTable& table, std::string const& buffer) {
        auto js = nlohmann::json::parse(buffer, nullptr, false);
        if (js.is_discarded() || !js.is_object()) return;

        auto data = js.find("texture_data");
        if (data == js.end() || !data->is_object()) return;

        for (auto it = data->begin(); it != data->end(); ++it) {
            auto textures = it.value().find("textures");
            if (textures == it.value().end()) continue;
            std::vector<std::string> paths;
            if (textures->is_string()) {
                paths.push_back(textures->get<std::string>());
            } else if (textures->is_array()) {
                for (auto const& e : *textures) {
                    if (e.is_string()) paths.push_back(e.get<std::string>());
                }
            }
            if (paths.empty()) continue;
            table.insert_or_assign(it.key(), std::move(paths));
        }
    }

    bool parsePackTextures(void* manager) {
        TextureTable items;
        TextureTable terrain;
        bool gotAny = false;
        SDK::loadPackResourceEveryStack(manager, "textures/item_texture.json",
                                        [&](std::string&& bytes) {
                                            mergeTextureJson(items, bytes);
                                            gotAny = true;
                                        });
        SDK::loadPackResourceEveryStack(manager, "textures/terrain_texture.json",
                                        [&](std::string&& bytes) {
                                            mergeTextureJson(terrain, bytes);
                                            gotAny = true;
                                        });
        if (!gotAny || (items.empty() && terrain.empty())) return false;

        itemTable = std::move(items);
        terrainTable = std::move(terrain);
        SDK::detail::iconDiag("parse", "tables ready, items=" + std::to_string(itemTable.size()) +
                                           " terrain=" + std::to_string(terrainTable.size()));
        for (char const* prefix : { "leather", "slime", "helmet", "chestplate", "leggings", "boots", "chest",
                                    "armor" }) {
            for (auto const& [key, paths] : itemTable) {
                if (key.starts_with(prefix)) {
                    SDK::detail::iconDiag("keys", std::string("item ") + key + " -> " + paths.front());
                }
            }
            for (auto const& [key, paths] : terrainTable) {
                if (key.starts_with(prefix)) {
                    SDK::detail::iconDiag("keys", std::string("terrain ") + key + " -> " + paths.front());
                }
            }
        }
        return true;
    }

    bool packReady(void* manager) {
        std::lock_guard lock { packMutex };
        if (packBroken) return false;
        if (!packParsed) packParsed = parsePackTextures(manager);
        return packParsed;
    }

    bool packTableLoaded() {
        std::lock_guard lock { packMutex };
        return packParsed && !packBroken;
    }

    void markPackBroken() {
        std::lock_guard lock { packMutex };
        packBroken = true;
        itemTable.clear();
        terrainTable.clear();
    }

    struct ManagerCache {
        void* owner = nullptr;
        size_t offset = 0;
        bool valid = false;
    };
    ManagerCache g_managerCache;

    bool probeRegionForManager(void* base, size_t want, void** outManager) {
        if (!base) return false;
        auto span = SDK::detail::readableSpan(base, want);
        if (span < 8) return false;
        auto* p = reinterpret_cast<char*>(base);
        for (size_t off = 0; off + 8 <= span; off += 8) {
            void* cand = *reinterpret_cast<void**>(p + off);
            if (!cand) continue;
            if (!SDK::detail::validateLayout(cand)) continue;
            std::string buffer;
            if (SDK::loadPackResourceFrom(cand, "textures/item_texture.json", buffer) && !buffer.empty()) {
                *outManager = cand;
                SDK::detail::iconDiag("probe", "locked manager at base+0x" + std::to_string(off) +
                                                   " stacks=" +
                                                   std::to_string(SDK::detail::validateLayout(cand)));
                return true;
            }
        }
        return false;
    }

    void* resolveManager() {
        if (g_managerCache.valid) {
            void* cand = *reinterpret_cast<void**>(reinterpret_cast<char*>(g_managerCache.owner) +
                                                   g_managerCache.offset);
            if (SDK::detail::validateLayout(cand)) return cand;
            g_managerCache.valid = false;
        }

        auto* ci = SDK::ClientInstance::get();
        if (!ci) {
            SDK::detail::iconDiag("probe", "no client instance");
            return nullptr;
        }
        void* mcGame = *reinterpret_cast<void**>(reinterpret_cast<char*>(ci) + 0x1A0);
        if (!mcGame) {
            SDK::detail::iconDiag("probe", "no minecraftGame");
            return nullptr;
        }

        void* found = nullptr;
        if (probeRegionForManager(mcGame, 0x4000, &found)) {
            g_managerCache.owner = mcGame;
            g_managerCache.offset = static_cast<size_t>(reinterpret_cast<char*>(found) -
                                                        reinterpret_cast<char*>(mcGame));
            g_managerCache.valid = true;
            return found;
        }
        if (probeRegionForManager(ci, 0x2000, &found)) {
            g_managerCache.owner = ci;
            g_managerCache.offset = static_cast<size_t>(reinterpret_cast<char*>(found) -
                                                        reinterpret_cast<char*>(ci));
            g_managerCache.valid = true;
            return found;
        }
        SDK::detail::iconDiag("probe", "no layout-valid manager found in mcGame/ci scan");
        return nullptr;
    }

    struct IconNameBuffer {
        alignas(16) unsigned char buf[128];
    };

    bool callIconNameBuilder(void* item, void* stack, unsigned aux, IconNameBuffer* out, bool* faulted) {
        __try {
            memory::callVirtual<void>(item, Signatures::VtableIndex::Item::buildIconName, out->buf, stack, aux,
                                      static_cast<char>(0));
        } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                    : EXCEPTION_CONTINUE_SEARCH) {
            *faulted = true;
            return false;
        }
        return true;
    }

    bool gameIconName(SDK::ItemStack* stack, std::string& out) {
        auto* item = stack->getItem();
        if (!item) return false;

        IconNameBuffer buffer {};
        bool faulted = false;
        if (!callIconNameBuilder(item, stack, static_cast<unsigned>(stack->aux) & 0xFFFFu, &buffer, &faulted)) {
            return false;
        }

        auto* s = buffer.buf;
        size_t len = *reinterpret_cast<size_t*>(s + 16);
        size_t cap = *reinterpret_cast<size_t*>(s + 24);
        char const* data;
        if (cap < 16) {
            data = reinterpret_cast<char const*>(s);
        } else {
            data = *reinterpret_cast<char const* const*>(s);
            if (!data || !SDK::detail::readable(data, len + 1)) return false;
        }
        if (len == 0 || len > 512) return false;
        out.assign(data, len);
        return true;
    }
}

ItemIconCache& ItemIconCache::get() {
    static auto* instance = new ItemIconCache;
    return *instance;
}

bool ItemIconCache::gameLayerIconsEnabled() {
    return Necromancer::getRenderer().getDeviceContext() != nullptr;
}

ItemIconCache::ItemIconCache() {
    worker = std::thread(&ItemIconCache::workerLoop, this);
}

ComPtr<ID2D1Bitmap> decodePngToBitmap(std::string const& bytes) {
    auto* dc = Necromancer::getRenderer().getDeviceContext();
    auto* factory = Necromancer::getRenderer().getImagingFactory();
    if (!dc || !factory || bytes.empty()) return nullptr;

    ComPtr<IStream> stream(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()),
                                             static_cast<UINT>(bytes.size())));
    if (!stream) return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                decoder.GetAddressOf())) ||
        !decoder)
        return nullptr;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || !frame) return nullptr;

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(conv.GetAddressOf()))) return nullptr;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return nullptr;

    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, bitmap.GetAddressOf()))) return nullptr;
    return bitmap;
}

bool decodeTgaPixels(unsigned char const* data, size_t size, int w, int h, int bpp, bool rle, bool topOrigin,
                     std::vector<unsigned char>& out) {
    if (bpp != 24 && bpp != 32) return false;
    size_t bytesPerPixel = static_cast<size_t>(bpp) / 8;
    size_t pixelCount = static_cast<size_t>(w) * h;
    out.assign(pixelCount * 4, 0);

    std::vector<unsigned char> raw;
    if (!rle) {
        if (size < pixelCount * bytesPerPixel) return false;
        raw.assign(data, data + pixelCount * bytesPerPixel);
    } else {
        raw.reserve(pixelCount * bytesPerPixel);
        size_t pos = 0;
        while (raw.size() < pixelCount * bytesPerPixel) {
            if (pos >= size) return false;
            unsigned char header = data[pos++];
            size_t count = static_cast<size_t>(header & 0x7F) + 1;
            if (header & 0x80) {
                if (pos + bytesPerPixel > size) return false;
                for (size_t i = 0; i < count; ++i) raw.insert(raw.end(), data + pos, data + pos + bytesPerPixel);
                pos += bytesPerPixel;
            } else {
                size_t chunk = count * bytesPerPixel;
                if (pos + chunk > size) return false;
                raw.insert(raw.end(), data + pos, data + pos + chunk);
                pos += chunk;
            }
        }
    }

    for (int y = 0; y < h; ++y) {
        int srcY = topOrigin ? y : h - 1 - y;
        for (int x = 0; x < w; ++x) {
            unsigned char const* px = raw.data() + (static_cast<size_t>(srcY) * w + x) * bytesPerPixel;
            unsigned char* dst = out.data() + (static_cast<size_t>(y) * w + x) * 4;
            dst[0] = px[2];
            dst[1] = px[1];
            dst[2] = px[0];
            dst[3] = bytesPerPixel == 4 ? px[3] : 255;
            float alpha = dst[3] / 255.f;
            dst[0] = static_cast<unsigned char>(dst[0] * alpha);
            dst[1] = static_cast<unsigned char>(dst[1] * alpha);
            dst[2] = static_cast<unsigned char>(dst[2] * alpha);
        }
    }
    return true;
}

ComPtr<ID2D1Bitmap> decodeTgaToBitmap(std::string const& bytes) {
    auto* dc = Necromancer::getRenderer().getDeviceContext();
    if (!dc || bytes.size() < 18) return nullptr;

    auto const* d = reinterpret_cast<unsigned char const*>(bytes.data());
    int idLen = d[0];
    int imageType = d[2];
    int w = d[12] | (d[13] << 8);
    int h = d[14] | (d[15] << 8);
    int bpp = d[16];
    bool topOrigin = (d[17] & 0x20) != 0;
    if (w <= 0 || w > 1024 || h <= 0 || h > 1024) return nullptr;
    if (d[1] != 0 || (imageType != 2 && imageType != 10)) return nullptr;

    size_t offset = 18 + static_cast<size_t>(idLen);
    if (offset >= bytes.size()) return nullptr;

    std::vector<unsigned char> pixels;
    if (!decodeTgaPixels(d + offset, bytes.size() - offset, w, h, bpp, imageType == 10, topOrigin, pixels)) {
        return nullptr;
    }

    ComPtr<ID2D1Bitmap> bitmap;
    D2D1_BITMAP_PROPERTIES props =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), pixels.data(),
                                static_cast<UINT32>(w) * 4, props, bitmap.GetAddressOf()))) {
        return nullptr;
    }
    return bitmap;
}

ComPtr<ID2D1Bitmap> decodeImageForPath(std::string const& path, std::string const& bytes) {
    if (path.ends_with(".tga")) return decodeTgaToBitmap(bytes);
    return decodePngToBitmap(bytes);
}

bool ItemIconCache::lookupResolved(std::string const& itemId, ResolvedTextures& out) {
    std::lock_guard lock { pendingMutex };
    auto it = resolvedTex.find(itemId);
    if (it != resolvedTex.end()) {
        out = it->second;
        return true;
    }
    if (packMissing.count(itemId)) return false;
    if (pendingIds.size() < maxPending &&
        std::find(pendingIds.begin(), pendingIds.end(), itemId) == pendingIds.end()) {
        pendingIds.push_back(itemId);
    }
    return false;
}

bool ItemIconCache::lookupResolvedNoQueue(std::string const& itemId, ResolvedTextures& out) {
    std::lock_guard lock { pendingMutex };
    auto it = resolvedTex.find(itemId);
    if (it == resolvedTex.end()) return false;
    out = it->second;
    return true;
}

bool ItemIconCache::prepareRenderCache() {
    auto* ctx = Necromancer::getRenderer().getDeviceContext();
    if (ownerCtx != ctx) {
        pathBitmaps.clear();
        ownerCtx = ctx;
    }
    return ownerCtx != nullptr;
}

ID2D1Bitmap* ItemIconCache::embeddedBitmap(std::string const& shortName) {
    if (!prepareRenderCache()) return nullptr;
    std::string key = "embed:" + shortName;
    auto it = pathBitmaps.find(key);
    if (it != pathBitmaps.end()) return it->second.Get();

    ComPtr<ID2D1Bitmap> bmp;
    std::string const aliased = aliasTextureName(shortName);
    for (std::string const* n : { &shortName, &aliased }) {
        if (auto const* entry = findEntry(*n)) {
            size_t size = static_cast<size_t>(entry->end - entry->begin);
            if (size > 0 && size <= static_cast<size_t>((std::numeric_limits<UINT>::max)())) {
                bmp = decodePngToBitmap(std::string(entry->begin, entry->end));
                if (bmp) break;
            }
        }
    }
    pathBitmaps.emplace(key, std::move(bmp));
    return pathBitmaps[key].Get();
}

ID2D1Bitmap* ItemIconCache::bitmapForPath(std::string const& path) {
    if (path.empty()) return nullptr;
    if (!prepareRenderCache()) return nullptr;
    auto it = pathBitmaps.find(path);
    if (it != pathBitmaps.end()) return it->second.Get();

    std::string bytes;
    {
        std::lock_guard lock { pendingMutex };
        if (pathMissing.count(path)) {
            pathBitmaps.emplace(path, nullptr);
            return nullptr;
        }
        auto raw = rawBytes.find(path);
        if (raw == rawBytes.end()) return nullptr;
        bytes = std::move(raw->second);
        rawBytes.erase(raw);
    }

    auto bmp = decodeImageForPath(path, bytes);
    if (!bmp) {
        SDK::detail::iconDiag(("decode:" + path).c_str(), "FAILED " + std::to_string(bytes.size()) + " bytes");
        std::lock_guard lock { pendingMutex };
        pathMissing.insert(path);
    }
    pathBitmaps.emplace(path, std::move(bmp));
    return pathBitmaps[path].Get();
}

ID2D1Bitmap* ItemIconCache::getBitmap(std::string const& itemId) {
    if (auto* emb = embeddedBitmap(shortNameOf(itemId))) return emb;
    ResolvedTextures res;
    if (!lookupResolved(itemId, res)) return nullptr;
    return bitmapForPath(res.a);
}

void ItemIconCache::tick() {
    lastTickMs.store(nowMs(), std::memory_order_release);
    {
        std::lock_guard lock { workerMutex };
        workPending = true;
    }
    workerCv.notify_all();
}

void ItemIconCache::tryGameIconResolve(std::string const& itemId, SDK::ItemStack* stack) {
    {
        std::lock_guard lock { pendingMutex };
        if (!gameIconAttempted.insert(itemId).second) return;
    }

    std::string name;
    if (!gameIconName(stack, name) || name.empty()) {
        SDK::detail::iconDiag(("gameicon:" + itemId).c_str(), "game lookup produced no usable name");
        return;
    }

    std::string path;
    {
        std::lock_guard packLock { packMutex };
        auto it = itemTable.find(name);
        if (it != itemTable.end() && !it->second.empty()) {
            path = it->second.front();
        } else {
            auto tit = terrainTable.find(name);
            if (tit != terrainTable.end() && !tit->second.empty()) path = tit->second.front();
        }
    }
    if (path.empty()) {
        SDK::detail::iconDiag(("gameicon:" + itemId).c_str(),
                              "icon name \"" + name + "\" not in tables, trying as path");
        path = name;
    } else {
        SDK::detail::iconDiag(("gameicon:" + itemId).c_str(), "icon name \"" + name + "\" -> " + path);
    }

    {
        std::lock_guard lock { pendingMutex };
        pendingPathResolves.emplace_back(itemId, path);
    }
    {
        std::lock_guard lock { workerMutex };
        workPending = true;
    }
    workerCv.notify_all();
}

void ItemIconCache::workerLoop() {
    std::unique_lock lock { workerMutex };
    while (!workerStop) {
        workerCv.wait_for(lock, std::chrono::milliseconds(500),
                          [this] { return workerStop || workPending; });
        if (workerStop) break;
        workPending = false;
        lock.unlock();
        doWork();
        lock.lock();
    }
}

void ItemIconCache::doWork() {
    if (nowMs() - lastTickMs.load(std::memory_order_acquire) > 2000) return;

    void* manager = resolveManager();
    if (!manager) return;

    static std::chrono::steady_clock::time_point lastParseAttempt {};
    auto now = std::chrono::steady_clock::now();
    if (!packTableLoaded()) {
        if (now - lastParseAttempt < std::chrono::milliseconds(600)) return;
        lastParseAttempt = now;
        if (!packReady(manager)) return;
    }

    std::vector<std::string> batch;
    {
        std::lock_guard lock { pendingMutex };
        if (pendingIds.size() > 4) {
            batch.assign(pendingIds.begin(), pendingIds.begin() + 4);
            pendingIds.erase(pendingIds.begin(), pendingIds.begin() + 4);
        } else {
            batch.swap(pendingIds);
        }
    }
    if (batch.empty()) return;

    auto fetchPath = [&](std::string const& path) -> std::string {
        if (path.empty()) return {};
        {
            std::lock_guard lock { pendingMutex };
            if (rawBytes.count(path)) return path;
            if (pathMissing.count(path)) return {};
        }
        for (char const* ext : { "", ".png", ".tga" }) {
            std::string candidate = path + ext;
            std::string bytes;
            if (SDK::loadPackResourceFrom(manager, candidate, bytes) && !bytes.empty()) {
                std::lock_guard lock { pendingMutex };
                rawBytes[candidate] = std::move(bytes);
                return candidate;
            }
        }
        std::lock_guard lock { pendingMutex };
        pathMissing.insert(path);
        SDK::detail::iconDiag(("resolve:" + path).c_str(), "fetch FAILED");
        return {};
    };

    for (auto const& itemId : batch) {
        std::string shortName = shortNameOf(itemId);
        std::string aliased = aliasTextureName(shortName);

        ResolvedTextures res;
        bool resolved = false;

        for (std::string const* key : { &shortName, &aliased }) {
            std::lock_guard packLock { packMutex };
            auto it = itemTable.find(*key);
            if (it != itemTable.end() && !it->second.empty()) {
                res.isBlock = false;
                res.a = it->second.front();
                res.b.clear();
                resolved = true;
                break;
            }
            auto tit = terrainTable.find(*key);
            if (tit != terrainTable.end() && !tit->second.empty()) {
                res.isBlock = true;
                res.a = tit->second.front();
                res.b = tit->second.front();
                resolved = true;
                break;
            }
        }

        if (!resolved) {
            for (std::string const* key : { &shortName, &aliased }) {
                std::string itemPath = "textures/items/" + *key;
                std::string got = fetchPath(itemPath);
                if (!got.empty()) {
                    res.isBlock = false;
                    res.a = got;
                    res.b.clear();
                    resolved = true;
                    break;
                }
                std::string blockPath = "textures/blocks/" + *key;
                got = fetchPath(blockPath);
                if (!got.empty()) {
                    res.isBlock = true;
                    res.a = got;
                    res.b = got;
                    resolved = true;
                    break;
                }
            }
        }

        if (!resolved) {
            SDK::detail::iconDiag(("resolve:" + itemId).c_str(), "key not in any table");
            std::lock_guard lock { pendingMutex };
            packMissing.insert(itemId);
            continue;
        }

        std::string aPath = fetchPath(res.a);
        if (aPath.empty()) {
            std::lock_guard lock { pendingMutex };
            packMissing.insert(itemId);
            continue;
        }
        res.a = aPath;
        if (res.isBlock) {
            std::string bPath = fetchPath(res.b);
            if (bPath.empty()) {
                std::lock_guard lock { pendingMutex };
                packMissing.insert(itemId);
                continue;
            }
            res.b = bPath;
        } else {
            res.b.clear();
        }

        SDK::detail::iconDiag(("resolve:" + itemId).c_str(),
                              res.isBlock ? "block top=" + res.a + " side=" + res.b : "flat " + res.a);
        std::lock_guard lock { pendingMutex };
        resolvedTex[itemId] = std::move(res);
    }

    std::vector<std::pair<std::string, std::string>> pathBatch;
    {
        std::lock_guard lock { pendingMutex };
        size_t n = std::min<size_t>(pendingPathResolves.size(), 4);
        pathBatch.assign(std::make_move_iterator(pendingPathResolves.begin()),
                         std::make_move_iterator(pendingPathResolves.begin() + n));
        pendingPathResolves.erase(pendingPathResolves.begin(), pendingPathResolves.begin() + n);
    }
    for (auto& [itemId, path] : pathBatch) {
        std::string got = fetchPath(path);
        if (!got.empty()) {
            std::lock_guard lock { pendingMutex };
            resolvedTex[itemId] = ResolvedTextures { false, got, std::string() };
            SDK::detail::iconDiag(("resolve:" + itemId).c_str(), "flat " + got + " (game icon)");
        } else {
            std::lock_guard lock { pendingMutex };
            packMissing.insert(itemId);
        }
    }
}

void ItemIconCache::shutdown() {
    {
        std::lock_guard lock { workerMutex };
        workerStop = true;
        workPending = false;
    }
    workerCv.notify_all();
    if (worker.joinable()) worker.join();
    invalidate();
}

void ItemIconCache::invalidate() {
    g_managerCache.valid = false;
    pathBitmaps.clear();
    ownerCtx = nullptr;
    std::lock_guard packLock { packMutex };
    packParsed = false;
    itemTable.clear();
    terrainTable.clear();
    packBroken = false;
    std::lock_guard pendingLock { pendingMutex };
    pendingIds.clear();
    resolvedTex.clear();
    rawBytes.clear();
    pathMissing.clear();
    packMissing.clear();
}

bool ItemIconCache::drawItem(DrawUtil& dc, SDK::ItemStack* stack, Vec2 const& pos, float size) {
    if (!stack) return false;

    SDK::Item* raw = stack->getItem();
    if (!raw) return false;

    if (dc.isMinecraft()) {
        auto& mcDc = static_cast<MCDrawUtil&>(dc);
        if (!mcDc.scn || !mcDc.renderCtx) return false;
        SDK::detail::iconDiag(("draw:" + raw->id.getString()).c_str(), "game-render branch");
        mcDc.drawItem(stack, pos, size / 48.f, 1.f);
        return true;
    }

    auto& d2dDc = static_cast<D2DUtil&>(dc);
    if (!d2dDc.ctx) return false;

    SDK::detail::iconDiag(("draw:" + raw->id.getString()).c_str(), "d2d branch");
    std::string const& id = raw->id.getString();
    if (drawItemIcon(d2dDc, id, pos, size)) return true;
    get().tryGameIconResolve(id, stack);
    return false;
}

namespace {
    void drawIsoCube(D2DUtil& dc, ID2D1Bitmap* top, ID2D1Bitmap* side, Vec2 const& pos, float size) {
        auto* ctx = dc.ctx;
        D2D1_MATRIX_3X2_F old;
        ctx->GetTransform(&old);

        float k = size / 32.f;
        ComPtr<ID2D1SolidColorBrush> shade;
        ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), shade.GetAddressOf());
        if (!shade) return;

        auto face = [&](float m11, float m12, float m21, float m22, float dx, float dy, float darkness,
                        ID2D1Bitmap* bmp) {
            ctx->SetTransform(D2D1::Matrix3x2F(m11, m12, m21, m22, pos.x + dx, pos.y + dy) * old);
            ctx->DrawBitmap(bmp, D2D1::RectF(0.f, 0.f, 16.f, 16.f), 1.f,
                            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            if (darkness > 0.f) {
                shade->SetOpacity(darkness);
                ctx->FillRectangle(D2D1::RectF(0.f, 0.f, 16.f, 16.f), shade.Get());
            }
        };

        face(k, k * 0.5f, -k, k * 0.5f, size * 0.5f, 0.f, 0.f, top);
        face(k, k * 0.5f, 0.f, k, 0.f, size * 0.25f, 0.35f, side);
        face(k, -k * 0.5f, 0.f, k, size * 0.5f, size * 0.5f, 0.15f, side);

        ctx->SetTransform(old);
    }
}

bool ItemIconCache::drawItemIcon(D2DUtil& dc, std::string const& itemId, Vec2 const& pos, float size) {
    if (!dc.ctx) return false;

    if (auto* emb = get().embeddedBitmap(shortNameOf(itemId))) {
        D2D1_RECT_F dest = D2D1::RectF(pos.x, pos.y, pos.x + size, pos.y + size);
        dc.ctx->DrawBitmap(emb, dest, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        return true;
    }

    ResolvedTextures res;
    if (!get().lookupResolved(itemId, res)) {
        // First frames after a slot swap: the new id is queued but not fetched yet.
        // Keep showing the previously drawn icon instead of dropping to a blank
        // badge, so the HUD does not visibly twitch on every hotbar switch.
        auto& last = lastDrawnId();
        if (last.lastId != itemId) {
            last.pendingId = itemId;
            last.pendingSince = nowSteady();
        }
        if (last.lastId != itemId && !last.lastId.empty() &&
            nowSteady() - last.pendingSince < 2000) {
            ResolvedTextures lastRes;
            if (get().lookupResolvedNoQueue(last.lastId, lastRes) ||
                get().embeddedBitmap(shortNameOf(last.lastId))) {
                if (drawResolvedOrEmbedded(dc, last.lastId, lastRes, pos, size)) {
                    return true;
                }
            }
        }
        return false;
    }

    {
        auto& last = lastDrawnId();
        last.lastId = itemId;
        last.pendingId.clear();
    }

    return drawResolved(dc, res, itemId, pos, size);
}

bool ItemIconCache::drawResolvedOrEmbedded(D2DUtil& dc, std::string const& itemId, ResolvedTextures const& res,
                                           Vec2 const& pos, float size) {
    if (auto* emb = get().embeddedBitmap(shortNameOf(itemId))) {
        D2D1_RECT_F dest = D2D1::RectF(pos.x, pos.y, pos.x + size, pos.y + size);
        dc.ctx->DrawBitmap(emb, dest, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        return true;
    }
    if (res.a.empty()) return false;
    return drawResolved(dc, res, itemId, pos, size);
}

bool ItemIconCache::drawResolved(D2DUtil& dc, ResolvedTextures const& res, std::string const& itemId, Vec2 const& pos,
                                 float size) {
    if (res.isBlock) {
        auto* top = get().bitmapForPath(res.a);
        auto* side = (res.b.empty() || res.b == res.a) ? top : get().bitmapForPath(res.b);
        if (!top || !side) return false;
        drawIsoCube(dc, top, side, pos, size);
        return true;
    }

    auto* bitmap = get().bitmapForPath(res.a);
    if (!bitmap) return false;

    D2D1_RECT_F dest = D2D1::RectF(pos.x, pos.y, pos.x + size, pos.y + size);
    dc.ctx->DrawBitmap(bitmap, dest, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    return true;
}
