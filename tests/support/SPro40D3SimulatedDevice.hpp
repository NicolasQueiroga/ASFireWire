// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3SimulatedDevice.hpp - Deterministic host-side TCD3070 (Saffire Pro
// 40 rev 2) DCP mailbox for mailbox-engine tests.
//
// Follows the SimulatedDiceDevice.hpp discipline: behaviour is modelled only
// as far as the evidence goes, and each modelled behaviour cites it. Fault
// knobs let tests script every failure the engine must survive.
//
// Evidence base:
//   - The 48-byte descriptor is the REAL hardware capture
//     (documentation/evidence/stage0-tcd3070/mailbox-descriptor-raw.txt,
//     2026-11-03): magic 0x1810196604061973, owner none, version 2, max
//     request 1024, response area 400, write +0x1C0 / read +0x30.
//   - Command semantics mirror the ALSA reference behavioral facts
//     (spro40d3.rs, cited per knob); meter VALUE ENCODING is unknown on
//     hardware, so meters return synthetic documented values.
//   - tools/firmware_analysis/Pro40d3-DCP.md: unknown command categories
//     answer status 4; the transport FSM re-arms per request; the DCP FSM
//     answers the notification BEFORE the write transaction completes
//     (0x1B688) - the default order below.
//
// This device knows nothing about the engine: it implements the asynchronous
// ISPro40D3Transport seam (acceptance + exactly-once completion, inline or
// deferred) and pushes notifications through a NotifySink the test wires to
// SPro40D3MailboxEngine::OnNotification.
//
// P02 knobs model the ACK/notification/response-read ORDERING space: the
// default fires the completion INLINE (inside the call) AFTER any inline
// notification - the honest firmware order. Deferred modes park completions
// in a FIFO the test drains deterministically; no wall clock exists here.
//
// REQUEST-LIFETIME TRIPWIRE (Stage 1 review, reentrant-transport finding):
// HandleRequest takes the WriteBlock span WITHOUT copying it and TOUCHES it
// again AFTER notifySink_ returns (post-notify counter/opcode re-read). The
// engine must keep the span valid for the entire synchronous call; inline
// completions and notifications only queue work into the pump, which runs
// after the transport call returns. If an engine ever frees or moves the
// request buffer while the transport call is executing, the post-notify
// touch reads freed/poisoned memory and ASan flags it. The re-read also
// detects in-call mutation (requestMutatedDuringNotify()).

#pragma once

#include "Audio/Protocols/DICE/Focusrite/SPro40D3MailboxEngine.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace ASFW::Testing::DICE {

/// The live 48-byte descriptor, verbatim (Stage 0 capture, 2026-11-03).
inline constexpr std::array<uint8_t, 48> kSPro40D3DescriptorBytes{
    // Verbatim capture; 64-bit fields store the LOW quadlet first
    // (spro40d3.rs deserialize_address; Stage 0 capture 2026-11-03).
    0x04, 0x06, 0x19, 0x73, 0x18, 0x10, 0x19, 0x66, // +0x00 magic
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, // +0x08 owner: none
    0x00, 0x00, 0x00, 0x02,                          // +0x10 (2, purpose unknown)
    0x00, 0x00, 0x00, 0x02,                          // +0x14 version 2
    0x00, 0x00, 0x04, 0x00,                          // +0x18 max request 1024
    0x00, 0x00, 0x01, 0x90,                          // +0x1C response area 400
    0xE0, 0x40, 0x01, 0xC0, 0x00, 0x00, 0xFF, 0xFF,  // +0x20 write address
    0xE0, 0x40, 0x00, 0x30, 0x00, 0x00, 0xFF, 0xFF,  // +0x28 read address
};

namespace D3 = ::ASFW::Audio::DICE::Focusrite::SPro40D3;

class SPro40D3SimulatedDevice final : public D3::ISPro40D3Transport {
public:
    using NotifySink = std::function<void(std::span<const uint8_t> frame)>;
    using Deferred = ASFW::Common::MoveOnlyCallback<void()>;

    struct Options {
        /// Deliver the response notification inside the request WriteBlock
        /// (what the real firmware does: the DCP FSM answers before the write
        /// transaction completes - Pro40d3-DCP.md transport FSM 0x1B688).
        bool notifyDuringWrite{true};
        /// Nonzero rstatus for the next N command responses, then back to 0.
        /// Models the transient errors that make the reference run its
        /// counter-reset ritual (spro40d3.rs send_command rstatus path).
        uint32_t failNextResponses{0};
        uint32_t forcedResponseStatus{4}; // status 4 = unknown category (firmware)
        /// Drop notifications for the next N commands (timeout path).
        uint32_t dropNextNotifications{0};
        /// Echo this offset added to the request counter in the response
        /// header (counter desync -> kCounterMismatch). Use an offset that
        /// cannot collide with the recent-counter window (>= 5).
        uint16_t counterEchoOffset{0};
        /// Serve these 48 bytes instead of the Stage 0 capture on descriptor
        /// reads (models a different unit / firmware revision after a bus
        /// reset -> kBadDescriptor on revalidation).
        std::optional<std::array<uint8_t, 48>> descriptorOverride{};
        /// REJECT (return false; nothing lands; not counted as a request)
        /// the Nth command write AT ISSUE (1-based; ritual reset frames
        /// excluded). Non-submission is established at the seam.
        uint32_t rejectWriteAtRequestIndex{0};
        /// The device APPLIES the Nth command write (1-based; ritual reset
        /// frames excluded), then that write's completion carries
        /// writeFailureStatus anyway (delivered-then-lost ACK; ambiguous
        /// write - P01). Ordinal-based so chained-step sequences can target
        /// one step deterministically; ArmFailWriteCompletions is the
        /// count-based mid-scenario variant.
        uint32_t failWriteCompletionAtRequestIndex{0};
        /// The next N command writes are applied and then fail their
        /// completion with writeFailureStatus (count-based, armable
        /// mid-scenario).
        uint32_t failNextWriteCompletions{0};
        /// Drop the notification with this 1-based ABSOLUTE ordinal (every
        /// notification the device has ever sent counts, delivered or
        /// dropped) - deterministic targeting inside multi-step sequences
        /// (the low table's response delivers; the high's is dropped).
        uint32_t dropNotificationAtAbsoluteIndex{0};
        /// Park the next N WRITE completions (accepted + applied) in the
        /// deferred FIFO instead of firing inline (deterministic ACK
        /// ordering: notification can run before the ACK is drained).
        uint32_t deferNextWriteCompletions{0};
        /// Park the next N READ completions in the deferred FIFO.
        uint32_t deferNextReads{0};
        /// Park the next N CAS completions in the deferred FIFO.
        uint32_t deferNextCas{0};
        /// IOReturn carried by deferred/rejected-write completions above.
        IOReturn writeFailureStatus{kIOReturnTimeout};
        // -- one-shot response-shape knobs (Stage 1 review) ----------------
        /// One-shot: the NEXT notification carries this rlen instead of the
        /// real response payload length (oversized / wrong-shape ingress).
        uint16_t nextNotificationLengthOverride{0};
        /// Offset added to the response header's payloadLength ECHO only
        /// (actual payload untouched) -> kMalformedResponse.
        int16_t responseLengthEchoOffset{0};
        /// One-shot: corrupt the response header's opcode echo (byte 1
        /// flipped) -> kMalformedResponse.
        bool responseOpcodeCorrupt{false};
        /// One-shot: a concurrent host's ownership CAS lands between our
        /// descriptor read and our own CAS. The next CompareSwap64 observes
        /// this value as the current owner (their claim committed; ours
        /// fails atomically) - models the register race window.
        uint64_t ownerRaceValue{0};
    };

    explicit SPro40D3SimulatedDevice(NotifySink sink, Options options)
        : notifySink_{std::move(sink)}, options_{options} {}
    explicit SPro40D3SimulatedDevice(NotifySink sink); // defined below the class

    // -- ISPro40D3Transport (async acceptance + exactly-once completion) ----

    [[nodiscard]] bool WriteBlock(uint64_t address, std::span<const uint8_t> bytes,
                                  WriteDone done) override {
        if (address == D3::kDcpBaseAddress + 8 || address != writeAddress() ||
            bytes.size() > kDcpMaxRequest || bytes.size() < D3::kMessageHeaderSize) {
            // Malformed issue: rejected, nothing submitted.
            return false;
        }
        if (bytes[0] == 0x80 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0 &&
            bytes.size() == D3::kCounterResetFrameSize) {
            // Counter-reset frame (live-verified 2026-11-06): a FULL mailbox
            // exchange like every other send_message - the response area
            // echoes the 16-byte reset frame and the device notifies its own
            // 0x80000000 ack (rstatus 0). The host must consume both before
            // the retry: an unconsumed ritual answer leaves the live
            // device's mailbox stuck and the retry is dropped. The delivery
            // knobs below still script notification loss for the ritual.
            sawResetFrame_ = true;
            std::fill(responseArea_.begin(), responseArea_.end(), 0);
            responseArea_[0] = 0x80;
            responseReady_ = true;
            if (options_.dropNextNotifications > 0) {
                --options_.dropNextNotifications;
                return CompleteWrite(std::move(done), /*requestIndexConsumed=*/false);
            }
            ++notifIndex_;
            if (options_.dropNotificationAtAbsoluteIndex != 0 &&
                notifIndex_ == options_.dropNotificationAtAbsoluteIndex) {
                return CompleteWrite(std::move(done), /*requestIndexConsumed=*/false);
            }
            std::array<uint8_t, 12> notif{};
            notif[0] = 0x80; // opcode echo of the reset frame; counter/rlen/rstatus 0
            if (options_.notifyDuringWrite) {
                notifySink_(notif);
            } else {
                pendingNotifications_.push_back(notif);
            }
            return CompleteWrite(std::move(done), /*requestIndexConsumed=*/false);
        }
        if (options_.rejectWriteAtRequestIndex != 0 &&
            requestIndex_ + 1 == options_.rejectWriteAtRequestIndex) {
            // Scripted rejection at issue: the request never lands, the
            // completion never fires, and the request ordinal is NOT
            // consumed (rejected writes are not requests).
            return false;
        }
        ++requestIndex_;
        HandleRequest(bytes);
        if (options_.failNextWriteCompletions > 0) {
            --options_.failNextWriteCompletions;
            // Applied, but the ACK is lost: the completion carries an error
            // (ambiguous write - P01).
            done(std::unexpected(options_.writeFailureStatus));
            return true;
        }
        if (options_.failWriteCompletionAtRequestIndex != 0 &&
            requestIndex_ == options_.failWriteCompletionAtRequestIndex) {
            done(std::unexpected(options_.writeFailureStatus));
            return true;
        }
        return CompleteWrite(std::move(done), /*requestIndexConsumed=*/true);
    }

    [[nodiscard]] bool ReadBlock(uint64_t address, uint32_t length, ReadDone done) override {
        ++readBlockCalls_;
        if (address == D3::kDcpBaseAddress) {
            if (length != D3::kDcpDescriptorSize) return false;
            // Honest descriptor: the owner field (+0x08) always reports the
            // CURRENT owner storage, never a static snapshot (Stage-1 review
            // round 3, finding A: a descriptor read decoupled from the CAS's
            // view of the owner hid the takeover hazard from the engine).
            std::array<uint8_t, 48> bytes = options_.descriptorOverride
                                                ? *options_.descriptorOverride
                                                : kSPro40D3DescriptorBytes;
            // 64-bit fields use "quadlet 0 low" word order (Stage 0 capture:
            // owner none = 00 00 00 00 FF FF 00 00 = 0xFFFF000000000000).
            ::ASFW::FW::WriteBE32(bytes.data() + 8, static_cast<uint32_t>(owner_));
            ::ASFW::FW::WriteBE32(bytes.data() + 12, static_cast<uint32_t>(owner_ >> 32));
            std::vector<uint8_t> out(bytes.begin(), bytes.end());
            return CompleteRead(std::move(out), std::move(done));
        }
        if (address == readAddress()) {
            if (!responseReady_) {
                ++unpairedReads_;
                // Completion error: the engine treats an unpaired response
                // read as a failed confirmation.
                done(std::unexpected(kIOReturnNotReady));
                return true;
            }
            responseReady_ = false; // one paired read per response (Stage 0 §7a)
            ++responseAreaReads_;
            std::vector<uint8_t> out(responseArea_.begin(),
                                      responseArea_.begin() + length);
            return CompleteRead(std::move(out), std::move(done));
        }
        return false;
    }

    [[nodiscard]] bool CompareSwap64(uint64_t address, uint64_t expected,
                                     uint64_t desired, CasDone done) override {
        if (address != D3::kDcpBaseAddress + 8) return false;
        ++casCalls_;
        // Race model (one-shot): a concurrent claim commits between the
        // engine's descriptor read and its CAS. Their claim wins; ours
        // fails atomically against the observed value.
        if (options_.ownerRaceValue != 0) {
            owner_ = options_.ownerRaceValue;
            options_.ownerRaceValue = 0;
            return CompleteCas(owner_, std::move(done)); // expected (none) mismatch
        }
        const uint64_t prev = owner_;
        if (prev == expected) owner_ = desired;
        return CompleteCas(prev, std::move(done));
    }

    // -- Deferred completions (deterministic ACK/read/CAS ordering) --------

    /// Fires parked completions FIFO. Tests call this to script ordering;
    /// notifications parked by notifyDuringWrite=false are separate
    /// (DeliverPendingNotifications).
    void DrainDeferredCompletions() {
        while (!deferred_.empty()) {
            Deferred fire = std::move(deferred_.front());
            deferred_.pop_front();
            fire();
        }
    }
    [[nodiscard]] bool HasDeferredCompletions() const noexcept { return !deferred_.empty(); }

    // -- Device behaviour ---------------------------------------------------

    /// Bus reset: the transport re-arms and in-flight answer state drops.
    /// The owner register is RETAINED (live hardware 2026-11-06: the D3's
    /// owner field is flash-backed - it survived a real power cycle,
    /// generation 7->9 with the prev-owner intact; a bus reset is weaker
    /// than a power cycle and the field survives it the same way. TCAT's
    /// per-generation re-claim is defensive client behavior, not evidence
    /// of clearing). Model a cleared field explicitly with
    /// ForceOwner(kDcpOwnerNone) - a foreign host's release, or a
    /// hypothetically dropping firmware.
    void BusReset() {
        responseReady_ = false;
        pendingNotifications_.clear();
    }

    /// Test-only: (re)arm the drop-next-N-notifications knob mid-scenario.
    void ArmDropNotifications(uint32_t n) { options_.dropNextNotifications = n; }

    /// Test-only: (re)arm the fail-next-N-responses knob mid-scenario.
    void ArmFailResponses(uint32_t n) { options_.failNextResponses = n; }

    /// Test-only: model the front panel (or any foreign writer) changing
    /// the control store behind the host's back - a knob turn, a panel
    /// button. Same store routing as the set_control handler; no command,
    /// no notification: the host only learns by reading (the poll).
    void SimulateForeignStoreWrite(uint32_t position, uint32_t value) {
        controlStore_[position] = value;
    }

    /// Test-only: (re)arm knobs that were constructor-only in Stage 1.
    void ArmRejectWriteAtRequestIndex(uint32_t n) { options_.rejectWriteAtRequestIndex = n; }
    void ArmFailWriteCompletions(uint32_t n) { options_.failNextWriteCompletions = n; }
    void SetFailWriteCompletionAtRequestIndex(uint32_t n) {
        options_.failWriteCompletionAtRequestIndex = n;
    }
    void SetDropNotificationAtAbsoluteIndex(uint32_t n) {
        options_.dropNotificationAtAbsoluteIndex = n;
    }
    void ArmDeferWriteCompletions(uint32_t n) { options_.deferNextWriteCompletions = n; }
    void ArmDeferReads(uint32_t n) { options_.deferNextReads = n; }
    void ArmDeferCas(uint32_t n) { options_.deferNextCas = n; }

    /// Test-only: swap the descriptor served on reads (revalidation tests).
    void SetDescriptorOverride(std::optional<std::array<uint8_t, 48>> bytes) {
        options_.descriptorOverride = std::move(bytes);
    }

    /// Test-only: force the device's owner storage to an arbitrary value -
    /// models any pre-existing claimant (TCAT, another host, or a stale
    /// value that happens to equal the engine's intended address).
    void ForceOwner(uint64_t owner) { owner_ = owner; }

    /// Test-only: arm the register race (one-shot, see ownerRaceValue).
    void ArmOwnerRace(uint64_t racer) { options_.ownerRaceValue = racer; }

    /// Test-only: write the response area out of band (models a late device
    /// re-send landing after the current command's response). The next
    /// paired read returns these bytes.
    void InjectResponse(std::span<const uint8_t> frame) {
        std::copy(frame.begin(), frame.end(), responseArea_.begin());
        responseReady_ = true;
    }

    /// Number of response-area reads NOT paired with a completed request.
    /// Invariant observer (never asserts; tests assert).
    [[nodiscard]] uint32_t UnpairedResponseReads() const noexcept { return unpairedReads_; }
    /// Total reads served from the response area (paired + would-be-unpaired).
    [[nodiscard]] uint32_t ResponseAreaReads() const noexcept { return responseAreaReads_; }
    /// Command requests accepted (ritual reset frames excluded).
    [[nodiscard]] uint32_t RequestCount() const noexcept { return requestIndex_; }
    /// CompareSwap64 calls (ownership lifecycle assertions).
    [[nodiscard]] uint32_t CasCalls() const noexcept { return casCalls_; }
    /// Total ReadBlock calls of any kind (register contract: zero during
    /// registration - the claim CAS is the only registration traffic).
    [[nodiscard]] uint32_t ReadBlockCalls() const noexcept { return readBlockCalls_; }
    [[nodiscard]] bool SawResetFrame() const noexcept { return sawResetFrame_; }
    [[nodiscard]] uint64_t Owner() const noexcept { return owner_; }
    /// True if the request frame changed while the notification callback ran
    /// (engine-side corruption; should never happen).
    [[nodiscard]] bool RequestMutatedDuringNotify() const noexcept {
        return requestMutatedDuringNotify_;
    }
    /// Last accepted request frame (for golden-trace comparisons).
    [[nodiscard]] const std::vector<uint8_t>& LastRequest() const noexcept { return lastRequest_; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint32_t>& ControlStore() const noexcept {
        return controlStore_;
    }
    [[nodiscard]] int32_t MixerVolume(uint8_t channel, uint8_t mix) const noexcept {
        return mixerVolumes_[channel][mix];
    }

    /// P08 fault knob: set_volume commands answered with status 5 once
    /// more than this many have been accepted (SIZE_MAX = never fail).
    /// Also counts accepted+rejected set_volume receipts for the
    /// minimal-request-count assertions.
    size_t setVolumeFailAfter{SIZE_MAX};
    size_t setVolumeReceipts{0};

    static constexpr uint64_t writeAddress() noexcept {
        return 0xFFFFE0400000ULL + 0x1C0;
    }
    static constexpr uint64_t readAddress() noexcept {
        return 0xFFFFE0400000ULL + 0x30;
    }

private:
    static constexpr uint32_t kDcpMaxRequest = 1024;
    static constexpr uint32_t kResponseAreaBytes = 400;

    bool CompleteWrite(WriteDone done, bool requestIndexConsumed) {
        (void)requestIndexConsumed;
        if (options_.deferNextWriteCompletions > 0) {
            --options_.deferNextWriteCompletions;
            Defer(std::expected<void, IOReturn>{}, std::move(done));
            return true;
        }
        done(std::expected<void, IOReturn>{});
        return true;
    }

    bool CompleteRead(std::vector<uint8_t> out, ReadDone done) {
        if (options_.deferNextReads > 0) {
            --options_.deferNextReads;
            Defer(std::move(out), std::move(done));
            return true;
        }
        done(std::expected<std::vector<uint8_t>, IOReturn>{std::move(out)});
        return true;
    }

    bool CompleteCas(uint64_t previous, CasDone done) {
        if (options_.deferNextCas > 0) {
            --options_.deferNextCas;
            Defer(previous, std::move(done));
            return true;
        }
        done(std::expected<uint64_t, IOReturn>{previous});
        return true;
    }

    void Defer(std::expected<void, IOReturn> result, WriteDone done) {
        deferred_.emplace_back(
            [result, done = std::move(done)]() mutable { done(result); });
    }
    void Defer(std::expected<std::vector<uint8_t>, IOReturn> result, ReadDone done) {
        deferred_.emplace_back(
            [result = std::move(result), done = std::move(done)]() mutable { done(std::move(result)); });
    }
    void Defer(uint64_t previous, CasDone done) {
        deferred_.emplace_back(
            [previous, done = std::move(done)]() mutable {
                done(std::expected<uint64_t, IOReturn>{previous});
            });
    }

    /// Takes the WriteBlock span WITHOUT copying: the request-lifetime
    /// tripwire (see file header). The span is touched again AFTER
    /// notifySink_ returns.
    void HandleRequest(std::span<const uint8_t> request) {
        lastRequest_.assign(request.begin(), request.end());
        const uint16_t counter = static_cast<uint16_t>((request[4] << 8) | request[5]);
        const D3::Opcode op{{request[0], request[1], request[2], request[3]}};

        // Build the response frame: echo header (counter per knob), payload per command.
        std::vector<uint8_t> response;
        uint32_t rstatus = 0;

        if (op == D3::kOpcodeSetVolume) {
            ++setVolumeReceipts;
            if (setVolumeReceipts > setVolumeFailAfter) {
                // The observed nonzero answer (status 5): the device
                // rejected the command. Header-only, rlen 0.
                rstatus = 5;
                response.assign(D3::kMessageHeaderSize, 0);
            } else {
                const int32_t vol = static_cast<int16_t>((request[16] << 8) | request[17]);
                mixerVolumes_[request[18]][request[19]] = vol;
                response.assign(D3::kMessageHeaderSize, 0);
            }
        } else if (op == D3::kOpcodeGetMeter) {
            const uint8_t count = request[17];
            const uint8_t pos = request[19];
            response.assign(D3::kMessageHeaderSize + count * 4, 0);
            // Synthetic meters (encoding unknown on hardware): value = 0x1000
            // + position*0x10 + index, documented for round-trip tests only.
            for (uint8_t i = 0; i < count; ++i)
                ::ASFW::FW::WriteBE32(response.data() + D3::kMessageHeaderSize + i * 4,
                              0x1000u + pos * 0x10u + i);
        } else if (op == D3::kOpcodeGetControl) {
            const uint32_t pos = ::ASFW::FW::ReadBE32(request.data() + 16);
            const uint32_t len = ::ASFW::FW::ReadBE32(request.data() + 20);
            response.assign(D3::kMessageHeaderSize + len, 0);
            const auto it = controlStore_.find(pos);
            ::ASFW::FW::WriteBE32(response.data() + D3::kMessageHeaderSize,
                          it != controlStore_.end() ? it->second : 0);
        } else if (op == D3::kOpcodeSetControl) {
            const uint32_t pos = ::ASFW::FW::ReadBE32(request.data() + 16);
            const uint32_t len = ::ASFW::FW::ReadBE32(request.data() + 20);
            if (len == 4) controlStore_[pos] = ::ASFW::FW::ReadBE32(request.data() + 24);
            response.assign(D3::kMessageHeaderSize, 0);
        } else if (op == D3::kOpcodeWriteSwNotice) {
            response.assign(D3::kMessageHeaderSize, 0);
        } else if (op == D3::kOpcodeSetRouting) {
            // Accept either table; store the raw frame for inspection.
            lastRoutingFrame_.assign(request.begin(), request.end());
            response.assign(D3::kMessageHeaderSize, 0);
        } else {
            rstatus = options_.forcedResponseStatus; // unknown category (firmware: 4)
            response.assign(D3::kMessageHeaderSize, 0);
        }

        // Response header: echo opcode + counter (+ knob offset), payload length.
        response[0] = op[0]; response[1] = op[1]; response[2] = op[2]; response[3] = op[3];
        const uint16_t echo = static_cast<uint16_t>(counter + options_.counterEchoOffset);
        response[4] = static_cast<uint8_t>(echo >> 8);
        response[5] = static_cast<uint8_t>(echo);
        // The TRUE payload length drives the notification; the header echo
        // is where the fault knobs diverge from it (a device that echoes an
        // inconsistent header must not also corrupt the notification).
        const uint16_t truePlen =
            static_cast<uint16_t>(response.size() - D3::kMessageHeaderSize);
        response[6] = static_cast<uint8_t>(truePlen >> 8);
        response[7] = static_cast<uint8_t>(truePlen);
        if (options_.responseLengthEchoOffset != 0) {
            const uint16_t plen = static_cast<uint16_t>(
                static_cast<int32_t>(truePlen) + options_.responseLengthEchoOffset);
            response[6] = static_cast<uint8_t>(plen >> 8);
            response[7] = static_cast<uint8_t>(plen);
        }
        if (options_.responseOpcodeCorrupt) {
            options_.responseOpcodeCorrupt = false; // one-shot
            response[1] = static_cast<uint8_t>(response[1] ^ 0xFF);
        }

        if (options_.failNextResponses > 0) {
            --options_.failNextResponses;
            rstatus = options_.forcedResponseStatus;
        }

        std::copy(response.begin(), response.end(), responseArea_.begin());
        responseReady_ = true;

        if (options_.dropNextNotifications > 0) {
            --options_.dropNextNotifications;
            return; // request lands, response sits unread, no notification
        }
        ++notifIndex_;
        if (options_.dropNotificationAtAbsoluteIndex != 0 &&
            notifIndex_ == options_.dropNotificationAtAbsoluteIndex) {
            return; // this one notification is lost in the mail
        }

        // Notification frame (live-hardware shape 2026-11-06): [0..4] =
        // request opcode echo, [4..6] = command counter echo, [6..8] =
        // response payload length, [8..12] = rstatus (spro40d3.rs
        // resp.connect_requested reads [6..12]; the live device echoes the
        // opcode and counter too, and the engine's stale-notification
        // discriminator keys on the opcode).
        std::array<uint8_t, 12> notif{};
        notif[0] = op[0]; notif[1] = op[1]; notif[2] = op[2]; notif[3] = op[3];
        notif[4] = static_cast<uint8_t>(echo >> 8);
        notif[5] = static_cast<uint8_t>(echo);
        notif[6] = static_cast<uint8_t>(truePlen >> 8);
        notif[7] = static_cast<uint8_t>(truePlen);
        if (options_.nextNotificationLengthOverride != 0) {
            // One-shot adversarial rlen (oversized / wrong-shape ingress).
            const uint16_t rlen = options_.nextNotificationLengthOverride;
            options_.nextNotificationLengthOverride = 0;
            notif[6] = static_cast<uint8_t>(rlen >> 8);
            notif[7] = static_cast<uint8_t>(rlen);
        }
        ::ASFW::FW::WriteBE32(notif.data() + 8, rstatus);
        if (options_.notifyDuringWrite) {
            notifySink_(notif);
            // REQUEST-LIFETIME TRIPWIRE: the notification callback has run
            // (engine code executed re-entrantly inside this WriteBlock).
            // Touch the request span again: if the engine freed/moved the
            // buffer during the callback, this reads poisoned memory (ASan).
            const uint16_t postCounter =
                static_cast<uint16_t>((request[4] << 8) | request[5]);
            const bool unchanged =
                postCounter == counter && request[0] == op[0] && request[1] == op[1] &&
                request[2] == op[2] && request[3] == op[3];
            requestMutatedDuringNotify_ = requestMutatedDuringNotify_ || !unchanged;
        } else {
            pendingNotifications_.push_back(notif);
        }
    }

public:
    /// For notifyDuringWrite=false rigs: deliver queued notifications.
    void DeliverPendingNotifications() {
        auto queued = std::move(pendingNotifications_);
        pendingNotifications_.clear();
        for (auto& n : queued) notifySink_(n);
    }

private:
    NotifySink notifySink_;
    Options options_;
    uint64_t owner_{D3::kDcpOwnerNone};
    std::array<uint8_t, kResponseAreaBytes> responseArea_{};
    bool responseReady_{false};
    bool sawResetFrame_{false};
    bool requestMutatedDuringNotify_{false};
    uint32_t unpairedReads_{0};
    uint32_t responseAreaReads_{0};
    uint32_t requestIndex_{0};
    uint32_t casCalls_{0};
    uint32_t readBlockCalls_{0};
    uint32_t notifIndex_{0};
    std::deque<std::array<uint8_t, 12>> pendingNotifications_;
    std::deque<Deferred> deferred_;
    std::vector<uint8_t> lastRequest_;
    std::vector<uint8_t> lastRoutingFrame_;
    std::unordered_map<uint32_t, uint32_t> controlStore_{};
    std::array<std::array<int32_t, 16>, 18> mixerVolumes_{};
};

// Defined out of line: an in-class `Options options = {}` default argument
// would need Options' default member initializers before the class is
// complete.
inline SPro40D3SimulatedDevice::SPro40D3SimulatedDevice(NotifySink sink)
    : SPro40D3SimulatedDevice(std::move(sink), Options{}) {}

} // namespace ASFW::Testing::DICE
