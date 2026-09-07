#include "pch.h"
#include "MaterialPtr.h"
#include "util/Util.h"
#include "client/Necromancer.h"
#include <charconv>

SDK::MaterialPtr* SDK::MaterialPtr::getUIColor() {
    static auto uiFillColorMaterial = createMaterial(HashedString("ui_fill_color"));
    return uiFillColorMaterial;
}

SDK::MaterialPtr* SDK::MaterialPtr::getUITextureAndColor() {
    static auto uiTexturedMaterial = createMaterial(HashedString("ui_textured"));
    return uiTexturedMaterial;
}

SDK::MaterialPtr* SDK::MaterialPtr::getSelectionBoxMaterial() {
    static auto material = SDK::MaterialPtr::createMaterial(HashedString("selection_box"));
    return material;
};
SDK::MaterialPtr* SDK::MaterialPtr::getSelectionOverlayMaterial() {
    static auto material = SDK::MaterialPtr::createMaterial(HashedString("selection_overlay"));
    return material;
};

SDK::MaterialPtr* SDK::MaterialPtr::createMaterial(const SDK::HashedString& name) {
    static class RenderMaterialGroup* materialGroup =
        Signatures::RenderMaterialGroup__common.as_ptr<class RenderMaterialGroup>();

    // Bedrock 1.26.44 changed this virtual from returning MaterialPtr* directly
    // to writing a shared_ptr<MaterialPtr> through an output parameter. Keep the
    // legacy call for 1.26.40-1.26.43 and use the new ABI for 1.26.44+.
    int build = 44;
    const auto& version = Necromancer::get().gameVersion;
    if (const auto dot = version.rfind('.'); dot != std::string::npos && dot + 1 < version.size()) {
        std::from_chars(version.data() + dot + 1, version.data() + version.size(), build);
    }

    if (build < 44) {
        return memory::callVirtual<SDK::MaterialPtr*, const SDK::HashedString&>(
            materialGroup, Signatures::VtableIndex::RenderMaterialGroup::createMaterial, name);
    }

    std::shared_ptr<SDK::MaterialPtr> material {};
    memory::callVirtual<void, std::shared_ptr<SDK::MaterialPtr>&, const SDK::HashedString&>(
        materialGroup, Signatures::VtableIndex::RenderMaterialGroup::createMaterial, material, name);
    return material.get();
}
