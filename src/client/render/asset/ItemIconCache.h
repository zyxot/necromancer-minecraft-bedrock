#pragma once
#include <atomic>
#include <condition_variable>
#include <d2d1_1.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wrl/client.h>

namespace SDK {
    class ItemStack;
}
class DrawUtil;
class D2DUtil;
struct Vec2;

// Decodes item textures into D2D bitmaps so the Direct2D overlay can draw real
// item icons. Resolution per item id: the merged item_texture tables (flat
// sprite) or the terrain tables (block -> isometric cube from its top+side
// textures), falling back to direct textures/items|blocks/<name> paths for
// custom items. All pack IO lives on the worker thread; bitmaps are created on
// the render thread only.
class ItemIconCache {
public:
    static ItemIconCache& get();

    // Returns nullptr when the id has no resolvable texture YET, which is the
    // signal to fall back to a letter badge.
    ID2D1Bitmap* getBitmap(std::string const& itemId);

    // Draws an item on whichever renderer is active: the game's item renderer
    // when available, otherwise the resolved texture through D2D (blocks render
    // as an isometric cube). Returns false when nothing could be produced.
    static bool drawItem(DrawUtil& dc, SDK::ItemStack* stack, Vec2 const& pos, float size);

    // Same as drawItem for a bare item id (no ItemStack), D2D renderer only.
    static bool drawItemIcon(D2DUtil& dc, std::string const& itemId, Vec2 const& pos, float size);

    // Game thread, every tick: just records liveness and wakes the worker. It
    // performs NO game calls whatsoever — every game virtual we tried froze or
    // misfired (pack IO deadlocked the load pipeline; ClientInstance slot 0x60
    // blocks during world load). All discovery and IO lives on the worker.
    void tick();

    // Stops the worker thread and drops all state. Runs on the game thread from
    // the eject teardown, before the DLL goes away.
    void shutdown();

    // D2D bitmaps belong to the device context that created them, so everything
    // has to be dropped whenever the render target is rebuilt. Also re-reads the
    // pack texture tables so a server's resource packs are picked up.
    void invalidate();

    // True once the D2D overlay renderer is alive, meaning the game-layer icon
    // consumers can hand the icon work to the hud_screen layer (ESP-style) and
    // keep the D2D pass chrome-only. Before that the D2D pass must draw icons
    // itself or nothing would show.
    static bool gameLayerIconsEnabled();

private:
    struct ResolvedTextures {
        bool isBlock = false;
        std::string a;
        std::string b;
    };

    ItemIconCache();

    void workerLoop();
    void doWork();
    bool lookupResolved(std::string const& itemId, ResolvedTextures& out);
    bool lookupResolvedNoQueue(std::string const& itemId, ResolvedTextures& out);
    static bool drawResolvedOrEmbedded(D2DUtil& dc, std::string const& itemId, ResolvedTextures const& res,
                                       Vec2 const& pos, float size);
    static bool drawResolved(D2DUtil& dc, ResolvedTextures const& res, std::string const& itemId, Vec2 const& pos,
                             float size);
    ID2D1Bitmap* bitmapForPath(std::string const& path);
    ID2D1Bitmap* embeddedBitmap(std::string const& shortName);
    bool prepareRenderCache();
    void tryGameIconResolve(std::string const& itemId, SDK::ItemStack* stack);

    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID2D1Bitmap>> pathBitmaps;
    ID2D1DeviceContext* ownerCtx = nullptr;

    std::mutex pendingMutex;
    std::vector<std::string> pendingIds;
    std::unordered_map<std::string, ResolvedTextures> resolvedTex;
    std::unordered_map<std::string, std::string> rawBytes;
    std::unordered_set<std::string> pathMissing;
    std::unordered_set<std::string> packMissing;
    std::vector<std::pair<std::string, std::string>> pendingPathResolves;
    std::unordered_set<std::string> gameIconAttempted;
    static constexpr size_t maxPending = 256;

    std::thread worker;
    std::mutex workerMutex;
    std::condition_variable workerCv;
    bool workerStop = false;
    bool workPending = false;
    std::atomic<long long> lastTickMs { 0 };
};
