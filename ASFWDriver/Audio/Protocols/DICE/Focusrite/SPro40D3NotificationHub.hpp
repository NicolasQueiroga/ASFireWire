// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "../Core/DiceWaitClock.hpp"
#include "../../../../Async/Rx/LocalRequestDispatch.hpp"
#include "../../../../Discovery/DeviceRegistry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

/// Ingress only: the bus Default queue copies notification bytes here, never
/// calls the mailbox engine (which may synchronously read on the control queue).
/// The serialized control queue calls Take(), then OnNotification(frame,
/// receivedMs) on its engine. No engine/session pointers or callbacks are held.
///
/// Composition MUST provide one process-lifetime allocator for an exclusively
/// reserved, nonoverlapping local 48-bit aperture. Do not reconstruct this hub
/// with the same aperture, even after teardown/reset: released addresses are
/// never reusable because owner-release CAS is unverified and writes can be late.
/// This class registers nothing. A future dispatch must own the hub, or quiesce
/// and remove every caller before destroying it. Registry and clock outlive it.
class SPro40D3NotificationHub final : public Async::ILocalAddressHandler {
public:
    static constexpr size_t kFrameBytes = 16;
    static constexpr size_t kQueueCapacity = 32;

    struct Region {
        uint64_t begin;
        uint64_t endExclusive;
    };

    struct Lease {
        uint64_t id{0};
        // Owner address value: offset only, without local node-ID bits.
        uint64_t offset{0};
        Discovery::DeviceRouteToken route{};
    };

    struct Notification {
        std::array<uint8_t, kFrameBytes> frame{};
        uint64_t receivedMs{0};
    };

    SPro40D3NotificationHub(Discovery::DeviceRegistry& registry,
                           DiceWaitClock& clock, Region region) noexcept;
    ~SPro40D3NotificationHub() override;
    SPro40D3NotificationHub(const SPro40D3NotificationHub&) = delete;
    SPro40D3NotificationHub& operator=(const SPro40D3NotificationHub&) = delete;

    // Invalid/misaligned region => BadArgument; stale route => NotReady;
    // exhausted aperture or capability IDs => NoSpace; missing lock => NoMemory.
    [[nodiscard]] std::expected<Lease, IOReturn>
    Allocate(Discovery::DeviceRouteToken route);

    // Binds an address reserved by the process-wide local-request manager. This
    // does not allocate transport memory and never makes a released address
    // reusable; it only admits the exact address supplied by that manager.
    [[nodiscard]] std::expected<Lease, IOReturn>
    AllocateExternal(Discovery::DeviceRouteToken route, uint64_t address);

    // Linearizes against ingress and Take; clears pending frames, no bus IO.
    // A forged/stale lease cannot release a different allocation.
    void Release(const Lease& lease) noexcept;

    // Pops one owned copy, FIFO. Drops the entire queue if the route is stale.
    // A copy already taken is owned by the control queue, not revoked by Release.
    [[nodiscard]] std::optional<Notification> Take(const Lease& lease) noexcept;

    [[nodiscard]] Async::LocalRequestResult
    HandleLocalRequest(const Async::LocalRequestContext& ctx) override;
    [[nodiscard]] const char* Name() const noexcept override {
        return "SPro40D3NotificationHub";
    }

private:
    struct Entry {
        Lease lease{};
        std::array<Notification, kQueueCapacity> queue{};
        size_t head{0};
        size_t count{0};
    };

    [[nodiscard]] static bool Matches(const Entry& entry, const Lease& lease) noexcept;
    [[nodiscard]] std::expected<Lease, IOReturn>
    AllocateLocked(Discovery::DeviceRouteToken route, uint64_t address) noexcept;

    Discovery::DeviceRegistry& registry_;
    DiceWaitClock& clock_;
    const Region region_;
    const bool validRegion_;
    IOLock* lock_{nullptr};
    uint64_t nextOffset_{0};
    uint64_t nextId_{1};
    // Only active leases consume storage. The bump cursor (not this map) keeps
    // all historical addresses permanently retired. No allocation on ingress.
    std::map<uint64_t, Entry> entries_;
    std::set<uint64_t> retiredOffsets_;
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
