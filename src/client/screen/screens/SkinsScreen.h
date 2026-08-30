#pragma once
#include "../Screen.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

class SkinsScreen : public Screen {
public:
    SkinsScreen();

    std::string getName() override { return "Skins"; }

    void onRender(Event& ev);
    void onClick(Event& ev);
    void onKey(Event& ev);
    void onCleanup(Event& ev);

protected:
    void onEnable(bool ignoreAnimations) override;
    void onDisable() override;

private:
    struct SkinRow {
        std::string fileName;
        std::wstring displayName;
        std::wstring detail;
        std::filesystem::path fullPath;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t fileSize = 0;
        bool slim = false;
        bool geometryFile = false;
        d2d::Rect cardRect = {};
    };

    void rebuildRows();
    ID2D1Bitmap* getPreview(SkinRow const& row);
    void runSync();
    void openFolder();
    void updateScrollbarDrag(Vec2 const& mouse);
    void setStatus(std::wstring text, bool success);

    std::vector<SkinRow> rows;
    std::unordered_map<std::string, ComPtr<ID2D1Bitmap>> previews;
    ID2D1DeviceContext* previewOwner = nullptr;

    d2d::Rect panelRect = {};
    d2d::Rect listRect = {};
    d2d::Rect closeButtonRect = {};
    d2d::Rect syncButtonRect = {};
    d2d::Rect folderButtonRect = {};
    d2d::Rect refreshButtonRect = {};
    d2d::Rect scrollbarTrackRect = {};
    d2d::Rect scrollbarThumbRect = {};

    std::wstring status;
    std::chrono::steady_clock::time_point statusUntil = {};
    bool statusSuccess = false;

    float scroll = 0.f;
    float scrollMax = 0.f;
    float lerpScroll = 0.f;
    float scrollbarDragOffset = 0.f;
    bool draggingScrollbar = false;
    bool rowsDirty = true;
};
