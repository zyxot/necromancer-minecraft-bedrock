#include "pch.h"
#include "PingDisplay.h"
#include "mc/common/network/RemoteConnectorComposite.h"
#include "client/event/events/AveragePingEvent.h"
#include "client/event/Eventing.h"
#include "client/misc/RealPing.h"

PingDisplay::PingDisplay()
    : TextModule("PingDisplay", LocalizeString::get("client.textmodule.pingDisplay.name"),
                 LocalizeString::get("client.textmodule.pingDisplay.desc"), HUD) {
    this->suffix = TextValue(L" ms");

    listen<AveragePingEvent>((EventListenerFunc)&PingDisplay::onAvgPing, true);
}

std::wstringstream PingDisplay::text(bool isDefault, bool inEditor) {
    std::wstringstream wss;
    int dPing = 0;

    auto* connectionInfo = SDK::RemoteConnectorComposite::getConnectionInfo();
    if (connectionInfo && !connectionInfo->hostIpAddress.empty()) {
        dPing = static_cast<int>(RealPing::get());
    }

    wss << dPing;

    return wss;
}

void PingDisplay::onAvgPing(Event& evGeneric) {
    AveragePingEvent& ev = static_cast<AveragePingEvent&>(evGeneric);

    RealPing::recordRaw(static_cast<uint32_t>(ev.getPing()));
    ping = static_cast<int>(RealPing::get());
}
