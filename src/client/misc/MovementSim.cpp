#include "pch.h"
#include "MovementSim.h"
#include "BlockSolid.h"
#include "util/Logger.h"
#include <mc/Addresses.h>
#include <mc/common/world/actor/Actor.h>
#include <mc/common/world/level/Dimension.h>
#include <mc/common/world/level/BlockSource.h>
#include <mc/common/world/level/block/Block.h>
#include <mc/common/world/level/block/BlockLegacy.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
    constexpr float gravity = 0.08f;
    constexpr float verticalDrag = 0.98f;
    constexpr float horizontalDrag = 0.91f;
    constexpr float terminalVelocity = -3.92f;
    constexpr float eps = 0.00001f;
    constexpr float inset = 0.001f;
    constexpr int worldFloorY = -64;
    constexpr size_t fetchAABBsVtIndex = Signatures::VtableIndex::BlockSourceExtra::fetchAABBs;

    std::atomic<int> shapeMode { 0 };

    int floorI(float v) {
        return static_cast<int>(std::floor(v));
    }

    bool blockIsLiquid(SDK::BlockSource* region, int x, int y, int z) {
        return BlockSolid::isLiquidAt(region, x, y, z);
    }

    bool fetchShapesRaw(SDK::BlockSource* region, std::vector<AABB>* out, AABB const* query) {
        __try {
            using Fn = void(__fastcall*)(void*, std::vector<AABB>&, AABB const&, bool);
            Fn* vtable = *reinterpret_cast<Fn**>(region);
            vtable[fetchAABBsVtIndex](region, *out, *query, false);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool validateShapes(std::vector<AABB> const& shapes, AABB const& query) {
        if (shapes.size() > 512) return false;
        for (auto const& s : shapes) {
            if (!std::isfinite(s.lower.x) || !std::isfinite(s.lower.y) || !std::isfinite(s.lower.z) ||
                !std::isfinite(s.higher.x) || !std::isfinite(s.higher.y) || !std::isfinite(s.higher.z)) {
                return false;
            }
            if (s.higher.x < s.lower.x || s.higher.y < s.lower.y || s.higher.z < s.lower.z) return false;
            if (s.higher.x - s.lower.x > 64.f || s.higher.y - s.lower.y > 64.f || s.higher.z - s.lower.z > 64.f) {
                return false;
            }
            if (s.higher.x < query.lower.x - 4.f || s.lower.x > query.higher.x + 4.f ||
                s.higher.y < query.lower.y - 4.f || s.lower.y > query.higher.y + 4.f ||
                s.higher.z < query.lower.z - 4.f || s.lower.z > query.higher.z + 4.f) {
                return false;
            }
        }
        return true;
    }

    void gridGather(SDK::BlockSource* region, AABB const& query, std::vector<AABB>& out) {
        int x0 = floorI(query.lower.x);
        int x1 = floorI(query.higher.x);
        int y0 = std::max(worldFloorY, floorI(query.lower.y));
        int y1 = floorI(query.higher.y);
        int z0 = floorI(query.lower.z);
        int z1 = floorI(query.higher.z);
        if (static_cast<long long>(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 4096) return;
        for (int x = x0; x <= x1; x++) {
            for (int y = y0; y <= y1; y++) {
                for (int z = z0; z <= z1; z++) {
                    if (!BlockSolid::isCollidable(region, x, y, z)) continue;
                    out.push_back(AABB {
                        Vec3 { static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) },
                        Vec3 { static_cast<float>(x) + 1.f, static_cast<float>(y) + 1.f, static_cast<float>(z) + 1.f },
                    });
                }
            }
        }
    }

    void gatherShapes(SDK::BlockSource* region, AABB const& query, std::vector<AABB>& out) {
        out.clear();
        int mode = shapeMode.load(std::memory_order_relaxed);
        if (mode == 2) {
            gridGather(region, query, out);
            return;
        }

        bool ok = fetchShapesRaw(region, &out, &query);
        if (!ok || !validateShapes(out, query)) {
            shapeMode.store(2, std::memory_order_relaxed);
            Logger::Warn("[MovementSim] fetchAABBs failed (ok={} count={}), using solid-block grid", ok, out.size());
            out.clear();
            gridGather(region, query, out);
            return;
        }

        if (mode == 0) {
            if (!out.empty()) {
                shapeMode.store(1, std::memory_order_relaxed);
                Logger::Info("[MovementSim] collision shapes verified ({} boxes)", out.size());
            } else {
                std::vector<AABB> grid;
                gridGather(region, query, grid);
                if (!grid.empty()) {
                    shapeMode.store(2, std::memory_order_relaxed);
                    Logger::Warn("[MovementSim] fetchAABBs returned empty where grid found {} solids, using "
                                 "solid-block grid",
                                 grid.size());
                    out = std::move(grid);
                }
            }
        }
    }

    float clipY(std::vector<AABB> const& shapes, AABB const& box, float dy) {
        for (auto const& s : shapes) {
            if (box.higher.x <= s.lower.x + eps || box.lower.x >= s.higher.x - eps) continue;
            if (box.higher.z <= s.lower.z + eps || box.lower.z >= s.higher.z - eps) continue;
            if (dy < 0.f && box.lower.y >= s.higher.y - eps) {
                float d = s.higher.y - box.lower.y;
                if (d > dy) dy = d;
            } else if (dy > 0.f && box.higher.y <= s.lower.y + eps) {
                float d = s.lower.y - box.higher.y;
                if (d < dy) dy = d;
            }
        }
        return dy;
    }

    float clipX(std::vector<AABB> const& shapes, AABB const& box, float dx) {
        for (auto const& s : shapes) {
            if (box.higher.y <= s.lower.y + eps || box.lower.y >= s.higher.y - eps) continue;
            if (box.higher.z <= s.lower.z + eps || box.lower.z >= s.higher.z - eps) continue;
            if (dx > 0.f && box.higher.x <= s.lower.x + eps) {
                float d = s.lower.x - box.higher.x;
                if (d < dx) dx = d;
            } else if (dx < 0.f && box.lower.x >= s.higher.x - eps) {
                float d = s.higher.x - box.lower.x;
                if (d > dx) dx = d;
            }
        }
        return dx;
    }

    float clipZ(std::vector<AABB> const& shapes, AABB const& box, float dz) {
        for (auto const& s : shapes) {
            if (box.higher.y <= s.lower.y + eps || box.lower.y >= s.higher.y - eps) continue;
            if (box.higher.x <= s.lower.x + eps || box.lower.x >= s.higher.x - eps) continue;
            if (dz > 0.f && box.higher.z <= s.lower.z + eps) {
                float d = s.lower.z - box.higher.z;
                if (d < dz) dz = d;
            } else if (dz < 0.f && box.lower.z >= s.higher.z - eps) {
                float d = s.higher.z - box.lower.z;
                if (d > dz) dz = d;
            }
        }
        return dz;
    }

    MovementSim::Result buildResult(SDK::BlockSource* region, std::vector<AABB> const& shapes, AABB const& box,
                                    int tick) {
        float feet = box.lower.y;
        float bestArea = 0.f;
        float ovMinX = box.lower.x;
        float ovMaxX = box.higher.x;
        float ovMinZ = box.lower.z;
        float ovMaxZ = box.higher.z;
        AABB const* bestShape = nullptr;

        for (auto const& s : shapes) {
            if (std::fabs(s.higher.y - feet) > 0.002f) continue;
            float oMinX = std::max(box.lower.x, s.lower.x);
            float oMaxX = std::min(box.higher.x, s.higher.x);
            float oMinZ = std::max(box.lower.z, s.lower.z);
            float oMaxZ = std::min(box.higher.z, s.higher.z);
            if (oMaxX - oMinX <= 0.f || oMaxZ - oMinZ <= 0.f) continue;
            float area = (oMaxX - oMinX) * (oMaxZ - oMinZ);
            if (area > bestArea) {
                bestArea = area;
                bestShape = &s;
                ovMinX = oMinX;
                ovMaxX = oMaxX;
                ovMinZ = oMinZ;
                ovMaxZ = oMaxZ;
            }
        }

        float cx = (ovMinX + ovMaxX) * 0.5f;
        float cz = (ovMinZ + ovMaxZ) * 0.5f;
        BlockPos block { floorI(cx), floorI(bestShape ? bestShape->lower.y + 0.01f : feet - 0.5f), floorI(cz) };

        bool liquid = blockIsLiquid(region, block.x, block.y + 1, block.z) ||
                      blockIsLiquid(region, block.x, block.y, block.z);

        return MovementSim::Result {
            block,
            feet,
            tick,
            Vec3 { cx, feet, cz },
            Vec2 { ovMinX, ovMinZ },
            Vec2 { ovMaxX, ovMaxZ },
            liquid,
        };
    }
}

bool MovementSim::isLiquidAt(SDK::BlockSource* region, BlockPos const& pos) {
    if (!region) return false;
    return blockIsLiquid(region, pos.x, pos.y, pos.z);
}

std::optional<MovementSim::Result> MovementSim::predictLanding(SDK::Actor* actor, int maxTicks) {
    if (!actor || !actor->aabbShape) return std::nullopt;
    auto& dim = actor->dimension;
    if (!dim) return std::nullopt;
    auto* region = dim->region;
    if (!region) return std::nullopt;

    AABB box = actor->getBoundingBox();
    Vec3 vel = actor->getVelocity();
    std::vector<AABB> shapes;
    shapes.reserve(64);

    for (int tick = 1; tick <= maxTicks; tick++) {
        vel.y = (vel.y - gravity) * verticalDrag;
        if (vel.y < terminalVelocity) vel.y = terminalVelocity;
        vel.x *= horizontalDrag;
        vel.z *= horizontalDrag;

        float reach = std::max({ NecromancerMath::abs(vel.x), NecromancerMath::abs(vel.y),
                                 NecromancerMath::abs(vel.z) }) +
                      0.5f;
        AABB query = box;
        query.lower.x -= reach;
        query.lower.y -= reach;
        query.lower.z -= reach;
        query.higher.x += reach;
        query.higher.y += reach;
        query.higher.z += reach;
        gatherShapes(region, query, shapes);

        float dy = clipY(shapes, box, vel.y);
        box.lower.y += dy;
        box.higher.y += dy;
        if (vel.y < 0.f && dy > vel.y + 0.000001f) {
            return buildResult(region, shapes, box, tick);
        }

        if (vel.y < 0.f) {
            int fy = floorI(box.lower.y);
            int fx0 = floorI(box.lower.x + inset);
            int fx1 = floorI(box.higher.x - inset);
            int fz0 = floorI(box.lower.z + inset);
            int fz1 = floorI(box.higher.z - inset);
            bool inLiquid = false;
            for (int lx = fx0; lx <= fx1 && !inLiquid; lx++) {
                for (int lz = fz0; lz <= fz1 && !inLiquid; lz++) {
                    if (blockIsLiquid(region, lx, fy, lz)) inLiquid = true;
                }
            }
            if (inLiquid) {
                float cx = (box.lower.x + box.higher.x) * 0.5f;
                float cz = (box.lower.z + box.higher.z) * 0.5f;
                return Result {
                    BlockPos { floorI(cx), fy, floorI(cz) },
                    box.lower.y,
                    tick,
                    Vec3 { cx, box.lower.y, cz },
                    Vec2 { box.lower.x, box.lower.z },
                    Vec2 { box.higher.x, box.higher.z },
                    true,
                };
            }
        }

        float dx = clipX(shapes, box, vel.x);
        box.lower.x += dx;
        box.higher.x += dx;
        if (NecromancerMath::abs(dx - vel.x) > 0.000001f) vel.x = 0.f;

        float dz = clipZ(shapes, box, vel.z);
        box.lower.z += dz;
        box.higher.z += dz;
        if (NecromancerMath::abs(dz - vel.z) > 0.000001f) vel.z = 0.f;

        if (box.lower.y < static_cast<float>(worldFloorY) - 4.f) return std::nullopt;
    }
    return std::nullopt;
}

MovementSim::ForwardResult MovementSim::predictForward(SDK::Actor* actor, Vec3 seedVel, int ticks, bool recordTrace,
                                                       bool sustainGround, float turnPerTickDeg) {
    ForwardResult out;
    if (!actor || !actor->aabbShape || ticks <= 0) return out;
    auto& dim = actor->dimension;
    if (!dim) return out;
    auto* region = dim->region;
    if (!region) return out;

    AABB box = actor->getBoundingBox();
    Vec3 vel = seedVel;
    bool grounded = sustainGround && actor->isOnGround();

    std::vector<AABB> shapes;
    shapes.reserve(64);
    if (recordTrace) out.trace.reserve(static_cast<size_t>(ticks));

    for (int tick = 1; tick <= ticks; tick++) {
        vel.y = (vel.y - gravity) * verticalDrag;
        if (vel.y < terminalVelocity) vel.y = terminalVelocity;
        if (sustainGround && grounded && turnPerTickDeg != 0.f) {
            float rad = turnPerTickDeg * (pi_f / 180.f);
            float cosT = std::cos(rad);
            float sinT = std::sin(rad);
            float nx = vel.x * cosT - vel.z * sinT;
            float nz = vel.x * sinT + vel.z * cosT;
            vel.x = nx;
            vel.z = nz;
        }
        if (!(sustainGround && grounded)) {
            vel.x *= horizontalDrag;
            vel.z *= horizontalDrag;
        }

        float reach = std::max({ NecromancerMath::abs(vel.x), NecromancerMath::abs(vel.y),
                                 NecromancerMath::abs(vel.z) }) +
                      0.5f;
        AABB query = box;
        query.lower.x -= reach;
        query.lower.y -= reach;
        query.lower.z -= reach;
        query.higher.x += reach;
        query.higher.y += reach;
        query.higher.z += reach;
        gatherShapes(region, query, shapes);

        float dy = clipY(shapes, box, vel.y);
        box.lower.y += dy;
        box.higher.y += dy;
        bool landed = false;
        if (vel.y < 0.f && dy > vel.y + 0.000001f) {
            vel.y = 0.f;
            landed = true;
            grounded = true;
            out.hitFloor = true;
        } else if (dy <= vel.y + 0.000001f) {
            grounded = false;
        }

        float dx = clipX(shapes, box, vel.x);
        box.lower.x += dx;
        box.higher.x += dx;
        if (NecromancerMath::abs(dx - vel.x) > 0.000001f) {
            vel.x = 0.f;
            out.hitWall = true;
        }

        float dz = clipZ(shapes, box, vel.z);
        box.lower.z += dz;
        box.higher.z += dz;
        if (NecromancerMath::abs(dz - vel.z) > 0.000001f) {
            vel.z = 0.f;
            out.hitWall = true;
        }

        if (box.lower.y < static_cast<float>(worldFloorY) - 4.f) return out;

        if (recordTrace) {
            out.trace.push_back({ box.getCenter(), box, vel, landed });
        }
    }

    out.valid = true;
    out.finalBox = box;
    out.finalPos = box.getCenter();
    out.finalVelocity = vel;
    return out;
}

namespace MovementSim {
    namespace {
        constexpr size_t historyLen = 48;
        constexpr int circleMinTurnTicks = 8;
        constexpr float circleMinTurnPerTick = 12.f;
        constexpr float circleMaxTurnPerTick = 45.f;
        constexpr int strafeWindow = 5;
        constexpr int strafeBaseWindow = 30;
        constexpr float strafeMinDelta = 25.f;
        constexpr float bhopMinAirTicks = 6;
        constexpr float bhopMinForwardSpeed = 0.14f;
        constexpr int bhopMinCycles = 2;
        constexpr float headingEps = 0.001f;
        constexpr float jumpVelocity = 0.42f;
        constexpr float sprintJumpBoost = 0.18f;

        struct History {
            std::vector<Sample> samples;
            Pattern cachedPattern;
            bool patternDirty = true;
            bool isPlayer = false;
        };

        std::mutex trackerMutex;
        std::unordered_map<uint64_t, History> histories;

        float headingOf(Vec3 const& vel) {
            return std::atan2(vel.z, vel.x) * (180.f / pi_f);
        }

        float wrapDeg(float a) {
            while (a > 180.f) a -= 360.f;
            while (a < -180.f) a += 360.f;
            return a;
        }

        bool sameSign(float a, float b) {
            return (a >= 0.f) == (b >= 0.f);
        }

        Pattern detectCircle(std::vector<Sample> const& s) {
            if (s.size() < static_cast<size_t>(circleMinTurnTicks) + 1) return {};

            int start = static_cast<int>(s.size()) - 1 - circleMinTurnTicks;
            float total = 0.f;
            float firstTurn = 0.f;
            for (int i = start + 1; i <= start + circleMinTurnTicks; i++) {
                float h0 = headingOf(s[i - 1].vel);
                float h1 = headingOf(s[i].vel);
                float mag0 = std::hypot(s[i - 1].vel.x, s[i - 1].vel.z);
                float mag1 = std::hypot(s[i].vel.x, s[i].vel.z);
                if (mag0 < headingEps || mag1 < headingEps) return {};
                float turn = wrapDeg(h1 - h0);
                if (std::abs(turn) < 0.5f) return {};
                if (std::abs(turn) > circleMaxTurnPerTick) return {};
                if (i == start + 1) {
                    firstTurn = turn;
                    continue;
                }
                if (!sameSign(turn, firstTurn)) return {};
                if (std::abs(turn) < circleMinTurnPerTick * 0.4f) return {};
                total += turn;
            }
            if (std::abs(total) < circleMinTurnTicks * circleMinTurnPerTick * 0.5f) return {};

            Pattern p;
            p.kind = Pattern::Kind::CircleStrafe;
            p.turnPerTick = total / static_cast<float>(circleMinTurnTicks);
            return p;
        }

        bool detectBhop(std::vector<Sample> const& s) {
            int n = static_cast<int>(s.size());
            if (n < bhopMinAirTicks * bhopMinCycles + bhopMinCycles + 1) return false;

            int i = n - 1;
            int cycles = 0;
            while (i >= 0 && cycles < bhopMinCycles) {
                if (s[i].onGround) {
                    i--;
                    continue;
                }
                int airStart = i;
                while (i >= 0 && !s[i].onGround) i--;
                int airTicks = airStart - i;
                if (airTicks < bhopMinAirTicks) return false;

                float forward = 0.f;
                int count = 0;
                for (int j = i + 1; j <= airStart; j++) {
                    forward += std::hypot(s[j].vel.x, s[j].vel.z);
                    count++;
                }
                if (count > 0) forward /= static_cast<float>(count);
                if (forward < bhopMinForwardSpeed) return false;

                cycles++;
                if (i >= 0 && s[i].onGround) {
                    int groundRun = 0;
                    int g = i;
                    while (g >= 0 && s[g].onGround) {
                        g--;
                        groundRun++;
                    }
                    if (groundRun > bhopMinAirTicks) return false;
                    i = g;
                }
            }
            return cycles >= bhopMinCycles;
        }

        Pattern detectStrafe(std::vector<Sample> const& s) {
            int n = static_cast<int>(s.size());
            if (n < strafeWindow + 2) return {};

            int recentStart = n - strafeWindow;
            float recentH = 0.f;
            float recentMag = 0.f;
            for (int i = recentStart; i < n; i++) {
                recentH += headingOf(s[i].vel);
                recentMag += std::hypot(s[i].vel.x, s[i].vel.z);
            }
            recentH /= static_cast<float>(strafeWindow);
            recentMag /= static_cast<float>(strafeWindow);
            if (recentMag < headingEps) return {};

            int baseCount = std::min(strafeBaseWindow, recentStart);
            if (baseCount < 3) return {};
            float baseH = 0.f;
            for (int i = recentStart - baseCount; i < recentStart; i++) {
                baseH += headingOf(s[i].vel);
            }
            baseH /= static_cast<float>(baseCount);

            if (std::abs(wrapDeg(recentH - baseH)) < strafeMinDelta) return {};

            Pattern p;
            p.kind = Pattern::Kind::Strafe;
            return p;
        }

        Pattern detectPattern(std::vector<Sample> const& s, bool isPlayer) {
            if (s.size() < 3) return {};
            Pattern circle = detectCircle(s);
            if (circle.kind != Pattern::Kind::None) return circle;
            if (isPlayer && detectBhop(s)) {
                Pattern p;
                p.kind = Pattern::Kind::Bunnyhop;
                return p;
            }
            if (isPlayer) return detectStrafe(s);
            return {};
        }

        std::optional<Vec3> simSeedVelocity(History& h) {
            Sample const& last = h.samples.back();

            if (h.cachedPattern.kind == Pattern::Kind::Bunnyhop && last.onGround) {
                float mag = std::hypot(last.vel.x, last.vel.z);
                if (mag < headingEps) return std::nullopt;
                float rad = headingOf(last.vel) * (pi_f / 180.f);
                return Vec3 { last.vel.x + std::cos(rad) * sprintJumpBoost, jumpVelocity,
                              last.vel.z + std::sin(rad) * sprintJumpBoost };
            }

            if (h.cachedPattern.kind == Pattern::Kind::Strafe) {
                int n = static_cast<int>(h.samples.size());
                int count = std::min(strafeWindow, n);
                if (count <= 0) return std::nullopt;
                Vec3 avg { 0.f, 0.f, 0.f };
                for (int i = n - count; i < n; i++) {
                    avg.x += h.samples[i].vel.x;
                    avg.y += h.samples[i].vel.y;
                    avg.z += h.samples[i].vel.z;
                }
                return Vec3 { avg.x / static_cast<float>(count), avg.y / static_cast<float>(count),
                              avg.z / static_cast<float>(count) };
            }

            return last.vel;
        }
    }

    void trackEntity(SDK::Actor* actor, bool isPlayer) {
        if (!actor || !actor->aabbShape) return;
        uint64_t id = actor->getRuntimeID();
        Sample sample { actor->getPos(), actor->getVelocity(), actor->isOnGround() };

        std::lock_guard lock { trackerMutex };
        auto& h = histories[id];
        h.isPlayer = isPlayer;
        if (h.samples.size() >= historyLen) {
            h.samples.erase(h.samples.begin(),
                            h.samples.begin() + static_cast<long>(h.samples.size() - historyLen + 1));
        }
        h.samples.push_back(sample);
        h.patternDirty = true;
    }

    void clearTracking() {
        std::lock_guard lock { trackerMutex };
        histories.clear();
    }

    Pattern patternOf(uint64_t runtimeId) {
        std::lock_guard lock { trackerMutex };
        auto it = histories.find(runtimeId);
        if (it == histories.end()) return {};
        History& h = it->second;
        if (h.patternDirty) {
            h.cachedPattern = detectPattern(h.samples, h.isPlayer);
            h.patternDirty = false;
        }
        return h.cachedPattern;
    }

    std::optional<Sample> latestSample(uint64_t runtimeId) {
        std::lock_guard lock { trackerMutex };
        auto it = histories.find(runtimeId);
        if (it == histories.end() || it->second.samples.empty()) return std::nullopt;
        return it->second.samples.back();
    }

    std::optional<SimSeed> simSeed(uint64_t runtimeId) {
        std::lock_guard lock { trackerMutex };
        auto it = histories.find(runtimeId);
        if (it == histories.end() || it->second.samples.empty()) return std::nullopt;
        History& h = it->second;
        if (h.patternDirty) {
            h.cachedPattern = detectPattern(h.samples, h.isPlayer);
            h.patternDirty = false;
        }

        auto vel = simSeedVelocity(h);
        if (!vel) return std::nullopt;

        Sample const& last = h.samples.back();
        float liveMag = std::hypot(last.vel.x, last.vel.z);
        if (liveMag < headingEps && h.samples.size() >= 2) {
            Sample const& prev = h.samples[h.samples.size() - 2];
            Vec3 delta { (last.pos.x - prev.pos.x), (last.pos.y - prev.pos.y), (last.pos.z - prev.pos.z) };
            float deltaMag = std::hypot(delta.x, delta.z);
            if (deltaMag > headingEps && deltaMag < 2.f) vel = delta;
        }

        SimSeed seed;
        seed.vel = *vel;
        if (h.cachedPattern.kind == Pattern::Kind::CircleStrafe) {
            seed.turnPerTick = h.cachedPattern.turnPerTick;
        }
        return seed;
    }

    namespace {
        constexpr std::chrono::milliseconds predictionTtl { 300 };
        std::mutex predictionMutex;
        std::unordered_map<uint64_t, PredictionDraw> publishedPredictions;

        void prunePredictions(std::chrono::steady_clock::time_point now) {
            std::erase_if(publishedPredictions, [&](auto const& entry) {
                return now - entry.second.at > predictionTtl;
            });
        }
    }

    void publishPrediction(uint64_t runtimeId, ForwardResult const& result) {
        if (!result.valid) return;
        auto now = std::chrono::steady_clock::now();

        PredictionDraw draw;
        draw.runtimeId = runtimeId;
        draw.finalBox = result.finalBox;
        draw.at = now;
        draw.path.reserve(result.trace.size());
        for (auto const& sample : result.trace) {
            draw.path.push_back(sample.pos);
        }

        std::lock_guard lock { predictionMutex };
        prunePredictions(now);
        publishedPredictions[runtimeId] = std::move(draw);
    }

    std::vector<PredictionDraw> activePredictions() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard lock { predictionMutex };
        prunePredictions(now);
        std::vector<PredictionDraw> out;
        out.reserve(publishedPredictions.size());
        for (auto& [id, draw] : publishedPredictions) {
            out.push_back(draw);
        }
        return out;
    }
}
