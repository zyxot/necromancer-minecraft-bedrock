#pragma once
class UpdatePlayerCameraEvent : public Event {
public:
    static const uint32_t hash = TOHASH(UpdatePlayerCameraEvent);

    void setViewAngles(Vec2 const& angles) { newViewAngles = angles; }
    void setViewAnglesPersistent(Vec2 const& angles) {
        newViewAngles = angles;
        persistent = true;
    }

    void setViewPosition(Vec3 const& position) { newPosition = position; }

    std::optional<Vec2> getNewRot() { return newViewAngles; }
    std::optional<Vec3> getNewPosition() { return newPosition; }
    bool isPersistent() const { return persistent; }

private:
    std::optional<Vec2> newViewAngles;
    std::optional<Vec3> newPosition;
    bool persistent = false;
};
