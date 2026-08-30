#include "pch.h"
#include "SkinsScreen.h"

#include "client/event/Eventing.h"
#include "client/event/events/ClickEvent.h"
#include "client/event/events/KeyUpdateEvent.h"
#include "client/event/events/RendererCleanupEvent.h"
#include "client/event/events/RenderOverlayEvent.h"
#include "client/Necromancer.h"
#include "client/localization/LocalizeString.h"
#include "client/misc/SkinPackManager.h"
#include "client/render/asset/Assets.h"
#include "util/DrawContext.h"

#include <algorithm>
#include <format>

namespace {
    constexpr std::chrono::milliseconds statusDuration = std::chrono::milliseconds(4200);
}

SkinsScreen::SkinsScreen() {
    Eventing::get().listen<RenderOverlayEvent>(this, (EventListenerFunc)&SkinsScreen::onRender, 1, true);
    Eventing::get().listen<ClickEvent>(this, (EventListenerFunc)&SkinsScreen::onClick, 4);
    Eventing::get().listen<KeyUpdateEvent>(this, (EventListenerFunc)&SkinsScreen::onKey, 1);
    Eventing::get().listen<RendererCleanupEvent>(this, (EventListenerFunc)&SkinsScreen::onCleanup, 1, true);
}

void SkinsScreen::onEnable(bool) {
    rowsDirty = true;
    scroll = 0.f;
    lerpScroll = 0.f;
    status.clear();
}

void SkinsScreen::onDisable() {
    mouseButtons = {};
    activeMouseButtons = {};
    justClicked = {};
    draggingScrollbar = false;
}

void SkinsScreen::onCleanup(Event&) {
    previews.clear();
    previewOwner = nullptr;
}

void SkinsScreen::setStatus(std::wstring text, bool success) {
    status = std::move(text);
    statusSuccess = success;
    statusUntil = std::chrono::steady_clock::now() + statusDuration;
}

void SkinsScreen::rebuildRows() {
    rows.clear();

    std::vector<SkinPack::SkinEntry> entries = SkinPack::scan();
    rows.reserve(entries.size());

    for (SkinPack::SkinEntry const& entry : entries) {
        SkinRow row {};
        row.fileName = entry.fileName;
        row.fullPath = entry.fullPath;
        row.width = entry.width;
        row.height = entry.height;
        row.fileSize = entry.fileSize;
        row.slim = entry.slim;
        row.geometryFile = !entry.geometrySource.empty();

        std::string label = entry.fileName.substr(0, entry.fileName.size() - 4);
        row.displayName = util::StrToWStr(label);

        std::string detail;
        if (entry.width > 0 && entry.height > 0) {
            detail = std::format("{}x{}", entry.width, entry.height);
        } else {
            detail = "unknown size";
        }
        detail += entry.slim ? " | slim arms" : " | classic arms";
        if (!entry.geometrySource.empty()) {
            detail += " | custom model: " + entry.geometry;
        }
        row.detail = util::StrToWStr(detail);

        rows.push_back(std::move(row));
    }

    std::unordered_map<std::string, ComPtr<ID2D1Bitmap>> kept;
    for (SkinRow const& row : rows) {
        if (auto found = previews.find(row.fileName); found != previews.end()) {
            kept.insert_or_assign(row.fileName, found->second);
        }
    }
    previews = std::move(kept);

    rowsDirty = false;
}

ID2D1Bitmap* SkinsScreen::getPreview(SkinRow const& row) {
    ID2D1DeviceContext* dc = Necromancer::getRenderer().getDeviceContext();
    IWICImagingFactory2* factory = Necromancer::getRenderer().getImagingFactory();
    if (!dc || !factory) return nullptr;

    if (previewOwner != dc) {
        previews.clear();
        previewOwner = dc;
    }

    if (auto found = previews.find(row.fileName); found != previews.end()) {
        return found->second.Get();
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(row.fullPath.wstring().c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) ||
        !decoder) {
        previews.insert_or_assign(row.fileName, nullptr);
        return nullptr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || !frame) {
        previews.insert_or_assign(row.fileName, nullptr);
        return nullptr;
    }

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(conv.GetAddressOf())) || !conv) {
        previews.insert_or_assign(row.fileName, nullptr);
        return nullptr;
    }

    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom))) {
        previews.insert_or_assign(row.fileName, nullptr);
        return nullptr;
    }

    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, bitmap.GetAddressOf()))) {
        previews.insert_or_assign(row.fileName, nullptr);
        return nullptr;
    }

    auto inserted = previews.insert_or_assign(row.fileName, bitmap);
    return inserted.first->second.Get();
}

void SkinsScreen::runSync() {
    std::string report;
    bool ok = SkinPack::sync(report);
    setStatus(util::StrToWStr(report), ok);
    rowsDirty = true;
}

void SkinsScreen::openFolder() {
    SkinPack::openFolder();
    setStatus(util::StrToWStr(SkinPack::getSkinsFolder().string()), true);
}

void SkinsScreen::updateScrollbarDrag(Vec2 const& mouse) {
    if (!draggingScrollbar) return;
    if (!mouseButtons[0] || scrollMax <= 0.f || scrollbarTrackRect.getHeight() <= scrollbarThumbRect.getHeight()) {
        draggingScrollbar = false;
        return;
    }

    float availableTrack = scrollbarTrackRect.getHeight() - scrollbarThumbRect.getHeight();
    float thumbTop = std::clamp(mouse.y - scrollbarDragOffset, scrollbarTrackRect.top,
                                scrollbarTrackRect.bottom - scrollbarThumbRect.getHeight());
    float normalized = (thumbTop - scrollbarTrackRect.top) / availableTrack;
    scroll = std::clamp(normalized * scrollMax, 0.f, scrollMax);
}

void SkinsScreen::onRender(Event&) {
    if (!isActive()) {
        justClicked = {};
        return;
    }

    if (rowsDirty) rebuildRows();

    D2DUtil dc;
    D2D1_SIZE_F screenSize = Necromancer::getRenderer().getScreenSize();
    Vec2 cursorPos = SDK::ClientInstance::get()->cursorPos;
    d2d::Color accent = d2d::Color(Necromancer::get().getAccentColor().getMainColor());

    updateScrollbarDrag(cursorPos);

    if (Necromancer::get().getMenuBlur()) {
        dc.drawGaussianBlur(Necromancer::get().getMenuBlur().value());
    }

    float scale = std::clamp(screenSize.width / 1920.f, 0.7f, 1.1f);
    float panelWidth = std::min(screenSize.width * 0.78f, 860.f * scale);
    float panelHeight = std::min(screenSize.height * 0.78f, 640.f * scale);
    panelRect = { (screenSize.width - panelWidth) * 0.5f, (screenSize.height - panelHeight) * 0.5f,
                  (screenSize.width + panelWidth) * 0.5f, (screenSize.height + panelHeight) * 0.5f };

    float pad = 25.f * scale;
    float radius = 19.f * scale;
    float rowHeight = 72.f * scale;
    float gap = 8.f * scale;
    float headerHeight = 76.f * scale;

    dc.fillRoundedRectangle(panelRect, d2d::Color::RGB(0x07, 0x07, 0x07).asAlpha(0.75f), radius);
    dc.drawRoundedRectangle(panelRect, d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(0.28f), radius, 4.f * scale,
                            DrawUtil::OutlinePosition::Outside);

    float closeSize = 22.f * scale;
    closeButtonRect = { panelRect.right - pad - closeSize, panelRect.top + 28.f * scale, panelRect.right - pad,
                        panelRect.top + 28.f * scale + closeSize };

    float buttonHeight = 32.f * scale;
    float buttonTop = panelRect.top + 23.f * scale;
    float syncWidth = 96.f * scale;
    float folderWidth = 118.f * scale;
    float refreshWidth = 92.f * scale;

    folderButtonRect = { closeButtonRect.left - 18.f * scale - folderWidth, buttonTop,
                         closeButtonRect.left - 18.f * scale, buttonTop + buttonHeight };
    syncButtonRect = { folderButtonRect.left - 10.f * scale - syncWidth, buttonTop, folderButtonRect.left - 10.f * scale,
                       buttonTop + buttonHeight };
    refreshButtonRect = { syncButtonRect.left - 10.f * scale - refreshWidth, buttonTop,
                          syncButtonRect.left - 10.f * scale, buttonTop + buttonHeight };

    float titleTop = panelRect.top + 19.f * scale;
    dc.drawText({ panelRect.left + pad, titleTop, refreshButtonRect.left - 16.f * scale, titleTop + 31.f * scale },
                LocalizeString::get("client.module.skins.name"), d2d::Colors::WHITE,
                Renderer::FontSelection::PrimaryLight, 25.f * scale, DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);

    std::wstring subtitle;
    bool showingStatus = !status.empty() && std::chrono::steady_clock::now() < statusUntil;
    if (showingStatus) {
        subtitle = status;
    } else {
        subtitle = util::FormatWString(LocalizeString::get(rows.size() == 1 ? "client.screen.skins.count.one"
                                                                           : "client.screen.skins.count.many"),
                                       { std::to_wstring(rows.size()) });
    }

    dc.drawSingleLineFitted(
        { panelRect.left + pad + 1.f * scale, titleTop + 31.f * scale, panelRect.right - pad, titleTop + 56.f * scale },
        subtitle,
        showingStatus ? (statusSuccess ? d2d::Color::RGB(0x8B, 0xE0, 0x9A) : d2d::Color::RGB(0xF4, 0x8B, 0x84))
                      : d2d::Color::RGB(0xD2, 0xD2, 0xD2).asAlpha(0.72f),
        Renderer::FontSelection::PrimaryRegular, 15.f * scale);

    struct ActionButton {
        d2d::Rect rect;
        std::wstring label;
    };

    ActionButton buttons[3] = {
        { refreshButtonRect, LocalizeString::get("client.screen.skins.refresh.name").value() },
        { syncButtonRect, LocalizeString::get("client.screen.skins.sync.name").value() },
        { folderButtonRect, LocalizeString::get("client.screen.skins.openFolder.name").value() }
    };

    for (ActionButton const& button : buttons) {
        bool hovered = button.rect.contains(cursorPos);
        dc.fillRoundedRectangle(button.rect, hovered ? accent : d2d::Color::RGB(0x38, 0x38, 0x38).asAlpha(0.88f),
                                button.rect.getHeight() * 0.23f);
        dc.drawText(button.rect, button.label, d2d::Colors::WHITE, Renderer::FontSelection::PrimaryRegular,
                    15.f * scale, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (ID2D1Bitmap* closeIcon = Necromancer::getAssets().xIcon.getBitmap()) {
        dc.ctx->DrawBitmap(closeIcon, closeButtonRect, closeButtonRect.contains(cursorPos) ? 1.f : 0.72f);
    }

    listRect = { panelRect.left + pad, panelRect.top + headerHeight + 7.f * scale, panelRect.right - pad,
                 panelRect.bottom - pad };

    float contentHeight = rows.empty() ? 0.f : static_cast<float>(rows.size()) * (rowHeight + gap) - gap;
    scrollMax = std::max(0.f, contentHeight - listRect.getHeight());
    scroll = std::clamp(scroll, 0.f, scrollMax);
    lerpScroll = std::lerp(lerpScroll, scroll, Necromancer::getRenderer().getDeltaTime() * 0.25f);
    if (std::abs(lerpScroll - scroll) < 0.1f) lerpScroll = scroll;

    if (rows.empty()) {
        dc.drawText(listRect, LocalizeString::get("client.screen.skins.empty.name"),
                    d2d::Color::RGB(0xA8, 0xB4, 0xC8), Renderer::FontSelection::PrimaryRegular, 17.f * scale,
                    DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        scrollbarTrackRect = {};
        scrollbarThumbRect = {};
        return;
    }

    dc.ctx->PushAxisAlignedClip(dc.getRect(listRect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    for (size_t i = 0; i < rows.size(); i++) {
        SkinRow& row = rows[i];
        float y = listRect.top + (static_cast<float>(i) * (rowHeight + gap)) - lerpScroll;
        d2d::Rect rowRect { listRect.left, y, listRect.right, y + rowHeight };
        row.cardRect = rowRect;
        if (rowRect.bottom < listRect.top || rowRect.top > listRect.bottom) continue;

        bool hovered = rowRect.contains(cursorPos);
        dc.fillRoundedRectangle(rowRect,
                                hovered ? d2d::Color::RGB(0x2A, 0x2A, 0x2A).asAlpha(0.58f)
                                        : d2d::Color::RGB(0x12, 0x12, 0x12).asAlpha(0.48f),
                                10.f * scale);

        float previewSize = rowHeight - 20.f * scale;
        d2d::Rect previewRect { rowRect.left + 12.f * scale, rowRect.top + 10.f * scale,
                                rowRect.left + 12.f * scale + previewSize, rowRect.top + 10.f * scale + previewSize };
        dc.fillRoundedRectangle(previewRect, d2d::Color::RGB(0x00, 0x00, 0x00).asAlpha(0.38f), 7.f * scale);

        if (ID2D1Bitmap* preview = getPreview(row)) {
            D2D1_SIZE_F pixelSize = preview->GetSize();
            if (pixelSize.width > 0.f && pixelSize.height > 0.f) {
                float fit = std::min(previewRect.getWidth() / pixelSize.width,
                                     previewRect.getHeight() / pixelSize.height);
                float drawWidth = pixelSize.width * fit;
                float drawHeight = pixelSize.height * fit;
                d2d::Rect dest { previewRect.left + (previewRect.getWidth() - drawWidth) * 0.5f,
                                 previewRect.top + (previewRect.getHeight() - drawHeight) * 0.5f, 0.f, 0.f };
                dest.right = dest.left + drawWidth;
                dest.bottom = dest.top + drawHeight;
                dc.ctx->DrawBitmap(preview, dc.getRect(dest), 1.f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
        }

        float textLeft = previewRect.right + 14.f * scale;
        dc.drawSingleLineFitted({ textLeft, rowRect.top + 14.f * scale, rowRect.right - 16.f * scale,
                                  rowRect.top + 39.f * scale },
                                row.displayName, d2d::Colors::WHITE, Renderer::FontSelection::PrimaryRegular,
                                20.f * scale);
        dc.drawSingleLineFitted({ textLeft, rowRect.top + 41.f * scale, rowRect.right - 16.f * scale,
                                  rowRect.bottom - 8.f * scale },
                                row.detail, d2d::Color::RGB(0xD2, 0xD2, 0xD2).asAlpha(0.68f),
                                Renderer::FontSelection::PrimaryRegular, 13.f * scale);
    }

    dc.ctx->PopAxisAlignedClip();

    if (scrollMax > 0.f) {
        float trackWidth = 4.f * scale;
        d2d::Rect track { listRect.right + 9.f * scale, listRect.top, listRect.right + 9.f * scale + trackWidth,
                          listRect.bottom };
        float thumbHeight =
            std::max(24.f * scale, listRect.getHeight() * (listRect.getHeight() / (listRect.getHeight() + scrollMax)));
        float thumbY = track.top + ((track.getHeight() - thumbHeight) * (scroll / scrollMax));
        scrollbarTrackRect = track;
        scrollbarThumbRect = { track.left, thumbY, track.right, thumbY + thumbHeight };
        dc.fillRoundedRectangle(scrollbarTrackRect, d2d::Color::RGB(0x55, 0x55, 0x55).asAlpha(0.28f),
                                trackWidth * 0.5f);
        dc.fillRoundedRectangle(scrollbarThumbRect,
                                (draggingScrollbar || scrollbarThumbRect.contains(cursorPos))
                                    ? accent
                                    : d2d::Color::RGB(0xD2, 0xD2, 0xD2).asAlpha(0.78f),
                                trackWidth * 0.5f);
    } else {
        scrollbarTrackRect = {};
        scrollbarThumbRect = {};
    }
}

void SkinsScreen::onClick(Event& evGeneric) {
    ClickEvent& ev = reinterpret_cast<ClickEvent&>(evGeneric);
    if (!isActive()) return;

    ClickEvent::ClickType clickType = ev.getClickType();
    if (clickType != ClickEvent::ClickType::None) ev.setCancelled(true);

    if (clickType == ClickEvent::ClickType::Wheel) {
        scroll = std::clamp(scroll - static_cast<float>(ev.getWheelDelta()) / 3.f, 0.f, scrollMax);
        return;
    }

    if (clickType != ClickEvent::ClickType::Left) return;

    Vec2 cursorPos = SDK::ClientInstance::get()->cursorPos;
    mouseButtons[0] = ev.isDown();
    if (!ev.isDown()) {
        draggingScrollbar = false;
        return;
    }

    if (!panelRect.contains(cursorPos)) {
        close();
        return;
    }

    if (closeButtonRect.contains(cursorPos)) {
        playClickSound();
        close();
        return;
    }

    if (refreshButtonRect.contains(cursorPos)) {
        playClickSound();
        previews.clear();
        rowsDirty = true;
        rebuildRows();
        setStatus(util::FormatWString(LocalizeString::get(rows.size() == 1 ? "client.screen.skins.count.one"
                                                                          : "client.screen.skins.count.many"),
                                      { std::to_wstring(rows.size()) }),
                  true);
        return;
    }

    if (syncButtonRect.contains(cursorPos)) {
        playClickSound();
        runSync();
        return;
    }

    if (folderButtonRect.contains(cursorPos)) {
        playClickSound();
        openFolder();
        return;
    }

    if (scrollMax > 0.f && scrollbarTrackRect.contains(cursorPos)) {
        draggingScrollbar = true;
        if (scrollbarThumbRect.contains(cursorPos)) {
            scrollbarDragOffset = cursorPos.y - scrollbarThumbRect.top;
        } else {
            scrollbarDragOffset = scrollbarThumbRect.getHeight() * 0.5f;
            updateScrollbarDrag(cursorPos);
        }
        return;
    }
}

void SkinsScreen::onKey(Event& evGeneric) {
    KeyUpdateEvent& ev = reinterpret_cast<KeyUpdateEvent&>(evGeneric);
    if (!isActive()) return;
    if (ev.getKey() == VK_F11) return;

    if (ev.isDown() && ev.getKey() == VK_ESCAPE) {
        close();
    }

    ev.setCancelled(true);
}
