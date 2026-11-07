// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3SimulatedScheduler.hpp - Deterministic virtual-clock
// ITimerScheduler for the SPro40D3 mailbox engine tests.
//
// Models the PRODUCTION scheduler's batch semantics, not just a queue
// (P00 baseline §4 / re-review R3; cited at DriverKitSessionScheduler.cpp
// HandleTimerFired): when the deadline arrives, all due callbacks are
// EXTRACTED under the lock and executed AFTER it is dropped. A Cancel()
// issued while that batch is running cannot stop an already-extracted
// callback - it is a no-op, not a fence. The engine must therefore never
// rely on Cancel for correctness; the same-batch A-cancels-B order is
// pinned by tests against this model.
//
// No wall clock exists here: time advances only when the test calls
// AdvanceNs/AdvanceToNs, and a callback scheduled at delay 0 still fires
// only on the next advance (ITimerScheduler contract: ScheduleAfter always
// defers, including delayNs == 0).

#pragma once

#include "Audio/Protocols/DICE/Focusrite/SPro40D3MailboxEngine.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace ASFW::Testing::DICE {

class SPro40D3SimulatedScheduler final : public ASFW::Scheduling::ITimerScheduler {
public:
    [[nodiscard]] ASFW::Scheduling::TimerToken ScheduleAfter(uint64_t delayNs,
                                                             std::function<void()> fn) override {
        if (!fn) return ASFW::Scheduling::kInvalidTimerToken;
        if (rejectNextSchedules_ > 0) {
            --rejectNextSchedules_;
            return ASFW::Scheduling::kInvalidTimerToken; // admission failure
        }
        const auto token = ++nextToken_;
        pending_.push_back(Entry{token, nowNs_ + delayNs, std::move(fn)});
        return token;
    }

    void Cancel(ASFW::Scheduling::TimerToken token) override {
        if (token == ASFW::Scheduling::kInvalidTimerToken) return;
        // Extraction model: entries already pulled into a running batch are
        // not in pending_ anymore and cannot be stopped. Matching production,
        // this returns success without preventing the extracted callback.
        std::erase_if(pending_, [token](const Entry& e) { return e.token == token; });
    }

    [[nodiscard]] uint64_t NowNs() const noexcept override { return nowNs_; }

    // -- test control --------------------------------------------------------

    void AdvanceNs(uint64_t deltaNs) { RunDue(nowNs_ + deltaNs); }
    void AdvanceToNs(uint64_t absoluteNs) {
        if (absoluteNs > nowNs_) RunDue(absoluteNs);
    }
    /// Milliseconds helper for tests written against the Stage-1 clock.
    void AdvanceMs(uint64_t deltaMs) { AdvanceNs(deltaMs * 1000000ull); }
    void AdvanceToMs(uint64_t absoluteMs) { AdvanceToNs(absoluteMs * 1000000ull); }

    /// Reject the next N ScheduleAfter calls (deadline-admission failure).
    void ArmRejectNextSchedules(uint32_t n) { rejectNextSchedules_ = n; }

    /// Deadlines not yet extracted (pending or extracted-and-running).
    [[nodiscard]] size_t PendingCount() const noexcept { return pending_.size(); }
    /// True while an extracted batch is executing (reentrancy observer).
    [[nodiscard]] bool BatchRunning() const noexcept { return batchRunning_; }

private:
    struct Entry {
        ASFW::Scheduling::TimerToken token{};
        uint64_t deadlineNs{};
        std::function<void()> fn;
    };

    void RunDue(uint64_t nowNs) {
        nowNs_ = nowNs;
        // Extract all due entries first (under the "lock")...
        std::vector<std::function<void()>> due;
        std::erase_if(pending_, [&](const Entry& e) {
            if (e.deadlineNs <= nowNs_) {
                due.push_back(std::move(e.fn));
                return true;
            }
            return false;
        });
        // ...then execute them after it is dropped. A Cancel() from inside
        // this loop only touches pending_ (production parity).
        batchRunning_ = true;
        for (auto& fn : due) fn();
        batchRunning_ = false;
    }

    std::vector<Entry> pending_;
    uint64_t nowNs_{0};
    ASFW::Scheduling::TimerToken nextToken_{0};
    uint32_t rejectNextSchedules_{0};
    bool batchRunning_{false};
};

} // namespace ASFW::Testing::DICE
