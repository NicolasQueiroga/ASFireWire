// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "SPro40D3NotificationHub.hpp"
#include "../../../../Protocols/SBP2/AddressSpaceManager.hpp"

#include <DriverKit/IOLib.h>

#include <expected>
#include <memory>
#include <optional>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

/// Owns one manager-reserved SPro40D3 notification address. The address range
/// is deliberately never deallocated: a late device write must not reach a
/// future owner. Create it as shared ownership so the manager callback only
/// observes a weak reference after teardown.
class SPro40D3NotificationEndpoint final {
public:
    static constexpr uint32_t kAddressBytes = SPro40D3NotificationHub::kFrameBytes;

    [[nodiscard]] static std::expected<std::shared_ptr<SPro40D3NotificationEndpoint>, IOReturn>
    Create(Protocols::SBP2::AddressSpaceManager& manager,
           std::shared_ptr<SPro40D3NotificationHub> hub);

    ~SPro40D3NotificationEndpoint();
    SPro40D3NotificationEndpoint(const SPro40D3NotificationEndpoint&) = delete;
    SPro40D3NotificationEndpoint& operator=(const SPro40D3NotificationEndpoint&) = delete;

    [[nodiscard]] std::expected<SPro40D3NotificationHub::Lease, IOReturn>
    Bind(Discovery::DeviceRouteToken route);
    void Release() noexcept;

    [[nodiscard]] uint64_t address() const noexcept { return address_; }

private:
    explicit SPro40D3NotificationEndpoint(std::shared_ptr<SPro40D3NotificationHub> hub) noexcept;
    [[nodiscard]] Async::ResponseCode
    HandleWrite(const Protocols::SBP2::AddressSpaceManager::RemoteWriteContext& context) noexcept;

    std::shared_ptr<SPro40D3NotificationHub> hub_;
    uint64_t handle_{0};
    uint64_t address_{0};
    IOLock* lock_{nullptr};
    std::optional<SPro40D3NotificationHub::Lease> lease_;
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
