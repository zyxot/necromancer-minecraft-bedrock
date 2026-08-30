#pragma once
#include "client/misc/EntityCache.h"
#include "client/misc/PlayerListManager.h"
#include "mc/common/client/player/LocalPlayer.h"
#include "mc/common/world/actor/player/Player.h"
#include <cmath>
#include <optional>

struct CombatTargetQuery {
    bool players = true;
    bool mobs = true;
    int mode = 0;
    float range = 6.f;
    float fov = 90.f;
    bool ignoreFriends = true;
};

struct CombatTargetResult {
    SDK::Actor* actor = nullptr;
    uint64_t runtimeId = 0;
    AABB box {};
    float distance = 0.f;
};

namespace CombatTarget {
    inline Vec3 lookDirOf(Vec2 const& rot) {
        float yaw = (rot.y + 90.f) * (pi_f / 180.f);
        float pitch = rot.x * -(pi_f / 180.f);
        return { cosf(yaw) * cosf(pitch), sinf(pitch), sinf(yaw) * cosf(pitch) };
    }

    inline std::optional<CombatTargetResult> find(SDK::LocalPlayer* lp, CombatTargetQuery const& q) {
        if (!lp || !lp->aabbShape) return std::nullopt;

        AABB selfBox = lp->aabbShape->boundingBox;
        Vec3 eye { (selfBox.lower.x + selfBox.higher.x) * 0.5f, selfBox.higher.y - 0.18f,
                   (selfBox.lower.z + selfBox.higher.z) * 0.5f };
        Vec3 forward = lookDirOf(lp->getRot());

        struct Entry {
            CombatTargetResult res;
            float score;
        };

        std::vector<Entry> entries;
        auto snap = EntityCache::get().snapshot();
        for (auto const& view : snap->views) {
            SDK::Actor* entt = view.actor;
            if (!entt || entt == lp || !entt->aabbShape) continue;
            bool isPlayer = view.isPlayer;
            if (isPlayer ? !q.players : !q.mobs) continue;
            if (view.invisible) continue;
            if (!view.hasHealth || view.health <= 0.f) continue;
            if (q.ignoreFriends && isPlayer &&
                PlayerListManager::get().isFriend(reinterpret_cast<SDK::Player*>(entt)->playerName))
                continue;

            AABB bounds = entt->getBoundingBox();
            float distance = eye.distance(bounds.getCenter());
            if (distance > q.range) continue;

            float score = distance;
            if (q.mode == 1) {
                Vec3 dir = (bounds.getCenter() - eye).normalized();
                float angle = std::acos(std::clamp(forward.x * dir.x + forward.y * dir.y + forward.z * dir.z, -1.f, 1.f)) *
                              (180.f / 3.14159265358979323846f);
                if (angle > q.fov) continue;
                score = angle;
            } else if (q.mode == 2) {
                score = view.health;
            }

            entries.push_back({ { entt, view.runtimeId, bounds, distance }, score });
        }

        if (entries.empty()) return std::nullopt;

        std::sort(entries.begin(), entries.end(), [](Entry const& a, Entry const& b) { return a.score < b.score; });
        return entries.front().res;
    }
}
