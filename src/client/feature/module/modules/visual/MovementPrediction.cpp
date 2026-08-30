#include "pch.h"
#include "MovementPrediction.h"
#include "client/event/events/RenderLevelEvent.h"
#include "client/misc/EntityCache.h"
#include "client/misc/MovementSim.h"
#include "client/feature/module/modules/visual/AntiObs.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/player/LocalPlayer.h"
#include "mc/common/client/renderer/MaterialPtr.h"
#include "util/DrawUtil3D.h"
#include <vector>

namespace {
    constexpr int style_outline = 0;
    constexpr int style_filled = 1;
    constexpr int style_both = 2;
}

MovementPrediction::MovementPrediction()
    : Module("MovementPrediction", LocalizeString::get("client.module.movementPrediction.name"),
             LocalizeString::get("client.module.movementPrediction.desc"), GAME, nokeybind) {
    addSetting("showPath", LocalizeString::get("client.module.movementPrediction.showPath.name"),
               LocalizeString::get("client.module.movementPrediction.showPath.desc"), showPath);
    addSetting("showHitbox", LocalizeString::get("client.module.movementPrediction.showHitbox.name"),
               LocalizeString::get("client.module.movementPrediction.showHitbox.desc"), showHitbox);
    addSetting("pathColor", LocalizeString::get("client.module.movementPrediction.pathColor.name"),
               LocalizeString::get("client.module.movementPrediction.pathColor.desc"), pathColor);
    addSetting("hitboxColor", LocalizeString::get("client.module.movementPrediction.hitboxColor.name"),
               LocalizeString::get("client.module.movementPrediction.hitboxColor.desc"), hitboxColor);

    hitboxStyle.addEntry(
        EnumEntry(style_outline, LocalizeString::get("client.module.movementPrediction.hitboxStyle.outline.name"),
                  LocalizeString::get("client.module.movementPrediction.hitboxStyle.outline.desc")));
    hitboxStyle.addEntry(
        EnumEntry(style_filled, LocalizeString::get("client.module.movementPrediction.hitboxStyle.filled.name"),
                  LocalizeString::get("client.module.movementPrediction.hitboxStyle.filled.desc")));
    hitboxStyle.addEntry(EnumEntry(style_both, LocalizeString::get("client.module.movementPrediction.hitboxStyle.both.name"),
                                    LocalizeString::get("client.module.movementPrediction.hitboxStyle.both.desc")));
    addEnumSetting("hitboxStyle", LocalizeString::get("client.module.movementPrediction.hitboxStyle.name"),
                   LocalizeString::get("client.module.movementPrediction.hitboxStyle.desc"), hitboxStyle);

    addSliderSetting("thickness", LocalizeString::get("client.module.movementPrediction.thickness.name"),
                     LocalizeString::get("client.module.movementPrediction.thickness.desc"), thickness,
                     FloatValue(0.05f), FloatValue(1.f), FloatValue(0.05f));

    listen<RenderLevelEvent>(static_cast<EventListenerFunc>(&MovementPrediction::onRenderLevel));
}

void MovementPrediction::onRenderLevel(Event& evG) {
    auto& ev = reinterpret_cast<RenderLevelEvent&>(evG);
    if (AntiObs::isActive()) return;

    auto ci = SDK::ClientInstance::get();
    if (!ci || !ci->levelRenderer || !SDK::ScreenContext::instance3d) return;
    auto lp = ci->getLocalPlayer();
    if (!lp) return;

    auto predictions = MovementSim::activePredictions();
    if (predictions.empty()) return;

    bool wantPath = std::get<BoolValue>(showPath).value;
    bool wantHitbox = std::get<BoolValue>(showHitbox).value;
    if (!wantPath && !wantHitbox) return;

    auto material = SDK::MaterialPtr::getSelectionOverlayMaterial();
    MCDrawUtil3D dc(ci->levelRenderer, SDK::ScreenContext::instance3d, material);

    float thick = std::get<FloatValue>(thickness).value / 10.f;
    d2d::Color pathCol(std::get<ColorValue>(pathColor).getMainColor());
    d2d::Color boxCol(std::get<ColorValue>(hitboxColor).getMainColor());
    int style = hitboxStyle.getSelectedKey();

    auto snap = EntityCache::get().snapshot();

    std::vector<MCDrawUtil3D::ColoredThickLine> lines;
    std::vector<MCDrawUtil3D::ColoredBox> fills;
    std::vector<MCDrawUtil3D::ColoredThickBox> outlines;

    for (auto const& pred : predictions) {
        auto view = snap->findView(pred.runtimeId);
        if (!view || !view->actor || view->actor == lp) continue;
        if (view->invisible) continue;
        if (!view->hasHealth || view->health <= 0.f) continue;

        if (wantPath && pred.path.size() >= 2) {
            Vec3 prev = pred.path.front();
            for (size_t i = 1; i < pred.path.size(); i++) {
                lines.push_back({ prev, pred.path[i], thick, pathCol });
                prev = pred.path[i];
            }
        }

        if (wantHitbox) {
            if (style != style_outline) fills.push_back({ pred.finalBox, boxCol });
            if (style != style_filled) outlines.push_back({ pred.finalBox, thick, boxCol });
        }
    }

    if (lines.empty() && fills.empty() && outlines.empty()) return;

    dc.drawThickLines(lines);
    dc.fillBoxes(fills);
    dc.drawThickBoxes(outlines);
}
