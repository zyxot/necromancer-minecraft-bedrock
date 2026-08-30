#pragma once

#include <cstdint>

namespace SDK {
    struct FallDistanceComponent : IEntityComponent {
        static constexpr uint32_t type_hash = 0xCE6B34F6;

        float fallDistance;
        bool shouldReset;
    };
}
