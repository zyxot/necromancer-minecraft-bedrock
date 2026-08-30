#include "pch.h"
#include "Spoofer.h"
#include "client/event/events/TickEvent.h"
#include "client/memory/hook/Hook.h"
#include "mc/Addresses.h"
#include "util/Logger.h"
#include <atomic>
#include <intrin.h>
#include <mutex>
#include <random>

namespace {
    using CreateClientData = void*(__fastcall*)(void*, void*, const std::string*, int64_t, int64_t, const std::string*,
                                                void*, unsigned int, const std::string*, const std::string*,
                                                const std::string*, unsigned char, void*, void*);

    std::mutex stateLock;
    bool idSpoofOn = false;
    bool editionOn = false;
    int fakeOs = 2;
    std::string fakeId;

    std::atomic<bool> jwtInstalled { false };
    std::atomic<bool> jwtFailed { false };
    std::atomic<bool> platformInstalled { false };
    std::atomic<bool> platformFailed { false };
    std::atomic<bool> waitLogged { false };
    std::shared_ptr<Hook> jwtHook;
    std::shared_ptr<Hook> buildPlatformHook;
    CreateClientData oCreateClientData = nullptr;
    int64_t (*oGetBuildPlatform)(void*) = nullptr;
    uintptr_t jwtStart = 0;
    constexpr size_t JwtScanRange = 0x700;

    HookGroup* hookGroup() {
        static auto* group = new HookGroup("Spoofer");
        return group;
    }

    void* __fastcall createClientDataDetour(void* result, void* a2, const std::string* deviceId, int64_t a4,
                                            int64_t a5, const std::string* a6, void* a7, unsigned int a8,
                                            const std::string* a9, const std::string* a10, const std::string* a11,
                                            unsigned char a12, void* a13, void* a14) {
        bool spoof = false;
        std::string localId;
        {
            std::lock_guard lk(stateLock);
            spoof = idSpoofOn && !fakeId.empty();
            if (spoof) localId = fakeId;
        }
        Logger::Info("[Spoofer] login client data being built (id spoof {})", spoof ? "ON" : "OFF");
        if (spoof) {
            void* r = oCreateClientData(result, a2, &localId, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            Logger::Info("[Spoofer] login DeviceId spoofed to {}", localId);
            return r;
        }
        return oCreateClientData(result, a2, deviceId, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }

    int64_t __fastcall getBuildPlatformDetour(void* self) {
        uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
        bool inJwt = jwtStart != 0 && ra >= jwtStart && ra < jwtStart + JwtScanRange;
        bool spoof = false;
        int os = 0;
        {
            std::lock_guard lk(stateLock);
            spoof = editionOn && inJwt;
            if (spoof) os = fakeOs;
        }
        if (spoof) {
            Logger::Info("[Spoofer] login DeviceOS spoofed to {}", os);
            return os;
        }
        return oGetBuildPlatform(self);
    }

    bool safeReadAppPlatform(uintptr_t globalPtr, uintptr_t& outPlatform, uintptr_t*& outVtable) {
        __try {
            outPlatform = *reinterpret_cast<uintptr_t*>(globalPtr);
            if (!outPlatform) return false;
            outVtable = *reinterpret_cast<uintptr_t**>(outPlatform);
            return outVtable != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void installJwtHook() {
        if (jwtInstalled.load(std::memory_order_acquire) || jwtFailed.load(std::memory_order_acquire)) return;

        uintptr_t jwtTarget = Signatures::ExtendedCertificate_createClientData.result;
        if (!jwtTarget) {
            jwtFailed.store(true, std::memory_order_release);
            Logger::Warn("[Spoofer] createClientData signature not resolved, DeviceId spoof unavailable");
            return;
        }

        jwtHook = hookGroup()->addHook(jwtTarget, &createClientDataDetour, "ExtendedCertificate::createClientData");
        if (!jwtHook || !jwtHook->oFunc<CreateClientData>()) {
            jwtFailed.store(true, std::memory_order_release);
            Logger::Warn("[Spoofer] failed to create createClientData hook at {:X}", jwtTarget);
            return;
        }
        oCreateClientData = jwtHook->oFunc<CreateClientData>();
        jwtStart = jwtTarget;
        jwtHook->enable();
        jwtInstalled.store(true, std::memory_order_release);
        Logger::Info("[Spoofer] DeviceId hook installed at {:X}", jwtTarget);
    }

    void installPlatformHook() {
        if (platformInstalled.load(std::memory_order_acquire) || platformFailed.load(std::memory_order_acquire)) return;

        uintptr_t globalPtr = Signatures::AppPlatform_loginInstance.result;
        if (!globalPtr) {
            platformFailed.store(true, std::memory_order_release);
            Logger::Warn("[Spoofer] AppPlatform signature not resolved, Edition Faker unavailable");
            return;
        }

        uintptr_t platform = 0;
        uintptr_t* vtable = nullptr;
        if (!safeReadAppPlatform(globalPtr, platform, vtable)) {
            if (!waitLogged.exchange(true, std::memory_order_acq_rel)) {
                Logger::Info("[Spoofer] AppPlatform not created yet, Edition Faker waiting");
            }
            return;
        }

        buildPlatformHook = hookGroup()->addTableSwapHook(reinterpret_cast<uintptr_t>(vtable + 20),
                                                         &getBuildPlatformDetour, "AppPlatform::getBuildPlatform");
        if (!buildPlatformHook) {
            platformFailed.store(true, std::memory_order_release);
            Logger::Warn("[Spoofer] failed to swap AppPlatform vtable slot 20");
            return;
        }
        oGetBuildPlatform = buildPlatformHook->oFunc<int64_t (*)(void*)>();
        if (!oGetBuildPlatform) {
            platformFailed.store(true, std::memory_order_release);
            Logger::Warn("[Spoofer] AppPlatform vtable swap returned no original");
            return;
        }

        platformInstalled.store(true, std::memory_order_release);
        Logger::Info("[Spoofer] Edition Faker hook installed (AppPlatform {:X})", platform);
    }

    void ensureInstalled() {
        installJwtHook();
        installPlatformHook();
    }

    bool hooksPending() {
        bool jwtPending = !jwtInstalled.load(std::memory_order_acquire) && !jwtFailed.load(std::memory_order_acquire);
        bool platformPending =
            !platformInstalled.load(std::memory_order_acquire) && !platformFailed.load(std::memory_order_acquire);
        return jwtPending || platformPending;
    }
}

std::string Spoofer::randomDeviceId() {
    static const char hex[] = "0123456789abcdef";
    std::mt19937 rng { std::random_device {}() };
    std::uniform_int_distribution<int> nib(0, 15);
    std::string id;
    id.reserve(36);
    for (int i = 0; i < 32; i++) {
        id += hex[nib(rng)];
        if (i == 7 || i == 11 || i == 15 || i == 19) id += '-';
    }
    return id;
}

Spoofer::Spoofer()
    : Module("Spoofer", LocalizeString::get("client.module.spoofer.name"),
             LocalizeString::get("client.module.spoofer.desc"), GAME, nokeybind) {
    edition.addEntry(EnumEntry(2, LocalizeString::get("client.module.spoofer.edition.ios.name")));
    edition.addEntry(EnumEntry(1, LocalizeString::get("client.module.spoofer.edition.android.name")));
    edition.addEntry(EnumEntry(12, LocalizeString::get("client.module.spoofer.edition.xbox.name")));
    edition.addEntry(EnumEntry(10, LocalizeString::get("client.module.spoofer.edition.playstation.name")));
    edition.addEntry(EnumEntry(11, LocalizeString::get("client.module.spoofer.edition.switch.name")));
    edition.addEntry(EnumEntry(3, LocalizeString::get("client.module.spoofer.edition.macos.name")));
    edition.addEntry(EnumEntry(13, LocalizeString::get("client.module.spoofer.edition.windowsphone.name")));
    edition.addEntry(EnumEntry(4, LocalizeString::get("client.module.spoofer.edition.fireos.name")));
    edition.addEntry(EnumEntry(5, LocalizeString::get("client.module.spoofer.edition.gearvr.name")));
    edition.addEntry(EnumEntry(6, LocalizeString::get("client.module.spoofer.edition.hololens.name")));
    edition.addEntry(EnumEntry(9, LocalizeString::get("client.module.spoofer.edition.tvos.name")));

    addSetting("editionFaker", LocalizeString::get("client.module.spoofer.editionFaker.name"),
               LocalizeString::get("client.module.spoofer.editionFaker.desc"), editionFaker);
    addEnumSetting("edition", LocalizeString::get("client.module.spoofer.edition.name"),
                   LocalizeString::get("client.module.spoofer.edition.desc"), edition, "editionFaker"_istrue);
    addSetting("spoofDeviceId", LocalizeString::get("client.module.spoofer.spoofDeviceId.name"),
               LocalizeString::get("client.module.spoofer.spoofDeviceId.desc"), spoofDeviceId);
    addSetting("autoDeviceId", LocalizeString::get("client.module.spoofer.autoDeviceId.name"),
               LocalizeString::get("client.module.spoofer.autoDeviceId.desc"), autoDeviceId, "spoofDeviceId"_istrue);
    addSetting("newIdEachJoin", LocalizeString::get("client.module.spoofer.newIdEachJoin.name"),
               LocalizeString::get("client.module.spoofer.newIdEachJoin.desc"), newIdEachJoin,
               Setting::Condition(std::vector<Setting::SingleCond> {
                   { "spoofDeviceId", { 1 }, false },
                   { "autoDeviceId", { 1 }, false },
               }));
    addSetting("deviceId", LocalizeString::get("client.module.spoofer.deviceId.name"),
               LocalizeString::get("client.module.spoofer.deviceId.desc"), deviceId,
               Setting::Condition(std::vector<Setting::SingleCond> {
                   { "spoofDeviceId", { 1 }, false },
                   { "autoDeviceId", { 0 }, false },
               }));

    listen<TickEvent>(static_cast<EventListenerFunc>(&Spoofer::onTick));
    pushState();
}

void Spoofer::onEnable() {
    if (std::get<BoolValue>(autoDeviceId).value && std::get<BoolValue>(newIdEachJoin).value) rerollId();
    ensureInstalled();
    pushState();
}

void Spoofer::onTick(Event&) {
    if (hooksPending()) ensureInstalled();
    pushState();
}

void Spoofer::pushState() {
    bool wantAuto = std::get<BoolValue>(autoDeviceId).value;
    std::string customId = util::WStrToStr(std::get<TextValue>(deviceId).str);
    bool useGenerated = wantAuto || customId.empty();

    if (useGenerated && generatedId.empty()) {
        generatedId = randomDeviceId();
        Logger::Info("[Spoofer] generated DeviceId {}", generatedId);
    }

    std::lock_guard lk(stateLock);
    idSpoofOn = std::get<BoolValue>(spoofDeviceId).value;
    editionOn = std::get<BoolValue>(editionFaker).value;
    fakeOs = edition.getSelectedKey();
    fakeId = useGenerated ? generatedId : customId;
}

void Spoofer::rerollId() {
    generatedId = randomDeviceId();
    Logger::Info("[Spoofer] rerolled DeviceId {}", generatedId);
}
