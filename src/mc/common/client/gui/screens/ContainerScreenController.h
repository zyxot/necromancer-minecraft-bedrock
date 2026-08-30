#pragma once
#include "ScreenController.h"
#include <string>
#include "mc/Util.h"

namespace SDK {
    class ItemStack;

    class ContainerScreenController : public ScreenController {
    public:
        CLASS_FIELD(void*, containerManager, 0x12C0);

        void clickSlot(const std::string& collection, int slot);
        void dropSlot(const std::string& collection, int slot);
        void* _getSelectedSlotInfo();
        void handleAutoPlace(const std::string& collection, int slot);
        ItemStack* getItemStack(const std::string& collection, int slot);

        void autoPlaceSlot(const std::string& collection, int slot, int64_t count);
        int transferSlot(const std::string& srcColl, int srcSlot, const std::string& dstColl, int dstSlot);
        bool tryExit();
    };
}
