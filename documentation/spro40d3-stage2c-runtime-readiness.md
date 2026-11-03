# FW-203 Stage 2C — dormant runtime safety and source-review readiness

**Disposition: approved for a host-only stacked PR. Not deployment or physical-mailbox readiness.**
This delta is based on `dc8880b4` (Stage 2B) on
`feat/fw203-spro40d3-stage2c-runtime`. No signature, installation,
live mailbox transaction, owner CAS, or hardware experiment was
performed. No mixer UI, streaming, or normal enumeration activation was added.
Stage 2B's dedicated-control-queue description is superseded below.

## 1. Updated architecture and ownership contracts

Production composition remains service lifetime:

```text
DriverWiring::EnsureSbp2Deps
  ServiceContext synchronized mailbox publication slot
  ControllerCore retained facade (retired values destroyed outside its lock)
  SPro40D3DriverKitManager
    detached shared Runtime + one lifecycle/state lock
    notification hub + local AddressSpaceManager endpoint reservation
    cached diagnostics; unconditional activation denial

Host tests only (ASFW_HOST_TEST)
  exact-scope, exact-route/session/runtime authorization
  simulated completion tickets, explicit virtual timeout, local cancellation

Unactivated protocol components
  Session / MailboxEngine / Transport -> canonical ProtocolRegisterIO -> fake I/O in tests
```

No production session, engine, transport, route binding, timer, or dispatch queue
is constructed by the manager. Removing the unused queue eliminates synchronous
self-dispatch and lifecycle-lock-across-dispatch instead of pretending that an
empty queue is a live cancellation/drain design.

The runtime lock serializes state transitions and short non-callback registry
metadata checks. `Stop()` closes admission, claims pending cancellations, clears
the borrowed registry pointer, and detaches endpoint/hub state before unlocking.
Endpoint retirement, callback invocation, and callback-capture destruction occur
outside the runtime lock. Late simulator tickets retain only detached shared
state; they do not own or dereference the manager or borrowed dependencies.

An already-claimed host callback can overlap `Stop()`: Stop is deliberately not
a self-wait barrier. The invoking API frame must retain the manager; callbacks
must retain any observers they use, must not throw, and must not destroy their
invoking manager. No manager-owned callback epilogue accesses registry/clock/bus.
This is an explicit host contract, **not** a production live-callback drain.

Selector 1018 now acquires a retained manager from a synchronized, service-lifetime
`ServiceContext` slot, not by entering a raw controller that runtime Reset may
have destroyed. Reset withdraws the slot before Stop/dependency destruction.
The mailbox portion of reset fanout also takes a retained slot copy. As with
other existing UserClient handlers, the service/context must remain alive for an
active invocation; service free must follow callback/UserClient drainage. Broader
audio/discovery callback-queue lifetime remains a separate audit prerequisite.

Notification ingress copies bounded raw frames only. AddressSpaceManager invokes
its weak endpoint callback outside its own lock; promoting it pins endpoint/hub.
Endpoint ingress checks its lease then releases its lock before the injected clock
or hub ingress. Hub retirement either erases an enqueued copy or rejects ingress
which has not committed. Registry and clock must outlive active direct hub users;
a strong endpoint reference alone does not retain those borrowed dependencies.

Transport calls require registry, bus and clock alive for the active call.
`CompletionRouteGuard::Close()` synchronizes registry checks before cancellation
or return. Detached native callbacks may retain request/pending storage but no
longer touch the borrowed registry. Cancellation is **detachment**, not evidence
that hardware stopped. The guard lock is never held across external completion.

## 2. Exact changed files

Relative to the Stage 2B base, the source/test changes are:

- `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3DriverKitManager.cpp`
- `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3DriverKitManager.hpp`
- `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3NotificationEndpoint.cpp`
- `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3NotificationEndpoint.hpp`
- `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3NotificationHub.cpp`
- `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3NotificationHub.hpp`
- `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3Session.cpp`
- `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3Session.hpp`
- `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3Transport.cpp`
- `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3Transport.hpp`
- `ASFWDriver/Controller/ControllerCoreFacades.cpp`
- `ASFWDriver/Discovery/DeviceRegistry.cpp`
- `ASFWDriver/Discovery/DeviceRegistry.hpp`
- `ASFWDriver/Protocols/Ports/ProtocolRegisterIO.hpp`
- `ASFWDriver/Service/DriverContext.cpp`
- `ASFWDriver/Service/DriverContext.hpp`
- `ASFWDriver/Shared/ASFWDiagnosticsABI.h`
- `ASFWDriver/UserClient/Handlers/DiagnosticsHandler.cpp`
- `tests/common/DiagnosticsServiceTests.cpp`
- `tests/devices/CMakeLists.txt`
- `tests/devices/SPro40D3DriverKitManagerTests.cpp`
- `tests/devices/SPro40D3NotificationEndpointTests.cpp`
- `tests/devices/SPro40D3NotificationHubTests.cpp`
- `tests/devices/SPro40D3SessionTests.cpp`
- `tests/devices/SPro40D3TransportTests.cpp`
- `documentation/spro40d3-stage2c-runtime-readiness.md` (new report)

No new production source/header files were added. Existing `project.yml` glob
already registers these files; XcodeGen reproducibility is checked rather than
manually modifying the generated project. The review archive's manifest is the
authoritative final inventory; local `.letta/` runtime metadata is excluded.

## 3. Hazards fixed and test intent

- Removed manager `DispatchSync` and the unused control queue: no lifecycle lock
  spans dispatch or callback work, and lifecycle reentry cannot self-dispatch.
- Serialized simulated operation terminal ownership: reset/removal/Stop,
  completion and explicit timeout cannot deliver two terminal callbacks.
- Detached late tickets and released user captures promptly. Callback and capture
  destruction can reenter diagnostics/lifecycle without taking a held state lock.
- Closed transport registry fences on **all** return paths, including empty
  native handle, success, timeout, stale route and synchronous cancel completion.
- Serialized whole transport result publication. Concurrent duplicate callbacks
  cannot mutate the first published payload; a published result wins a deadline
  race. Transient native payloads are copied into owned storage.
- Removed endpoint-lock-across-clock/hub-ingress deadlock; latch-controlled tests
  force retirement and same-generation replug before queue commit.
- Retained source/address/generation/tCode/payload-length validation, bounded FIFO,
  tombstone reservations and exact-address collision exclusion.
- Moved last-manager destruction outside the controller publication lock and
  added a stable service-context publication slot for cached diagnostics/reset.
- Guarded the entire host Session startup against nested Start, and used a
  lifecycle epoch so descriptor/register waits cannot overwrite Stop/removal/reset.
  Quiesce permanently closes the transport's local submission admission before
  retiring ingress; registration interrupted during its **second descriptor read**
  cannot submit an acquisition CAS to a retired address. This does not undo an
  already-submitted transaction; the active serialized call still owns dependencies
  until it returns. Known foreign-owner loss retires the local lease, without a
  speculative release CAS. Lifecycle invalidation clears stale owner evidence.
- Separated cached identity from explicit route validity, preserved invalid-node
  sentinel 0xFFFF and bounded reset freshness with a registry epoch high-watermark;
  fresh rediscovery does not revive authorization or require an earlier query.

Tests use latches/barriers and virtual clocks, not timing sleeps as proof of race
coverage. Host evidence cannot prove DriverKit scheduling or physical behavior.

## 4. Scope authorization and disabled production gate

Four independent scopes exist: passive descriptor, notification ownership,
read-only mailbox, and mixer/routing write. No scope implies another.
`RequestActivation()` returns `kIOReturnNotPermitted` for every scope, including
invalid enum values, in every configuration. `kProductionActivationEnabled` is a
literal `false`; changing `ASFW_SPRO40D3_EXPERIMENTAL_ACTIVATION` cannot enable it.
Manager host tests deliberately compile that macro as 1.

Only `ASFW_HOST_TEST` exposes authorization/completion APIs. A simulation policy
must match the catalog-confirmed TCD3070 DICE identity, unit version, full
`DeviceRouteToken` (GUID/incarnation/generation/node/route epoch), exact scope,
nonzero globally unique session token, and runtime generation. Admission **and
completion** revalidate. Cross-manager replay, mismatched components, stale route,
reset/removal and Stop invalidate tickets. A simulator permit is neither external
authentication nor authority to issue a real transaction. No unrestricted public
job queue/register endpoint or automatic discovery activation was introduced.

## 5. Cached diagnostics and provenance

Shared diagnostics ABI is **13**. `ASFWDiagSPro40D3Mailbox` remains prefix-compatible
with ABI 12 and grows from 104 to 168 bytes; compile-time assertions pin size and
key offsets. Selector **1018** accepts exactly one 64-bit GUID and only copies
cached/local metadata. It never reads a DCP descriptor, polls firmware, binds a
lease, claims ownership, submits I/O, or writes mixer/routing data. Prefix
layout preservation is not old-app compatibility: the app validates the exact
shared ABI version, so any future deployment requires a matched rebuilt app+dext
pair; the existing ABI12 app must not be assumed compatible with this ABI13 dext.
This applies to all diagnostics using the shared header, not only selector 1018:
`ASFW/ViewModels/ASFWDiagnosticsClient.swift` imports the ABI through
`ASFW/ASFW-Bridging-Header.h` and requires exact `header.abiVersion` equality with
`ASFW_DIAG_ABI_VERSION`, as well as the imported structure's expected size.
An ABI 12 application binary retains its compiled version expectation; preserving
a structure prefix does not negotiate or bypass that check. Future deployment
must rebuild and validate both the application and DriverKit extension together.

The envelope remains `Unavailable` for live mailbox evidence. Cached discovery
vendor/model/GUID and observed node/bus generation are distinct from actual DCP
identification. `header.generation` is the cached bus generation;
`runtimeGeneration` is a local invalidation epoch, not a bus generation.
`sessionToken`, session/activation state, reservation state and notification
address do not establish device ownership. `observedForeignOwner` and
`ownerProvenance` remain unavailable (zero); zero is not an owner-none sentinel.

ABI 13 additionally exports pending, cancelled and timed-out operation counts,
identification/reservation/owner provenance, last error and the explicit
`hardwareActivationDisabled=1` flag. Production operation counters remain zero;
nonzero simulator counters are test evidence only. Reset/removal/Stop revoke
session authorization, increment runtime generation and clear pending state;
retired reservations are tombstoned rather than reused. Stopped snapshots never
dereference the destroyed registry. Identification state is `0 unavailable`,
`1 cached discovery`, `2 stale cached discovery`; these values describe registry
metadata for the requested GUID, including non-TCD devices, **not** confirmation
of a DCP/TCD3070 identity. Physical descriptor identification remains unavailable.
The independent `routeValid` field at offset 164 is `1` only for a current,
non-quarantined full registry route. Missing/stale routes use invalid-node sentinel
`0xFFFF`, generation zero and routeValid zero, not valid node zero. A genuinely
advanced fresh registry route can restore local node/bus-generation metadata after
reset/removal without binding a mailbox or authorizing/reviving any ticket.
A bounded registry route-epoch high-watermark fence replaces query-created
per-GUID history. Reset/removal quarantines routes at or below that local fence;
a validated newer route restores metadata even on the first-ever query for its
GUID. Routes not advanced past the manager's conservative lifecycle fence remain
unknown. This local fence is not the actual bus generation.

## 6. Framing evidence and conservative ingress policy

Read-only local behavioral reference:
`references/snd-firewire-ctl-services/protocols/dice/src/focusrite/spro40d3.rs`
in the primary checkout, not this worktree. The read-only reference audit pinned
reference HEAD `7654d40cb0e62426e460c0b94605e2a4b506d6bb`,
file SHA256 `a230e75e13cd3c1cb65322782c3a655429c906dd43812b52da8d053f4878da69`:

- Lines **249–251** decode length from bytes `[6..8]` and status `[8..12]`:
  evidence of a **12-byte readable prefix**.
- Line **258** reserves **16 address bytes**: evidence of reservation size,
  **not** the exact size of an actual device notification write.

Primary-checkout `tools/firmware_analysis/Pro40d3-DCP.md:27–30` describes a
16-byte **request** transport header and segmented responses. Its lines 3–6
identify local IDA-database provenance. Those notes were read; the underlying
firmware database was not independently reanalyzed here. The analysis-note hash
(SHA256) is `19e9ec922c9f4f557c105394bdc9ff7679b7d1b02b9b9fb9ee0a142e578b2d78`.
Request-header size
cannot establish notification-write size.

The codec's minimum-12-byte parsing behavior remains distinct from ingress.
Ingress continues to accept **exactly 16 raw bytes** and reject raw 12-byte
writes; it never pads 12 to 16. `kAddressBytes` names reservation extent separately
from `kFrameBytes` policy. Synthetic vectors test lengths 0–17, prefix decoding
and opaque suffix preservation, not device compatibility. Actual wire length
remains a blocker requiring independently verified firmware construction or a
separately authorized physical capture. No GPL/LGPL reference code was copied.

## 7. Complete validation and source review

Validation is performed on Darwin with Xcode 27.2 / XcodeGen 2.46.0. Complete
commands, output and terminal statuses are retained under
`build/fw203-stage2c-validation/` and included in the review archive. The existing
Release host configuration and version-refresh logs were reused, not silently
replaced. Sanitizers use separate build directories. Final evidence uses only
`final-*.log`/XML produced after the tracked-source freeze; earlier attempts,
including the older `xcode-debug-final.log`, are history rather than final evidence.
The initial Debug undefined-destructor link failure occurred during overlapping
source edits/builds; its failure is preserved, not counted as a passing build.

Tracked-source freeze: **2026-11-03 06:03:56 UTC**, all **2,114 tracked
inputs**, manifest SHA256
`445dd6eebd39844a5e32f5e000dbf2cac98bbd46aef1b993e34a5082c49a7609`.
No tracked source changed afterward. The new report is review evidence rather
than a compiled input and was completed after testing. Final build/test runs
finished at 06:10:34 UTC; each command/build/test phase below has terminal exit
status **0**.

| Final check | Actual result | Evidence in validation directory |
| --- | --- | --- |
| Full Release host build + CTest | **4,125 registered; 4,119 passed; 6 skipped; 0 failed** | `final-host-build.log`, `final-host-tests.log`/XML |
| ASan + UBSan selected mailbox/transport/lifetime/allocator tests | **219/219 passed**, no findings | `final-asan-build.log`, `final-asan-tests.log`/XML |
| TSan selected manager/transport/hub/endpoint/allocator tests | **116/116 passed**, no findings | `final-tsan-build.log`, `final-tsan-tests.log`/XML |
| XcodeGen 2.46.0, before/after reproducibility | Generated project matches HEAD exactly | `final-xcodegen*.log` |
| Unsigned Debug DriverKit build | **BUILD SUCCEEDED**, DriverKit 27.2 SDK, x86_64 + arm64e | `final-xcode-debug.log` |
| Unsigned Release DriverKit build | **BUILD SUCCEEDED**, DriverKit 27.2 SDK, x86_64 + arm64 | `final-xcode-release.log` |
| Read-only source review | Required R1–R4 resolved; no blocking findings in this delta | `source-review-initial.log`, `source-review-final.log` |
| Source/delta integrity | Frozen tracked hashes unchanged; delta applies to the stated base and reproduces every current tracked input plus report | `final-source-freeze.json`, `final-source-diff-before.log`, `final-source-diff-after.log`, `final-delta-apply-check.log` |
| Signing identity probe | **0 valid identities**; this is a blocking deployment fact, not a provisioning pass | `final-signing-identity-audit.log` |

The host skips are unchanged external-fixture/documentation cases: three
`SelfIDSequenceEnumeratorTests` need `firewire/self-id-sequence-helper-test.c`;
two `ConfigROMReferenceCrcTests` vectors need `firewire/device-attribute-test.c`;
`ATDescriptorZValueTests.CommandPtr_Z0_CausesNoFetch` deliberately documents the
fixed Z=0/no-DMA bug. Exact names/reasons are preserved in XML/stdout. No SPro40D3
case was skipped. The selected runs include **37 Session cases**, including six
second-descriptor no-CAS cases, and **22 manager cases**, including first-query
fresh rediscovery and detached callback/dependency lifetime coverage. Sanitizer
configurations are Release with ASan+UBSan or TSan enabled independently; LSan is
disabled and leak-sanitizer coverage is not claimed.

The source-review package is `/tmp/fw203-stage2c-runtime-readiness-review.zip`, with an
external `.sha256` companion. It preserves the approved pre-submission source
snapshot and report (**2,115 source entries**), `delta.patch`, exact changed-file
inventory, validation commands/logs/XML, source freeze and a per-file SHA256
manifest. ZIP CRCs, every manifest payload hash and every source entry are
verified. Produced apps/dexts, signing material, fetched dependencies, build
caches and local runtime metadata are excluded. The archive's external checksum
is deliberately outside the embedded report to avoid a self-referential hash.

### Approval-stage application compatibility check

The complete **ASFW application Release scheme**, including its DriverKit target
dependency and embedded extension, built successfully with signing disabled:

```sh
env SKIP_VERSION_BUMP=1 xcodebuild -project ASFW.xcodeproj -scheme ASFW \
  -configuration Release -derivedDataPath build/fw203-stage2c-xcode \
  CODE_SIGNING_ALLOWED=NO ONLY_ACTIVE_ARCH=NO build
```

The application contains x86_64 and arm64 slices. `ASFWDiagnosticsClient.swift`
compiled for both architectures against the updated shared declarations. A
separate compile-only Swift IR probe through the actual application bridging
header confirmed `ASFW_DIAG_ABI_VERSION == 13`,
`MemoryLayout<ASFWDiagSPro40D3Mailbox>.size == 168`, and access to the new cached
fields. That probe was not linked or executed. No compatibility code correction
was necessary, and all 2,114 frozen tracked inputs remained unchanged.

Supplemental evidence is retained locally in
`build/fw203-stage2c-validation/approval-unsigned-app-release.log` and
`approval-swift-abi13.log`, with the probe source/IR beside them. These checks and
this report addendum postdate the immutable source-review ZIP; they are not
claimed to be part of its original manifest. No application, installer,
UserClient connection, or hardware session was launched. Successful compilation
does not establish runtime compatibility of an old ABI 12 application binary
with an ABI 13 extension; the matched app/extension requirement in section 5
remains mandatory for any separately authorized deployment.

Neither unsigned compilation nor sanitizer success is signing/provisioning,
installation, DriverKit runtime, real callback-drain, or hardware evidence.

## 8. Actual signing, protected deployment and rollback state

The read-only `security find-identity -v -p codesigning` probe reports **0 valid
identities**, exit 0. No signature was created. `project.yml` configures automatic
signing/team `F6YA6B56LR`; that is not proof of credentials or capability grants.
App `net.asfw.app` and dext `net.asfw.driver` need separate authorized profiles
and signatures, with matching embedded dext/UserClient identifiers. The dext
requests DriverKit, PCI, Audio, MIDI and SCSIController capabilities; Debug's PCI
entitlement is broader than Release. Release app sandboxing does not replace the
exact dext UserClient entitlement. No permissive client exception was added.

For a future approved build, verify identifier ownership, valid private-key
identity, actual per-target profile entitlements/device/expiry, exact bundle IDs,
team/certificate chain, nested integrity and native architecture slices. Current
Debug is x86_64/arm64e; Release is x86_64/arm64. Do not treat arm64e preview-ABI
requirements as an ordinary protected-deployment recipe. A development profile
alone is **not a demonstrated SIP-intact deployment path**. Obtain the applicable
Apple-supported, properly entitled Developer ID/notarized or other authorized
route; notarization does not substitute for DriverKit capability grants. Normal
platform protections must remain enabled. No ad-hoc/SIP/AMFI bypass is authorized.

Do **not** use current `install.sh` as rollback preparation: it deletes the
existing app before copying its replacement. Preserve the entire known-good
signed containing app+dext bundle unchanged, with version, signature/profile,
architectures and provenance. Do not replace its embedded dext in place. A new
identifier pair avoids same-ID registration replacement but does not isolate the
PCI controller; two driver personalities can still compete for hardware.

Future rollback must use supported activation/deactivation, honor user approval
and reboot-required results, and retain the original containing app until removal
finishes. Do not delete `/Library/SystemExtensions` manually. Current
`DriverInstallManager` rejects non-newer replacement builds, so same-ID downgrade
is not guaranteed; plan deactivate/removal/reboot then activate the preserved
known-good pair. Its current success-result handling also starts verification
for `.willCompleteAfterReboot`; that is an existing app readiness gap, not proof
of immediate activation/detachment. No Swift/install behavior was changed here.

## 9. Remaining physical/runtime risks and readiness gaps

- No signed/provisioned installation, load, sanctioned UserClient connection or
  security-policy eligibility was demonstrated; no valid signing identity exists.
- No production live execution queue, authorization issuer, timeout pump,
  callback-draining barrier, or activation path is implemented. Designing these
  is a separate source review, not a macro flip.
- Claimed callbacks use the explicit retaining contract; Stop does not wait for
  arbitrary user callbacks. A future live runtime needs a verified completion
  and dependency ownership/drain design without Default/control queue cycles.
- Service free must outlive active UserClient/fanout calls. Existing non-mailbox
  audio reset fanout and discovery raw-observer unregistration need their own
  owner-queue/drain proof before expanding live work. Weak mailbox capture does
  not pin an AudioCoordinator observer or an arbitrary ServiceContext callback.
- Host allocator uniqueness/tombstones do not prove firmware acceptance of the
  composed host address, post-reset source IDs, sleep/wake lifetime or physical
  notification length. Retained reservations consume controller-lifetime space.
- Descriptor contents, actual owner sentinel/foreign-owner behavior, acquisition,
  retention/release, segmentation, real timeout timing and every mixer/routing
  effect remain physically unobserved.
- Existing unrelated analyzer/conversion warnings are recorded in build logs;
  no claim of repository-wide warning cleanup or synchronization proof is made.

**Readiness remains source-review-only.** Host validation supports review of the
dormant implementation, not deployment, live registration, or audio changes.

## 10. First-test protocol — separate approval checkpoints, proposal only

Each checkpoint requires fresh explicit approval and its own recorded stop/go
result. This document grants no authority to execute any of them.

1. **Preserve known-good/rollback:** record macOS/controller/device GUID+ROM,
   versions, current audio configuration and signed app+dext provenance. Preserve
   and verify the complete known-good bundle; keep alternate non-ASFW audio.
   Stop if protected rollback cannot be demonstrated.
2. **Signing/installation eligibility:** obtain approved app+dext identities,
   profiles and permitted native architectures. Inspect actual entitlements,
   signatures, nested integrity and supported deployment eligibility while
   protections stay enabled. Authorize installation separately; honor approvals
   and reboot state. This source package is unsigned and cannot satisfy this gate.
3. **Cached identity only:** with audio idle, no mounted FireWire storage, monitor
   levels down and logs prepared, authorize only cached metadata observation.
   Confirm exact TCD3070 identity/current route. No descriptor read or bus command
   is implied. Stop on stale identity, reset, mismatch or unexpected I/O.
4. **Passive descriptor evidence:** first implement/review a narrowly authorized,
   bounded read-only diagnostic path and lifetime/timeouts. Only then separately
   authorize a descriptor read. Record bytes/provenance and validate against the
   known firmware profile. No owner registration, mailbox or control permission.
5. **Notification/ownership experiment:** only after actual framing evidence,
   local reservation/address composition, foreign-owner observation, production
   timeout/drain and an independently verified release/recovery strategy are
   reviewed, separately authorize at most one registration. The current code has
   **no verified ownership-release CAS**, so this checkpoint remains blocked;
   an assumed power-cycle recovery is not sufficient evidence. Any foreign owner other than the exact verified none
   sentinel is an immediate stop; no takeover or speculative release CAS. No
   mailbox reads are implied. Reset/removal invalidates the session immediately.
6. **Read-only mailbox:** require another approval after registration evidence;
   run one allowlisted bounded read with logs and validated response. No writes
   or scope inheritance; stop on framing, identity, owner or generation mismatch.
7. **Mixer/routing writes:** out of this stage. Require configuration backup,
   separately reviewed commands/recovery, physical safety checks and a new
   explicit approval. Successful reads never imply write authorization.
8. **Recovery/rollback:** on any unexpected result stop all further experiments,
   preserve logs, use supported experimental deactivation and known-good
   reactivation/reboot as required, and verify known-good audio. Do not repair an
   orphaned owner via unverified CAS or change platform protections.
