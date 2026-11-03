// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "Audio/Protocols/DICE/Focusrite/SPro40D3Session.hpp"
#include "DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../mocks/MockFireWireBus.hpp"
#include "../support/SPro40D3SimulatedDevice.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <utility>
#include <optional>
#include <vector>

namespace {
using namespace ASFW;
namespace D3 = Audio::DICE::Focusrite::SPro40D3;
namespace Profiles = DeviceProfiles::Audio;
using ::testing::_;
using ::testing::Return;

class SessionClock final : public Audio::DICE::DiceWaitClock {
public:
    void SleepMs(uint32_t ms) noexcept override { now += ms; }
    void Backoff(uint32_t) noexcept override {
        ++now;
        if (auto callback = std::exchange(onBackoff, {})) callback();
    }
    uint64_t NowMs() const noexcept override { return now; }
    uint64_t now{10};
    std::function<void()> onBackoff;
};

class SPro40D3SessionTests : public ::testing::Test {
protected:
    void SetUp() override {
        rom.bib.guid = 0x00130E0404C01234ULL; // TCD3070's GUID product is 0x13, NOT 0xde.
        rom.gen = FW::Generation{19};
        rom.nodeId = 7;
        rom.rootDirMinimal = {
            {.key = Discovery::CfgKey::VendorId, .value = Profiles::kFocusriteVendorId},
            {.key = Discovery::CfgKey::ModelId, .value = Profiles::kSPro40Tcd3070ModelId}};
        Discovery::UnitDirectory unit{};
        unit.offsetQuadlets = 5;
        unit.unitSpecId = Profiles::kFocusriteVendorId;
        unit.unitSwVersion = 1;
        unit.modelId = Profiles::kSPro40Tcd3070ModelId;
        rom.unitDirectories.push_back(unit);
        Discover();
        ON_CALL(bus, GetSpeed(_)).WillByDefault(Return(FW::FwSpeed::S400));
        ON_CALL(bus, ReadBlock(_, _, _, _, _, _))
            .WillByDefault([this](auto gen, auto node, auto address, auto length, auto, auto cb) {
                ++transactions;
                EXPECT_EQ(gen, route.generation);
                EXPECT_EQ(node, FW::NodeId{static_cast<uint8_t>(route.nodeId)});
                const auto reply = sim.ReadBlock(Address(address), length);
                if (Address(address) == D3::kDcpBaseAddress) {
                    ++descriptorReads;
                    if (descriptorReads == 2 && onSecondDescriptor) {
                        auto callback = std::exchange(onSecondDescriptor, {});
                        callback();
                    }
                }
                if (std::exchange(delayDescriptor, false) ||
                    (delayDescriptorNumber != 0 && descriptorReads == delayDescriptorNumber)) {
                    pendingReply = [cb = std::move(cb), reply] {
                        ASSERT_TRUE(reply);
                        cb(Async::AsyncStatus::kSuccess, *reply);
                    };
                    return Async::AsyncHandle{123};
                }
                if (reply) cb(Async::AsyncStatus::kSuccess, *reply);
                else cb(Async::AsyncStatus::kHardwareError, {});
                return Async::AsyncHandle{};
            });
        ON_CALL(bus, WriteBlock(_, _, _, _, _, _))
            .WillByDefault([this](auto, auto, auto address, auto bytes, auto, auto cb) {
                ++transactions;
                const auto reply = sim.WriteBlock(Address(address), bytes);
                if (reply) cb(Async::AsyncStatus::kSuccess, {});
                else cb(Async::AsyncStatus::kHardwareError, {});
                return Async::AsyncHandle{};
            });
        ON_CALL(bus, Lock(_, _, _, _, _, _, _, _))
            .WillByDefault([this](auto, auto, auto address, auto op, auto bytes,
                                  auto responseLength, auto, auto cb) {
                ++transactions;
                EXPECT_EQ(op, FW::LockOp::kCompareSwap);
                EXPECT_EQ(bytes.size(), 16U);
                EXPECT_EQ(responseLength, 8U);
                // spro40d3.rs:259-279: each address/operand is low quadlet
                // first, each quadlet big endian. This exercises the REAL adapter.
                const auto reply = sim.CompareSwap64(Address(address),
                    D3::ReadDcpAddress(bytes.first(8)), D3::ReadDcpAddress(bytes.subspan(8)));
                std::array<uint8_t, 8> raw{};
                if (reply) {
                    FW::WriteBE32(raw.data(), static_cast<uint32_t>(*reply));
                    FW::WriteBE32(raw.data() + 4, static_cast<uint32_t>(*reply >> 32));
                    if (std::exchange(delayCas, false)) {
                        pendingReply = [cb = std::move(cb), raw] {
                            cb(Async::AsyncStatus::kSuccess, raw);
                        };
                        return Async::AsyncHandle{123};
                    }
                    cb(Async::AsyncStatus::kSuccess, raw);
                } else cb(Async::AsyncStatus::kHardwareError, {});
                return Async::AsyncHandle{};
            });
        session = NewSession();
    }
    void Discover() {
        (void)registry.UpsertFromROM(rom, Discovery::LinkPolicy{});
        route = registry.CurrentRoute(rom.bib.guid).value_or(Discovery::DeviceRouteToken{});
    }
    std::unique_ptr<D3::SPro40D3Session> NewSession() {
        return std::make_unique<D3::SPro40D3Session>(bus, bus, registry, clock, hub);
    }
    static uint64_t Address(Async::FWAddress a) {
        return (static_cast<uint64_t>(a.addressHi) << 32) | a.addressLo;
    }
    Async::LocalRequestResult Deliver(std::span<const uint8_t> bytes,
                                     std::optional<uint32_t> generation = {},
                                     std::optional<uint64_t> offset = {}) {
        // The Stage1 simulator exposes the first 12 bytes consumed by the
        // engine. The reference receiver reserves a 16-byte block; pad its
        // unused tail at the integration boundary, not in Stage1.
        std::array<uint8_t, 16> frame{};
        std::copy_n(bytes.begin(), std::min(bytes.size(), frame.size()), frame.begin());
        return hub.HandleLocalRequest({.destOffset = offset.value_or(sim.Owner()),
            .tCode = 1, .sourceID = static_cast<uint16_t>(0xFFC0 | route.nodeId),
            .generation = generation.value_or(route.generation.value),
            .writePayload = frame, .dataLength = 16});
    }
    void Notification(std::span<const uint8_t> frame) {
        savedNotification.assign(frame.begin(), frame.end());
        if (deliverNotifications) (void)Deliver(frame, notificationGeneration);
    }
    void Start() {
        ASSERT_TRUE(session->StartForHostValidation(route));
        ASSERT_TRUE(session->IsReady());
    }
    void DuringStartupWait(bool registration, std::function<void()> callback) {
        delayDescriptor = !registration;
        delayCas = registration;
        clock.onBackoff = [this, callback = std::move(callback)] {
            ASSERT_TRUE(pendingReply);
            callback();
            auto reply = std::exchange(pendingReply, {});
            reply();
        };
    }
    void Rebind(bool clearOwner = true) {
        registry.InvalidateLiveMappingsForBusReset();
        if (clearOwner) sim.BusReset();
        rom.gen = FW::Generation{rom.gen.value + 1};
        rom.nodeId = 9;
        Discover();
    }
    testing::NiceMock<Async::Mocks::MockFireWireBus> bus;
    Discovery::DeviceRegistry registry;
    SessionClock clock;
    D3::SPro40D3NotificationHub hub{registry, clock, {0x000100000000ULL, 0x000100010000ULL}};
    Discovery::ConfigROM rom;
    Discovery::DeviceRouteToken route;
    bool deliverNotifications{true};
    std::optional<uint32_t> notificationGeneration;
    std::vector<uint8_t> savedNotification;
    Testing::DICE::SPro40D3SimulatedDevice sim{[this](auto frame) { Notification(frame); }};
    std::unique_ptr<D3::SPro40D3Session> session;
    unsigned transactions{0};
    bool delayDescriptor{false};
    bool delayCas{false};
    unsigned descriptorReads{0};
    unsigned delayDescriptorNumber{0};
    std::function<void()> onSecondDescriptor;
    std::function<void()> pendingReply;
};

using SessionState = D3::SPro40D3Session::State;
class SPro40D3SessionStartupTests : public SPro40D3SessionTests,
    public ::testing::WithParamInterface<std::pair<bool, SessionState>> {};

TEST_P(SPro40D3SessionStartupTests, LifecycleDuringWaitCannotPublishReadyOrReuseRevokedLease) {
    const auto [registration, terminalState] = GetParam();
    bool interrupted = false;
    DuringStartupWait(registration, [&] {
        interrupted = true;
        EXPECT_TRUE(registry.IsCurrent(route));
        switch (terminalState) {
        case SessionState::Disabled: session->Stop(); break;
        case SessionState::Removed: session->DeviceRemoved(); break;
        case SessionState::NeedsRebind: session->OnBusReset(); break;
        default: FAIL();
        }
        const auto nested = session->StartForHostValidation(route);
        ASSERT_FALSE(nested);
        EXPECT_EQ(nested.error(), D3::MailboxError::kBusy);
    });
    const auto started = session->StartForHostValidation(route);
    ASSERT_FALSE(started);
    EXPECT_TRUE(interrupted);
    EXPECT_EQ(started.error(), terminalState == SessionState::NeedsRebind
        ? D3::MailboxError::kBusReset : D3::MailboxError::kNotRegistered);
    EXPECT_TRUE(registry.IsCurrent(route)); // No transport route failure masks the bug.
    EXPECT_EQ(session->GetState(), terminalState);
    EXPECT_FALSE(session->IsReady());
    EXPECT_EQ(session->ForeignOwner(), 0U);
    EXPECT_EQ(sim.CasCalls(), registration ? 1U : 0U);
    if (registration) {
        const std::array<uint8_t, 16> notification{};
        const auto late = Deliver(notification);
        EXPECT_TRUE(late.claimed);
        EXPECT_EQ(late.rcode, Async::ResponseCode::AddressError);
    }
    const auto before = transactions;
    session->ServiceNotifications();
    EXPECT_FALSE(session->ReadControl32ForValidation(0, [](auto) { FAIL(); }));
    EXPECT_EQ(transactions, before);
    EXPECT_EQ(sim.RequestCount(), 0U);
    EXPECT_EQ(sim.CasCalls(), registration ? 1U : 0U); // No release CAS.
}

TEST_P(SPro40D3SessionStartupTests, LifecycleDuringRegistrationDescriptorPreventsCasToRetiredLease) {
    const auto [nativeCallback, terminalState] = GetParam();
    constexpr uint64_t leaseOffset = 0x000100000000ULL; // First fixture hub lease.
    const std::array<uint8_t, 16> notification{};
    bool interrupted = false;
    auto invalidate = [&] {
        interrupted = true;
        EXPECT_EQ(descriptorReads, 2U);
        EXPECT_TRUE(registry.IsCurrent(route));
        EXPECT_EQ(Deliver(notification, {}, leaseOffset).rcode, Async::ResponseCode::Complete);
        switch (terminalState) {
        case SessionState::Disabled: session->Stop(); break;
        case SessionState::Removed: session->DeviceRemoved(); break;
        case SessionState::NeedsRebind: session->OnBusReset(); break;
        default: FAIL();
        }
        const auto nested = session->StartForHostValidation(route);
        ASSERT_FALSE(nested);
        EXPECT_EQ(nested.error(), D3::MailboxError::kBusy);
        EXPECT_EQ(Deliver(notification, {}, leaseOffset).rcode, Async::ResponseCode::AddressError);
    };
    if (nativeCallback) {
        onSecondDescriptor = invalidate;
    } else {
        delayDescriptorNumber = 2;
        clock.onBackoff = [&] {
            ASSERT_TRUE(pendingReply);
            invalidate();
            auto reply = std::exchange(pendingReply, {});
            reply(); // The active read drains successfully despite invalidation.
        };
    }
    const auto started = session->StartForHostValidation(route);
    ASSERT_FALSE(started);
    EXPECT_TRUE(interrupted);
    EXPECT_EQ(started.error(), terminalState == SessionState::NeedsRebind
        ? D3::MailboxError::kBusReset : D3::MailboxError::kNotRegistered);
    EXPECT_EQ(session->GetState(), terminalState);
    EXPECT_FALSE(session->IsReady());
    EXPECT_TRUE(registry.IsCurrent(route));
    EXPECT_EQ(session->ForeignOwner(), 0U);
    EXPECT_EQ(transactions, 2U); // Only the two descriptor reads, no submitted CAS.
    EXPECT_EQ(sim.CasCalls(), 0U);
    EXPECT_EQ(sim.Owner(), D3::kDcpOwnerNone);
    EXPECT_EQ(Deliver(notification, {}, leaseOffset).rcode, Async::ResponseCode::AddressError);
    session->ServiceNotifications();
    EXPECT_FALSE(session->ReadControl32ForValidation(0, [](auto) { FAIL(); }));
    EXPECT_EQ(transactions, 2U);
}

INSTANTIATE_TEST_SUITE_P(DescriptorAndRegistration, SPro40D3SessionStartupTests,
    ::testing::Values(std::pair{false, SessionState::Disabled},
                      std::pair{false, SessionState::Removed},
                      std::pair{false, SessionState::NeedsRebind},
                      std::pair{true, SessionState::Disabled},
                      std::pair{true, SessionState::Removed},
                      std::pair{true, SessionState::NeedsRebind}));

TEST_F(SPro40D3SessionTests, NestedStartDuringDescriptorWaitIsBusyAndOuterStartSucceeds) {
    DuringStartupWait(false, [&] {
        const auto nested = session->StartForHostValidation(route);
        ASSERT_FALSE(nested);
        EXPECT_EQ(nested.error(), D3::MailboxError::kBusy);
    });
    Start();
    EXPECT_EQ(sim.CasCalls(), 1U);
    EXPECT_FALSE(pendingReply);
    ASSERT_TRUE(session->StartForHostValidation(route)); // Guard unwound.
}

TEST_F(SPro40D3SessionTests, NestedStartFromNativeDescriptorCallbackIsBusy) {
    EXPECT_CALL(bus, ReadBlock(_, _, _, _, _, _)).Times(::testing::AnyNumber());
    EXPECT_CALL(bus, ReadBlock(_, _, _, _, _, _))
        .WillOnce([&](auto, auto, auto address, auto length, auto, auto cb) {
            ++transactions;
            const auto nested = session->StartForHostValidation(route);
            EXPECT_FALSE(nested);
            if (!nested) EXPECT_EQ(nested.error(), D3::MailboxError::kBusy);
            const auto reply = sim.ReadBlock(Address(address), length);
            EXPECT_TRUE(reply);
            if (reply) cb(Async::AsyncStatus::kSuccess, *reply);
            return Async::AsyncHandle{};
        }).RetiresOnSaturation();
    Start();
    EXPECT_EQ(sim.CasCalls(), 1U);
}

TEST_F(SPro40D3SessionTests, ConstructionAndProviderDoNotOptInOrPublishCapabilities) {
    EXPECT_EQ(transactions, 0U);
    EXPECT_EQ(session->GetState(), D3::SPro40D3Session::State::Disabled);
    EXPECT_FALSE(session->CopySnapshot());
    int completions = 0;
    session->Prepare([&](auto result) { EXPECT_EQ(result, kIOReturnNotReady); ++completions; });
    session->SetParameter({}, false, [&](auto result, auto) {
        EXPECT_EQ(result, kIOReturnUnsupported); ++completions;
    });
    session->SetRoute({}, {}, {}, [&](auto result, auto) {
        EXPECT_EQ(result, kIOReturnUnsupported); ++completions;
    });
    EXPECT_FALSE(session->ReadControl32ForValidation(0, [](auto) { FAIL(); }));
    session->ServiceNotifications();
    EXPECT_EQ(completions, 3);
    EXPECT_EQ(transactions, 0U);
}

TEST_F(SPro40D3SessionTests, ReadUsesBusAdapterAndQueuedHubNotEngineDuringRequestWrite) {
    Start();
    EXPECT_EQ(sim.CasCalls(), 1U);
    bool completed = false;
    ASSERT_TRUE(session->ReadControl32ForValidation(0, [&](auto result) {
        ASSERT_TRUE(result); EXPECT_EQ(*result, 0U); completed = true;
    }));
    EXPECT_FALSE(completed);
    EXPECT_EQ(sim.ResponseAreaReads(), 0U);
    session->ServiceNotifications();
    EXPECT_TRUE(completed);
    EXPECT_EQ(sim.ResponseAreaReads(), 1U);
    EXPECT_EQ(sim.UnpairedResponseReads(), 0U);
    EXPECT_FALSE(sim.RequestMutatedDuringNotify());
    EXPECT_FALSE(session->CopySnapshot());
    session->Prepare([](auto result) { EXPECT_EQ(result, kIOReturnUnsupported); });
}

TEST_F(SPro40D3SessionTests, CatalogExceptionAdmitsGuidProduct13WithAbsentRootFields) {
    rom.rootDirMinimal.clear();
    Discover();
    const auto record = registry.SnapshotByGuid(route.guid);
    ASSERT_TRUE(record);
    ASSERT_FALSE(record->identity.rootVendorId);
    ASSERT_FALSE(record->identity.rootModelId);
    const auto* policy = Profiles::CurrentAudioPolicy(*record);
    ASSERT_NE(policy, nullptr);
    ASSERT_EQ(policy->plan.exactVariantId,
              static_cast<uint32_t>(Profiles::DeviceDefinitionId::FocusriteSPro40Tcd3070));
    Start();
}

TEST_F(SPro40D3SessionTests, NonTcdIdentitiesNeverTouchMailbox) {
    struct Case { uint32_t model; uint32_t vendor; uint32_t version; uint64_t guid; };
    const std::array cases{
        Case{Profiles::kSPro40ModelId, Profiles::kFocusriteVendorId, 1, 0x00130E0504001234ULL},
        Case{0x123456, Profiles::kFocusriteVendorId, 1, 0x00130E5504001234ULL},
        Case{Profiles::kSPro40Tcd3070ModelId, 0x123456, 1, 0x1234561304001234ULL},
        Case{Profiles::kSPro40Tcd3070ModelId, Profiles::kFocusriteVendorId, 2, 0x00130E0404C01234ULL}};
    for (const auto& c : cases) {
        session = NewSession();
        registry.Clear();
        rom.bib.guid = c.guid;
        rom.rootDirMinimal[0].value = c.vendor;
        rom.rootDirMinimal[1].value = c.model;
        rom.unitDirectories[0].unitSpecId = c.vendor;
        rom.unitDirectories[0].modelId = c.model;
        rom.unitDirectories[0].unitSwVersion = c.version;
        Discover();
        EXPECT_FALSE(session->StartForHostValidation(route));
        EXPECT_EQ(transactions, 0U);
    }
}

TEST_F(SPro40D3SessionTests, AmbiguousCatalogDecisionNeverTouchesMailbox) {
    rom.bib.guid = 0x0001F20000001234ULL;
    rom.rootDirMinimal[0].value = Profiles::kMotuVendorId;
    rom.rootDirMinimal[1].value = 0;
    rom.unitDirectories[0].unitSpecId = Profiles::kMotuVendorId;
    rom.unitDirectories[0].unitSwVersion = Profiles::kMotu828mk2SwVersion;
    auto sibling = rom.unitDirectories[0];
    sibling.offsetQuadlets = 9;
    sibling.unitSwVersion = Profiles::kMotuUltraliteSwVersion;
    rom.unitDirectories.push_back(sibling);
    Discover();
    const auto record = registry.SnapshotByGuid(rom.bib.guid);
    ASSERT_TRUE(record);
    ASSERT_EQ(record->quarantineReason, Discovery::QuarantineReason::AmbiguousIdentity);
    EXPECT_FALSE(session->StartForHostValidation(route));
    EXPECT_EQ(transactions, 0U);
}

TEST_F(SPro40D3SessionTests, EveryStage0DescriptorFieldIsStrictBeforeCas) {
    for (const size_t offset : {0U, 0x14U, 0x18U, 0x1CU, 0x20U, 0x28U}) {
        auto bytes = Testing::DICE::kSPro40D3DescriptorBytes;
        bytes[offset] ^= 1;
        sim.SetDescriptorOverride(bytes);
        session = NewSession();
        const auto started = session->StartForHostValidation(route);
        ASSERT_FALSE(started);
        EXPECT_EQ(started.error(), D3::MailboxError::kBadDescriptor);
        EXPECT_EQ(sim.CasCalls(), 0U);
        EXPECT_EQ(sim.RequestCount(), 0U);
    }
}

TEST_F(SPro40D3SessionTests, ForeignOwnerDoesNotCasAndFailureIsLatched) {
    const uint64_t foreign = 0xFFC1000100000040ULL;
    sim.ForceOwner(foreign);
    const auto started = session->StartForHostValidation(route);
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error(), D3::MailboxError::kOwnershipConflict);
    EXPECT_EQ(session->ForeignOwner(), foreign);
    EXPECT_EQ(sim.CasCalls(), 0U);
    EXPECT_EQ(sim.Owner(), foreign);
    const auto before = transactions;
    EXPECT_FALSE(session->StartForHostValidation(route));
    EXPECT_EQ(transactions, before);
}

TEST_F(SPro40D3SessionTests, RacingCasConflictNeverRetriesOrReleasesForeignOwner) {
    const uint64_t foreign = 0xFFC2000100000080ULL;
    sim.ArmOwnerRace(foreign);
    EXPECT_FALSE(session->StartForHostValidation(route));
    EXPECT_EQ(sim.CasCalls(), 1U);
    EXPECT_EQ(session->ForeignOwner(), foreign);
    session->Stop();
    EXPECT_EQ(session->ForeignOwner(), 0U);
    EXPECT_EQ(sim.CasCalls(), 1U);
    EXPECT_EQ(sim.Owner(), foreign);
}

TEST_F(SPro40D3SessionTests, DuplicateStartRevalidatesSameEngineAndOwnershipLossClosesAdmission) {
    Start();
    ASSERT_TRUE(session->StartForHostValidation(route));
    EXPECT_EQ(sim.CasCalls(), 1U);
    const auto oldOwner = sim.Owner();
    sim.ForceOwner(D3::kDcpOwnerNone);
    EXPECT_FALSE(session->StartForHostValidation(route));
    EXPECT_FALSE(session->IsReady());
    EXPECT_EQ(session->ForeignOwner(), D3::kDcpOwnerNone);
    const std::array<uint8_t, 16> notification{};
    const auto late = Deliver(notification, {}, oldOwner);
    EXPECT_TRUE(late.claimed);
    EXPECT_EQ(late.rcode, Async::ResponseCode::AddressError);
    EXPECT_EQ(sim.CasCalls(), 1U);
    EXPECT_FALSE(session->ReadControl32ForValidation(0, [](auto) { FAIL(); }));
    EXPECT_EQ(sim.RequestCount(), 0U);
}

TEST_F(SPro40D3SessionTests, QueuedTimelyNotificationCannotBeatCurrentTimeout) {
    Start();
    unsigned completed = 0;
    ASSERT_TRUE(session->ReadControl32ForValidation(0, [&](auto result) {
        ASSERT_FALSE(result); EXPECT_EQ(result.error(), D3::MailboxError::kTimeout); ++completed;
    }));
    clock.now += 201;
    session->ServiceNotifications();
    session->ServiceNotifications();
    EXPECT_EQ(completed, 1U);
    EXPECT_EQ(sim.ResponseAreaReads(), 0U);
}

TEST_F(SPro40D3SessionTests, LateIngressAndWrongGenerationCannotReadResponse) {
    Start();
    deliverNotifications = false;
    unsigned completed = 0;
    ASSERT_TRUE(session->ReadControl32ForValidation(0, [&](auto result) {
        ASSERT_FALSE(result); EXPECT_EQ(result.error(), D3::MailboxError::kTimeout); ++completed;
    }));
    const auto wrong = Deliver(savedNotification, route.generation.value + 1);
    EXPECT_NE(wrong.rcode, Async::ResponseCode::Complete);
    session->ServiceNotifications();
    EXPECT_EQ(completed, 0U);
    clock.now += 200;
    (void)Deliver(savedNotification);
    session->ServiceNotifications();
    EXPECT_EQ(completed, 1U);
    EXPECT_EQ(sim.ResponseAreaReads(), 0U);
}

TEST_F(SPro40D3SessionTests, ResetCompletesOnceClosesAdmissionBeforeCallbackAndDoesNoIo) {
    Start();
    unsigned completed = 0;
    ASSERT_TRUE(session->ReadControl32ForValidation(0, [&](auto result) {
        ASSERT_FALSE(result); EXPECT_EQ(result.error(), D3::MailboxError::kBusReset); ++completed;
        EXPECT_FALSE(session->IsReady());
        EXPECT_FALSE(session->ReadControl32ForValidation(0, [](auto) { FAIL(); }));
        EXPECT_FALSE(session->StartForHostValidation(route));
        session->Stop();
        session->ServiceNotifications();
    }));
    const auto before = transactions;
    session->OnBusReset();
    session->OnBusReset();
    session->ServiceNotifications();
    EXPECT_EQ(transactions, before);
    EXPECT_EQ(completed, 1U);
    EXPECT_EQ(sim.ResponseAreaReads(), 0U);
    EXPECT_EQ(sim.CasCalls(), 1U);
}

TEST_F(SPro40D3SessionTests, ResetNeedsExplicitFreshRouteAndFreshUnownedClaim) {
    Start();
    const auto old = route;
    session->OnBusReset();
    EXPECT_FALSE(session->StartForHostValidation(old));
    Rebind();
    const auto before = transactions;
    session->ServiceNotifications();
    EXPECT_EQ(transactions, before);
    ASSERT_TRUE(session->StartForHostValidation(route));
    EXPECT_NE(route, old);
    EXPECT_EQ(sim.CasCalls(), 2U);
    ASSERT_TRUE(session->ReadControl32ForValidation(0, [](auto result) { EXPECT_TRUE(result); }));
    session->ServiceNotifications();
    EXPECT_EQ(sim.ResponseAreaReads(), 1U);
}

TEST_F(SPro40D3SessionTests, ResetWithRetainedOwnerRefusesNewClaim) {
    Start();
    const auto owner = sim.Owner();
    session->OnBusReset();
    Rebind(false);
    EXPECT_FALSE(session->StartForHostValidation(route));
    EXPECT_EQ(session->ForeignOwner(), owner);
    EXPECT_EQ(sim.Owner(), owner);
    EXPECT_EQ(sim.CasCalls(), 1U);
}

TEST_F(SPro40D3SessionTests, StaleRegistryRouteFlushesQueuedWorkAndCancelsOnlyOnce) {
    Start();
    unsigned completed = 0;
    ASSERT_TRUE(session->ReadControl32ForValidation(0, [&](auto result) {
        ASSERT_FALSE(result); EXPECT_EQ(result.error(), D3::MailboxError::kBusReset); ++completed;
    }));
    registry.InvalidateLiveMappingsForBusReset();
    const auto before = transactions;
    session->ServiceNotifications();
    session->ServiceNotifications();
    EXPECT_EQ(completed, 1U);
    EXPECT_EQ(transactions, before);
    EXPECT_FALSE(session->IsReady());
}

TEST_F(SPro40D3SessionTests, CompletionCanStopResetAndAttemptRestartWithoutDestroyingActiveEngine) {
    Start();
    unsigned completed = 0;
    ASSERT_TRUE(session->ReadControl32ForValidation(0, [&](auto result) {
        ASSERT_TRUE(result); ++completed;
        session->Stop();
        session->OnBusReset();
        EXPECT_FALSE(session->StartForHostValidation(route));
        EXPECT_FALSE(session->ReadControl32ForValidation(0, [](auto) { FAIL(); }));
        session->ServiceNotifications();
    }));
    session->ServiceNotifications();
    EXPECT_EQ(completed, 1U);
    EXPECT_FALSE(session->IsReady());
    EXPECT_EQ(sim.CasCalls(), 1U);
}

TEST_F(SPro40D3SessionTests, StopRetiresQueueAndLateNotificationWithoutOwnerRelease) {
    Start();
    unsigned completed = 0;
    ASSERT_TRUE(session->ReadControl32ForValidation(0, [&](auto result) {
        ASSERT_FALSE(result); EXPECT_EQ(result.error(), D3::MailboxError::kBusReset); ++completed;
    }));
    const auto owner = sim.Owner();
    const auto before = transactions;
    session->Stop();
    const auto late = Deliver(savedNotification, {}, owner);
    EXPECT_TRUE(late.claimed); // Hub reserves its aperture even for retired addresses.
    EXPECT_EQ(late.rcode, Async::ResponseCode::AddressError);
    session->ServiceNotifications();
    EXPECT_EQ(completed, 1U);
    EXPECT_EQ(sim.Owner(), owner);
    EXPECT_EQ(transactions, before);
    EXPECT_EQ(sim.CasCalls(), 1U);
    // A new engine must not infer self ownership from a retained address.
    EXPECT_FALSE(session->StartForHostValidation(route));
    EXPECT_EQ(session->ForeignOwner(), owner);
    EXPECT_EQ(sim.CasCalls(), 1U);
}

TEST_F(SPro40D3SessionTests, RemovalCancelsPendingCommandBeforeReentrantCallsAndWithoutIo) {
    Start();
    unsigned completed = 0;
    ASSERT_TRUE(session->ReadControl32ForValidation(0, [&](auto result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), D3::MailboxError::kBusReset);
        ++completed;
        EXPECT_EQ(session->GetState(), D3::SPro40D3Session::State::Removed);
        EXPECT_FALSE(session->ReadControl32ForValidation(0, [](auto) { FAIL(); }));
        EXPECT_FALSE(session->StartForHostValidation(route));
        session->Stop();
    }));
    const auto before = transactions;
    session->DeviceRemoved();
    session->DeviceRemoved();
    session->ServiceNotifications();
    EXPECT_EQ(completed, 1U);
    EXPECT_EQ(transactions, before);
    EXPECT_EQ(sim.ResponseAreaReads(), 0U);
    EXPECT_EQ(sim.CasCalls(), 1U);
    EXPECT_EQ(session->GetState(), D3::SPro40D3Session::State::Removed);
}

TEST_F(SPro40D3SessionTests, DestructionCancelsPendingCommandAndLateIngressHasNoSessionTarget) {
    Start();
    unsigned completed = 0;
    ASSERT_TRUE(session->ReadControl32ForValidation(0, [&](auto result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), D3::MailboxError::kBusReset);
        ++completed;
    }));
    const auto oldOwner = sim.Owner();
    const auto before = transactions;
    session.reset();
    EXPECT_EQ(completed, 1U);
    const auto late = Deliver(savedNotification, {}, oldOwner);
    EXPECT_EQ(late.rcode, Async::ResponseCode::AddressError);
    EXPECT_EQ(transactions, before);
    EXPECT_EQ(sim.ResponseAreaReads(), 0U);
    EXPECT_EQ(sim.CasCalls(), 1U);
}

TEST_F(SPro40D3SessionTests, RemovalIsTerminalAndReplugRequiresNewSession) {
    Start();
    const auto oldRoute = route;
    session->DeviceRemoved();
    registry.RetireDevice(route.guid);
    sim.BusReset();
    Discover();
    EXPECT_FALSE(registry.IsCurrent(oldRoute));
    EXPECT_NE(route.deviceIncarnation, oldRoute.deviceIncarnation);
    const auto before = transactions;
    EXPECT_FALSE(session->StartForHostValidation(route));
    session->Stop();
    EXPECT_EQ(session->GetState(), D3::SPro40D3Session::State::Removed);
    EXPECT_EQ(transactions, before);
    session = NewSession();
    Start();
}

TEST_F(SPro40D3SessionTests, RebindCancellationCannotOverrideReentrantRemoval) {
    Start();
    unsigned completed = 0;
    ASSERT_TRUE(session->ReadControl32ForValidation(0, [&](auto result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), D3::MailboxError::kBusReset);
        ++completed;
        session->DeviceRemoved();
    }));
    Rebind();
    const auto before = transactions;
    EXPECT_FALSE(session->StartForHostValidation(route));
    EXPECT_EQ(completed, 1U);
    EXPECT_EQ(session->GetState(), D3::SPro40D3Session::State::Removed);
    EXPECT_EQ(transactions, before);
    EXPECT_EQ(sim.CasCalls(), 1U);
}

TEST_F(SPro40D3SessionTests, ReplugCannotReuseOldSessionEvenWithoutRemovalCallback) {
    Start();
    registry.RetireDevice(route.guid);
    sim.BusReset();
    Discover();
    session->ServiceNotifications();
    EXPECT_FALSE(session->StartForHostValidation(route));
    EXPECT_EQ(sim.CasCalls(), 1U);
}

TEST(SPro40D3SessionBoundary, UnknownProvenanceIsAbsentNotObserved) {
    EXPECT_FALSE(D3::SPro40D3Session::ToStateOrigin(D3::ValueProvenance::Unknown));
    EXPECT_EQ(D3::SPro40D3Session::ToStateOrigin(D3::ValueProvenance::Observed),
              AudioModel::StateOrigin::Observed);
    EXPECT_EQ(D3::SPro40D3Session::ToStateOrigin(D3::ValueProvenance::Cached),
              AudioModel::StateOrigin::Cached);
}
} // namespace
