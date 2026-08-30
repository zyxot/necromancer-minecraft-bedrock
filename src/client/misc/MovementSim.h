#pragma once
#include "util/LMath.h"
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace SDK {
    class Actor;
    class BlockSource;
}

namespace MovementSim {
    struct Result {
        BlockPos landingBlock;
        float faceY;
        int ticksToImpact;
        Vec3 impactPos;
        Vec2 faceMin;
        Vec2 faceMax;
        bool landsInLiquid;
    };

    struct ForwardSample {
        Vec3 pos;
        AABB box;
        Vec3 velocity;
        bool onGround;
    };

    struct ForwardResult {
        bool valid = false;
        AABB finalBox;
        Vec3 finalPos;
        Vec3 finalVelocity;
        std::vector<ForwardSample> trace;
        bool hitWall = false;
        bool hitFloor = false;
    };

    struct Pattern {
        enum class Kind : uint8_t {
            None = 0,
            CircleStrafe,
            Bunnyhop,
            Strafe,
        };

        Kind kind = Kind::None;
        float turnPerTick = 0.f;
        int periodTicks = 0;
    };

    struct Sample {
        Vec3 pos;
        Vec3 vel;
        bool onGround;
    };

    struct SimSeed {
        Vec3 vel;
        float turnPerTick = 0.f;
    };

    bool isLiquidAt(SDK::BlockSource* region, BlockPos const& pos);

    std::optional<Result> predictLanding(SDK::Actor* actor, int maxTicks = 200);

    ForwardResult predictForward(SDK::Actor* actor, Vec3 seedVel, int ticks, bool recordTrace = false,
                                 bool sustainGround = false, float turnPerTickDeg = 0.f);

    void trackEntity(SDK::Actor* actor, bool isPlayer);
    void clearTracking();
    Pattern patternOf(uint64_t runtimeId);
    std::optional<SimSeed> simSeed(uint64_t runtimeId);
    std::optional<Sample> latestSample(uint64_t runtimeId);

    struct PredictionDraw {
        uint64_t runtimeId = 0;
        std::vector<Vec3> path;
        AABB finalBox {};
        std::chrono::steady_clock::time_point at {};
    };

    void publishPrediction(uint64_t runtimeId, ForwardResult const& result);
    std::vector<PredictionDraw> activePredictions();
}
