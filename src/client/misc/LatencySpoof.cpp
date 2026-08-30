#include "pch.h"
#include "LatencySpoof.h"
#include "client/memory/hook/Hook.h"
#include "mc/Addresses.h"

#include <WinSock2.h>
#include <ws2tcpip.h>  // sockaddr_in6
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {
    // RakNet's RNS2_Windows send descriptor. Only the first three fields matter
    // here: the datagram bytes, its length, and the destination address that
    // follows them inline (see the sendto call in the hooked function).
    // Layout taken directly from the sendto call in the hooked function:
    //   sendto(sock, *(char**)(p+0), *(DWORD*)(p+8), 0, (sockaddr*)(p+0x10), len)
    struct RNS2_SendParameters {
        char* data;       // +0x00
        uint32_t length;  // +0x08
        uint32_t unused;  // +0x0C
        // destination sockaddr blob lives inline at +0x10
    };
    static constexpr size_t sendParamAddrOffset = Signatures::FieldOffset::RakNetSendParameters::systemAddress;
    // Per-send TTL. When > 0 the game brackets the sendto with getsockopt/setsockopt
    // to change TTL for that one datagram, which means it is a probe (MTU discovery
    // / LAN scan), not game traffic. We cannot reproduce that bracketing on replay,
    // so those are never delayed.
    static constexpr size_t sendParamTtlOffset = Signatures::FieldOffset::RakNetSendParameters::ttl;
    // The RakNet socket object keeps the OS handle here (v6 = *(int*)(a1 + 184)).
    static constexpr size_t socketHandleOffset = Signatures::FieldOffset::RakNetSendParameters::socketHandle;
    // An optional alternate send interface gets first refusal (a1 + 264): if it is
    // present and succeeds, the real function never reaches sendto at all. When it
    // exists we must not delay, because the OS handle at +184 is then not
    // necessarily the live transport and a replayed datagram would go nowhere.
    static constexpr size_t altInterfaceOffset = Signatures::FieldOffset::RakNetSendParameters::altInterface;

    // Windows address families, as compared in the original function.
    constexpr uint16_t familyIPv4 = 2;   // AF_INET  -> tolen 16
    constexpr uint16_t familyIPv6 = 23;  // AF_INET6 -> tolen 28

    struct HeldDatagram {
        std::vector<char> bytes;
        sockaddr_storage to {};
        int toLen = 0;
        SOCKET sock = INVALID_SOCKET;
        std::chrono::steady_clock::time_point releaseAt {};
    };

    std::shared_ptr<Hook> sendHook;
    std::atomic<uint32_t> latencyMs { 0 };
    std::atomic<bool> choking { false };
    std::atomic<bool> installed { false };
    std::atomic<bool> running { false };
    std::atomic<uint64_t> held { 0 };
    std::atomic<uint64_t> passed { 0 };
    std::atomic<uint64_t> choked { 0 };

    // One-shot bypass, armed by the packet layer right before an attack is sent.
    std::atomic<int> bypassCount { 0 };
    std::atomic<int64_t> bypassArmedAtMs { 0 };
    // If the datagram we expected never shows up, stop honouring the bypass rather
    // than letting it apply to whatever happens to be sent next.
    constexpr int64_t bypassWindowMs = 60;

    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    constexpr size_t maxQueuedDatagrams = 2048;

    std::mutex sendGate;
    std::mutex queueLock;
    std::deque<HeldDatagram> queue;
    std::deque<HeldDatagram> chokeQueue;
    std::thread releaseThread;

    using SendFn = int64_t (*)(void*, void*);

    constexpr size_t socketRecvHandlerOffset = 0xF0;

    // The recv thread allocates a 1760-byte RNS2RecvStruct per datagram and hands
    // ownership to OnRNS2Recv (handler vtable slot 1). Holding the pointer is
    // therefore ownership-correct: on release we call the original OnRNS2Recv
    // with it and the game's pipeline consumes and frees it as if it had just
    // arrived off the socket.
    struct HeldRecv {
        void* handler;
        void* recvStruct;
        std::chrono::steady_clock::time_point releaseAt;
    };

    std::mutex recvLock;
    std::deque<HeldRecv> recvQueue;
    std::atomic<uint32_t> inboundDelayMs { 0 };
    std::atomic<bool> inboundFrozen { false };
    std::atomic<uint64_t> inboundHeldTotal { 0 };
    constexpr size_t maxHeldRecv = 4096;

    void* recvHandlerSlot = nullptr;
    void* recvOriginalFn = nullptr;
    std::atomic<bool> recvHookInstalled { false };

    using RecvFn = void (*)(void*, void*);

    bool inboundControlActive() {
        return inboundFrozen.load(std::memory_order_acquire) || inboundDelayMs.load(std::memory_order_relaxed) > 0;
    }

    void armRecvHook(void* socket);

    void recvDetour(void* handler, void* recvStruct) {
        if (!recvHookInstalled.load(std::memory_order_acquire)) {
            reinterpret_cast<RecvFn>(recvOriginalFn)(handler, recvStruct);
            return;
        }

        bool frozen = inboundFrozen.load(std::memory_order_acquire);
        uint32_t delay = inboundDelayMs.load(std::memory_order_relaxed);
        if (!frozen && delay == 0) {
            reinterpret_cast<RecvFn>(recvOriginalFn)(handler, recvStruct);
            return;
        }

        {
            std::lock_guard lk(recvLock);
            if (!inboundControlActive()) {
                reinterpret_cast<RecvFn>(recvOriginalFn)(handler, recvStruct);
                return;
            }
            if (recvQueue.size() >= maxHeldRecv) {
                // Bounded hold: feed the oldest through so the backlog can never
                // outgrow the cap. Ownership of the delivered one moves to the game.
                auto& front = recvQueue.front();
                reinterpret_cast<RecvFn>(recvOriginalFn)(front.handler, front.recvStruct);
                recvQueue.pop_front();
            }
            recvQueue.push_back({ handler, recvStruct,
                                  frozen ? std::chrono::steady_clock::time_point::max()
                                         : std::chrono::steady_clock::now() +
                                               std::chrono::milliseconds(delay) });
        }
        inboundHeldTotal.fetch_add(1, std::memory_order_relaxed);
    }

    void flushInbound() {
        std::deque<HeldRecv> pending;
        {
            std::lock_guard lk(recvLock);
            pending.swap(recvQueue);
        }
        for (auto& entry : pending) reinterpret_cast<RecvFn>(recvOriginalFn)(entry.handler, entry.recvStruct);
    }

    void pumpInboundRecv() {
        if (!inboundControlActive()) return;
        if (inboundFrozen.load(std::memory_order_acquire)) return;

        auto now = std::chrono::steady_clock::now();
        std::deque<HeldRecv> due;
        {
            std::lock_guard lk(recvLock);
            while (!recvQueue.empty() && recvQueue.front().releaseAt <= now) {
                due.push_back(recvQueue.front());
                recvQueue.pop_front();
            }
        }
        for (auto& entry : due) reinterpret_cast<RecvFn>(recvOriginalFn)(entry.handler, entry.recvStruct);
    }

    bool installRecvHook();

    void uninstallRecvHook() {
        if (!recvHookInstalled.exchange(false, std::memory_order_acq_rel)) return;
        if (!recvHandlerSlot || !recvOriginalFn) return;
        DWORD oldProtect = 0;
        if (!VirtualProtect(recvHandlerSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return;
        *static_cast<void**>(recvHandlerSlot) = recvOriginalFn;
        VirtualProtect(recvHandlerSlot, sizeof(void*), oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), recvHandlerSlot, sizeof(void*));
    }

    void armRecvHook(void* socket) {
        if (!socket || recvHandlerSlot) return;
        void* handler = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(socket) + socketRecvHandlerOffset);
        if (!handler) return;
        void** vft = *reinterpret_cast<void***>(handler);
        if (!vft || !vft[1]) return;
        recvOriginalFn = vft[1];
        recvHandlerSlot = vft + 1;
    }

    void sendDatagram(HeldDatagram& datagram) {
        if (datagram.sock == INVALID_SOCKET || datagram.bytes.empty()) return;
        ::sendto(datagram.sock, datagram.bytes.data(), static_cast<int>(datagram.bytes.size()), 0,
                 reinterpret_cast<sockaddr*>(&datagram.to), datagram.toLen);
    }

    HeldDatagram captureDatagram(void* self, uint8_t* base, char const* data, uint32_t len, uint16_t family) {
        HeldDatagram datagram;
        datagram.sock =
            static_cast<SOCKET>(*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(self) + socketHandleOffset));
        datagram.toLen = family == familyIPv6 ? 28 : 16;
        std::memcpy(&datagram.to, base + sendParamAddrOffset, static_cast<size_t>(datagram.toLen));
        datagram.bytes.assign(data, data + len);
        return datagram;
    }

    void drainQueueLocked(std::deque<HeldDatagram>& source) {
        std::deque<HeldDatagram> pending;
        {
            std::lock_guard lk(queueLock);
            pending.swap(source);
        }
        for (auto& datagram : pending) sendDatagram(datagram);
    }

    // RakNet datagram header bits. A datagram is only delayed when it is
    // positively identified as plain reliable/unreliable data. ACK and NAK
    // carry no game state and must never be delayed: starving the peer of ACKs
    // stalls its send window, which is exactly what makes a generic shaper
    // freeze the world and then drop the connection past ~200ms.
    constexpr uint8_t bitIsValid = 0x80;
    constexpr uint8_t bitIsAck = 0x40;
    constexpr uint8_t bitIsNak = 0x20;

    bool isDelayableData(char const* data, uint32_t len) {
        if (!data || len < 1) return false;
        auto flags = static_cast<uint8_t>(data[0]);
        if (!(flags & bitIsValid)) return false;  // offline//handshake message
        if (flags & bitIsAck) return false;       // never delay acknowledgements
        if (flags & bitIsNak) return false;       // nor retransmit requests
        return true;
    }

    void releaseLoop() {
        while (running.load(std::memory_order_relaxed)) {
            auto now = std::chrono::steady_clock::now();
            pumpInboundRecv();
            for (;;) {
                HeldDatagram out;
                {
                    std::lock_guard sendLock(sendGate);
                    {
                        std::lock_guard lk(queueLock);
                        if (queue.empty() || queue.front().releaseAt > now) break;
                        out = std::move(queue.front());
                        queue.pop_front();
                    }
                    sendDatagram(out);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    int64_t detour(void* self, void* params) {
        std::lock_guard sendLock(sendGate);
        armRecvHook(self);
        uint32_t delay = latencyMs.load(std::memory_order_relaxed);
        bool holdForChoke = choking.load(std::memory_order_acquire);
        if ((!holdForChoke && delay == 0) || !params) {
            passed.fetch_add(1, std::memory_order_relaxed);
            return sendHook->oFunc<SendFn>()(self, params);
        }

        auto* base = reinterpret_cast<uint8_t*>(params);
        auto* sp = reinterpret_cast<RNS2_SendParameters*>(params);
        char const* data = sp->data;
        uint32_t len = sp->length;

        // If the alternate send interface is installed, the original returns before
        // ever reaching sendto, so the handle at +184 is not necessarily what the
        // datagram would have travelled over. Don't touch those.
        if (*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(self) + altInterfaceOffset)) {
            passed.fetch_add(1, std::memory_order_relaxed);
            return sendHook->oFunc<SendFn>()(self, params);
        }

        // TTL-overridden sends are probes wrapped in getsockopt/setsockopt. Replaying
        // one later would send it with the wrong TTL, so leave them alone.
        if (*reinterpret_cast<int32_t*>(base + sendParamTtlOffset) > 0) {
            passed.fetch_add(1, std::memory_order_relaxed);
            return sendHook->oFunc<SendFn>()(self, params);
        }

        // Only IPv4/IPv6 reach sendto in the original; any other family falls out of
        // the loop without sending, so mirroring that means passing it through.
        auto* addr = reinterpret_cast<sockaddr*>(base + sendParamAddrOffset);
        uint16_t family = addr->sa_family;
        if (family != familyIPv4 && family != familyIPv6) {
            passed.fetch_add(1, std::memory_order_relaxed);
            return sendHook->oFunc<SendFn>()(self, params);
        }

        if (!isDelayableData(data, len)) {
            passed.fetch_add(1, std::memory_order_relaxed);
            return sendHook->oFunc<SendFn>()(self, params);
        }

        // Attack bypass: the packet layer armed this just before an attack, so send
        // this datagram now instead of holding it. Checked after the data test so a
        // stale bypass can never rush an ACK.
        if (bypassCount.load(std::memory_order_relaxed) > 0) {
            if (nowMs() - bypassArmedAtMs.load(std::memory_order_relaxed) <= bypassWindowMs) {
                bypassCount.fetch_sub(1, std::memory_order_relaxed);
                passed.fetch_add(1, std::memory_order_relaxed);
                return sendHook->oFunc<SendFn>()(self, params);
            }
            bypassCount.store(0, std::memory_order_relaxed);
        }

        if (holdForChoke) {
            auto datagram = captureDatagram(self, base, data, len, family);
            {
                std::lock_guard lk(queueLock);
                if (!choking.load(std::memory_order_acquire)) {
                    sendDatagram(datagram);
                    passed.fetch_add(1, std::memory_order_relaxed);
                    return static_cast<int64_t>(len);
                }
                if (chokeQueue.size() >= maxQueuedDatagrams) chokeQueue.pop_front();
                chokeQueue.push_back(std::move(datagram));
            }
            choked.fetch_add(1, std::memory_order_relaxed);
            return static_cast<int64_t>(len);
        }

        HeldDatagram hd = captureDatagram(self, base, data, len, family);
        hd.releaseAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);

        {
            std::lock_guard lk(queueLock);
            // Bounded so a stall cannot grow without limit. At ~60 datagrams/s
            // even a 1s delay only needs a fraction of this.
            if (queue.size() >= maxQueuedDatagrams) queue.pop_front();
            queue.push_back(std::move(hd));
        }
        held.fetch_add(1, std::memory_order_relaxed);

        // Report success to RakNet: from its point of view the datagram is gone.
        return static_cast<int64_t>(len);
    }
}

namespace LatencySpoof {
    void init() {
        if (installed.load()) return;
        auto target = Signatures::RakNetSocket_send.result;
        if (!target) return;

        static auto* group = new HookGroup("LatencySpoof");
        sendHook = group->addHook(target, &detour, "RakNetSocket::Send");
        if (!sendHook) return;
        sendHook->enable();

        running.store(true);
        releaseThread = std::thread(releaseLoop);
        installed.store(true);
    }

    void shutdown() {
        choking.store(false, std::memory_order_release);
        latencyMs.store(0, std::memory_order_release);
        inboundFrozen.store(false, std::memory_order_release);
        inboundDelayMs.store(0, std::memory_order_release);
        running.store(false, std::memory_order_release);
        if (releaseThread.joinable()) releaseThread.join();
        // Restore the handler vtable BEFORE replaying held structs: after the slot
        // is back to the original, nothing enters our detour anymore, so the
        // backlog can drain safely through the game's own handler.
        uninstallRecvHook();
        flushInbound();
        {
            std::lock_guard sendLock(sendGate);
            drainQueueLocked(queue);
            drainQueueLocked(chokeQueue);
        }
        installed.store(false, std::memory_order_release);
    }

    void setLatency(uint32_t ms) {
        std::lock_guard sendLock(sendGate);
        latencyMs.store(ms, std::memory_order_relaxed);
        if (ms == 0) drainQueueLocked(queue);
    }

    uint32_t getLatency() { return latencyMs.load(std::memory_order_relaxed); }

    void setChoking(bool enabled) {
        std::lock_guard sendLock(sendGate);
        if (enabled) {
            drainQueueLocked(queue);
            choking.store(true, std::memory_order_release);
            return;
        }
        if (!choking.exchange(false, std::memory_order_acq_rel)) return;
        drainQueueLocked(chokeQueue);
    }

    void clearChoke() {
        std::lock_guard sendLock(sendGate);
        choking.store(false, std::memory_order_release);
        std::lock_guard queueGuard(queueLock);
        chokeQueue.clear();
    }

    bool isChoking() { return choking.load(std::memory_order_acquire); }
    uint64_t chokedCount() { return choked.load(std::memory_order_relaxed); }

    void requestBypass(int count) {
        if (count < 1) return;
        bypassArmedAtMs.store(nowMs(), std::memory_order_relaxed);
        bypassCount.store(count, std::memory_order_relaxed);
    }
    bool hooked() { return installed.load(); }
    uint64_t heldCount() { return held.load(std::memory_order_relaxed); }
    uint64_t passedCount() { return passed.load(std::memory_order_relaxed); }

    bool installRecvHook() {
        if (recvHookInstalled.load(std::memory_order_acquire)) return true;
        std::lock_guard sendLock(sendGate);
        if (recvHookInstalled.load(std::memory_order_acquire)) return true;
        if (!recvHandlerSlot || !recvOriginalFn) return false;

        DWORD oldProtect = 0;
        if (!VirtualProtect(recvHandlerSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
        *static_cast<void**>(recvHandlerSlot) = reinterpret_cast<void*>(&recvDetour);
        VirtualProtect(recvHandlerSlot, sizeof(void*), oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), recvHandlerSlot, sizeof(void*));

        recvHookInstalled.store(true, std::memory_order_release);
        return true;
    }

    void setInboundDelay(uint32_t ms) {
        inboundDelayMs.store(ms, std::memory_order_relaxed);
        if (ms > 0 || inboundFrozen.load(std::memory_order_acquire)) {
            if (!installRecvHook()) inboundDelayMs.store(0, std::memory_order_relaxed);
            return;
        }
        if (recvHookInstalled.load(std::memory_order_acquire)) flushInbound();
    }

    void setInboundFrozen(bool frozen) {
        if (frozen) {
            inboundFrozen.store(true, std::memory_order_release);
            installRecvHook();
            return;
        }
        if (!inboundFrozen.exchange(false, std::memory_order_acq_rel)) return;
        if (recvHookInstalled.load(std::memory_order_acquire)) flushInbound();
    }

    bool inboundHooked() { return recvHookInstalled.load(std::memory_order_acquire); }
    uint64_t inboundHeldCount() { return inboundHeldTotal.load(std::memory_order_relaxed); }

    uint32_t getInboundDelay() { return inboundDelayMs.load(std::memory_order_relaxed); }
}
