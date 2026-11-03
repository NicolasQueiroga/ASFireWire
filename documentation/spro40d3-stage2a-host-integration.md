# FW-203 Stage 2A — host-only transport and lifecycle integration

## 1. Scope and review boundary

Stage 2A is based on Stage 1 commit
`a14152fedf66c94a86a87f3c4f1920f7aed85293`, on branch
`feat/fw203-spro40d3-stage2a-transport`. Stage 1 PR:
<https://github.com/mrmidi/ASFireWire/pull/241>. Review this delta against
`feat/fw203-spro40d3-mailbox-host`, not as a replacement for that PR.

**No physical device transactions, live notification registration, driver
installation/replacement, automatic provider activation, or audio-streaming
changes are part of this stage. No Stage 2 PR is authorized yet.** All exercised
I/O below uses injected host mocks. No Stage 1 protocol source is modified.

The code implements the DriverKit-facing ports, but this is NOT a claim that
it has been built, signed, loaded, or validated as a running DriverKit service.
Production composition remains deliberately disconnected.

## 2. Architecture

```text
Future explicit control-queue owner (not constructed by the current factory)
    SPro40D3Session : IControlProvider
       | owns engine + immutable route-bound transport
       SPro40D3MailboxEngine (unchanged Stage 1)
       | ISPro40D3Transport
       SPro40D3Transport
       | IFireWireBusOps / IFireWireBusInfo / DeviceRegistry route token
       ASFW async transaction infrastructure

Future LocalRequestDispatch participant (NOT installed by this stage)
    SPro40D3NotificationHub : ILocalAddressHandler
       | exact source + generation + slot match; copy 16 bytes, timestamp
       bounded per-lease FIFO
       | Take() on the control queue, never a direct callback into the engine
       SPro40D3Session::ServiceNotifications()
```

### Transport adapter

The adapter submits absolute 48-bit reads, writes and compare-swap locks through
the existing `Async::IFireWireBusOps`. This is the same neutral bus seam consumed
by `Protocols::Ports::ProtocolRegisterIO`; no OHCI/DMA mechanics or audio-stream
code are changed. An immutable `DeviceRouteToken` ties each adapter to a GUID,
device incarnation, route epoch, generation and node. The registry validates it
before submission, while waiting, and on completion. Rediscovery uses a new
adapter rather than mutating an in-flight route.

`DiceWaitClock` provides bounded waits (default 2000 ms) without blocking the
Default queue that delivers bus completions. There are no adapter retries.
Timeout/route loss abandons the pending result and requests native cancellation;
late callbacks own their pending storage and do not reference the adapter or a
returned caller buffer. Dependency lifetimes still matter: the bus, registry,
clock and bus-info service must outlive their users, and registry lifetime must
extend through outstanding callbacks.

Read lengths are exact, quadlet-aligned and bounded. Addresses are checked for
48-bit overflow. 64-bit CAS operands use **low 32-bit big-endian quadlet first,
then high quadlet**, for both compare and replacement; the old owner is decoded
the same way. Do not substitute the generic `CompareSwap64BE` encoding. The
behavioral source is `snd-firewire-ctl-services`,
`protocols/dice/src/focusrite/spro40d3.rs:259–279`. Only protocol facts were used;
no reference implementation or test vectors were copied.

### Notification allocation and ingress

The hub implements ASFW's existing `Async/Rx/LocalRequestDispatch.hpp` interface.
It does not install itself. Its constructor requires an explicitly supplied,
exclusive 48-bit local aperture; there is intentionally no production default.
Aligned 16-byte slots are bump-allocated, and **never reused for this hub's
lifetime**, even after release/reset. A lease binds the slot to a full route.
Local lease release removes ingress eligibility and queued copies; it does NOT
perform the unverified device-side release CAS.

The receive handler accepts only a 16-byte block write from the expected full
local-bus source ID and generation while the registry route is current. It
copies the frame and ingress timestamp into a bounded 32-frame FIFO. Wrong
source/generation, malformed requests, retired addresses and queue overflow
are rejected without touching the engine. Ingress never reads the device, waits
for a bus completion, or calls a session callback. The reference reserves a
16-byte receiver before publishing the address (`spro40d3.rs:245–259`).

**Allocation precondition, not yet a production guarantee:** the future
composition root must reserve this aperture globally against CSR/FCP/DICE/MOTU/
Fireworks/SBP-2 handlers and other sessions. It must provide one allocator for
the controller/process lifetime and a boot/host-unique allocation strategy.
Constructing two hubs with overlapping regions, or recreating a hub at the same
base, defeats the no-reuse guarantee. Cross-host address uniqueness and firmware
interpretation of the owner/address bits remain hardware-validation gates;
a local bump allocator alone cannot establish them. Test addresses are fixtures,
not recommended live addresses.

### Lifecycle and strict activation boundary

Construction has no I/O. `StartForHostValidation(route)` is the only explicit
registration entry. Admission requires the current catalog policy's exact
`FocusriteSPro40Tcd3070` variant, DICE family, selected interface version 1 and
matching current route. This preserves the catalog's GUID-product `0x13`
exception for model `0x00DE`; it does not broaden generic DICE or TCD22xx
activation. See `DeviceProfiles/Audio/Definitions/Focusrite.hpp` and
`DeviceProfiles/Audio/DiceIdentity.hpp` for existing identity rules. Names alone
and legacy flat vendor/model fields do not authorize a mailbox probe.

The descriptor must match the characterized Stage 0 layout: magic, version 2,
1024-byte maximum request, 400-byte response area, write window `base+0x1C0`,
read window `base+0x30`. Unknown layouts fail closed before any ownership CAS.
The receiver lease exists before the engine publishes it. Stage 1 registration
revalidates the descriptor and claims **only** the constant none sentinel;
foreign ownership is diagnostic conflict, never takeover or automatic retry.
Duplicate registration reuses the same engine so confirmed ownership loss
invalidates readiness instead of reconstructing away its history.

Session calls, completions and destruction belong to one serialized control
queue; the bus queue only touches the locked hub. During notification service,
current-time deadline polling precedes processing: an old queued timestamp must
not permit a response-area read after the command deadline. Synchronous ingress
during a request write is queued until the control queue is ready.
The future queue owner must schedule `ServiceNotifications` periodically even
when no frame arrives, so missing notifications reach the engine deadline.
This stage injects neither a live dispatch source nor a controller timer.

Reset/removal/stop close admission and retire the local receiver before engine
cancellation callbacks run. Stage 1's `OnBusReset` is used to invalidate
registration, controls and pending work; consequently cancellation reports
`kBusReset` for teardown too, rather than introducing an unreviewed engine error.
Reentrant stop/reset is supported without destroying an engine on its callback
stack. Callbacks must not destroy the session itself; its owner retains it until
the outer API returns. Removal is terminal for that session. Rebind after reset
is explicit, requires a newly current route, and repeats identity/descriptor
checks. Retained ownership is conflict, not evidence that a reset cleared it.

Existing reset ordering is the model: `ControllerCoreInterrupts.cpp` invalidates
registry mappings before higher-level work; `AudioCoordinator::OnDeviceRemoved`
retires work before cleanup. **These paths are not changed or hooked up here.**
A future composition root must serialize session events onto the control queue,
drain it before dependency destruction, and quiesce local dispatch before
destroying the hub. No code may transact after hardware detachment.

### Control-provider boundary

The session implements the existing `Audio::Controls::IControlProvider` rather
than adding a parallel control API. `Prepare` does not start registration,
`CopySnapshot` publishes no topology, and both parameter and route setters fail
unsupported without I/O. No mixer, route, volume, meter or other unverified
capability is advertised to CoreAudio or the application.

`ReadControl32ForValidation` is an explicit diagnostic entry used by host tests,
not a published production parameter. A future reviewed projection can map
`Observed`/`Cached` to the existing model origins; `Unknown` has no corresponding
enum and must be represented as absence, not falsely labeled `Observed` or
`Derived`. This stage adds that mapping but no topology or capability changes.

### Build isolation

Only the three new host-test targets compile the new adapters/session. There
are no changes to `FamilyProtocolConstruction`, `IDeviceProtocol`, the catalog,
`LocalRequestWiring`, `ServiceContext`, AudioDeviceHost or streaming code.

`project.yml` now explicitly excludes `Audio/Protocols/DICE/Focusrite/SPro40D3*`
from the recursive DriverKit source glob. This also protects Stage 1 files from
being silently picked up by a future XcodeGen run. XcodeGen was run; the generated
project stayed byte-identical and contains no mailbox references. No signing
settings, entitlements, version or generated build artifacts were changed.

## 3. Host validation

The integration tests use the real registry/catalog and mocked native bus I/O;
the session rig translates native lock operands and feeds the unchanged Stage 1
simulator. Simulated success establishes plumbing, not firmware compatibility.
In particular, Stage 1's simulator emits the 12 notification bytes consumed by
its parser. The session test rig pads the unused tail to the receiver's modeled
16-byte frame. This is explicit fixture adaptation, **not evidence of a captured
16-byte notification**. Exact live notification length and source-ID framing
must be checked; a different length is rejected, not silently padded in the
production-facing handler.

| Target | Tests | Principal coverage |
|---|---:|---|
| `SPro40D3TransportTests` | 39 | Native addresses/speed/generation; low-quadlet-first CAS; previous-owner return; exact reply shape; limits/alignment; synchronous and delayed completion; route loss before/during/after submission; failed submission; timeout/cancel; late callbacks and request-buffer lifetime |
| `SPro40D3NotificationHubTests` | 14 | Allocation/exhaustion; non-reuse; full source/generation matching; malformed ingress; copy/timestamp ownership; bounded FIFO wrap/overflow; stale/replugged routes; two-device attribution; concurrent ingress/release |
| `SPro40D3SessionTests` | 23 | No implicit activation/capabilities; actual bus adapter + queued notification flow; exact catalog variant and GUID exception; bad descriptor/foreign owner/CAS race; confirmed ownership loss; notification race/deadline; explicit reset/rebind; retained owner; reentrant stop/removal; pending-command removal/destruction; provenance boundary |
| Unchanged Stage 1 codec/engine targets | 14 + 52 | Protocol regression baseline |

**Full Release host regression:** **4060 registered tests, 4054 passed,
6 pre-existing Linux-fixture-dependent skips, 0 failures.** All 76 new Stage 2A
tests passed. The unchanged Stage 1 engine/codecs also passed (52/52 and 14/14).

**ASan+UBSan:** all **142/142** mailbox tests passed, including all **76** new
Stage 2A cases, with no sanitizer reports. This is a targeted sanitizer run,
not a claim that the entire repository was rebuilt under sanitizers in Stage 2A.

Reproduction (host only):

```sh
./bump.sh refresh  # required ignored DriverVersion.hpp; no version/build bump
cmake -S tests -B build/tests_build -DCMAKE_BUILD_TYPE=Release -DASFW_ENABLE_ASAN=OFF
cmake --build build/tests_build --parallel 8
ctest --test-dir build/tests_build --output-on-failure --parallel 8

cmake -S tests -B build/tests_asan -DCMAKE_BUILD_TYPE=Release -DASFW_ENABLE_ASAN=ON
cmake --build build/tests_asan --parallel 4 --target \
  SPro40D3TransportTests SPro40D3NotificationHubTests SPro40D3SessionTests \
  SPro40D3MailboxEngineTests SPro40D3MailboxCodecTests
ctest --test-dir build/tests_asan -R SPro40D3 --output-on-failure
xcodegen generate  # project generation only, no dext build or installation
git diff --check
```

Final logs are retained in the ignored `build/fw203-stage2a-validation/`
directory. A clean worktree needed `bump.sh refresh` before the full host build;
direct CMake otherwise encountered the existing missing generated-header
dependency. No tracked version metadata changed.

Code review covered correctness, lifetime, activation boundaries and build
isolation; prompt route-loss cancellation and CAS alignment were tightened, and
direct pending-removal/destruction regressions added before final testing.
There is no deterministic cross-thread completion-versus-timeout stress test;
shared pending lifetime and publication are covered by delayed-completion and
cancellation tests, not by a ThreadSanitizer proof. Real DriverKit queue/teardown
scheduling, address reservation, signatures and firmware behavior remain untested.

## 4. First hardware-validation checklist — future authorization required

This is a procedure proposal, **not authorization to execute it**. Stage 2A
must first be reviewed. Production wiring, a signed build, installation and each
live transaction require a separate agreed plan and explicit approval.

### A. Before building/installing anything

- [ ] Review Stage 2A, keep its delta stacked over Stage 1, and approve a narrowly
  scoped live-test build. Do not simply remove exclusions and auto-start sessions.
- [ ] Record the target GUID/ROM, catalog exact variant, model `0x00DE`, firmware,
  macOS version and controller/adapter chain. Exclude every other device/family.
- [ ] Stop DAWs/audio I/O; mute speakers and physically lower monitor/headphone
  levels. Save device configuration externally if the vendor tools permit it.
  Use a maintenance window with no active recording or mounted FireWire storage.
- [ ] Retain the exact known-good signed app/dext installer and version/hash.
  Confirm the app's supported uninstall/reactivation path and a reboot fallback
  before replacing anything. Keep an independent copy off the test build tree.
- [ ] Agree on observability and transaction budgets: one registration attempt,
  no ownership-takeover loop, no routing/mixer/output writes, no streaming changes.

### B. Signing/build gate

- [ ] Follow the repository's current Building-and-Signing/Installing guides
  linked from README, not instructions for a different distribution channel.
  Prefer an appropriately entitled, signed development build using the intended
  team/provisioning profiles. Verify app and nested dext signatures, team IDs,
  bundle ID `net.asfw.driver`, and app/dext entitlement agreement.
- [ ] Check DriverKit, PCI transport, AudioDriverKit and existing required family
  entitlements against `ASFWDriver*.entitlements`; the app needs system-extension
  installation and user-client access. Do not invent an entitlement for this
  mailbox. Do not expand PCI matching for distribution merely to run this test.
- [ ] Verify the architecture slices of the actual build. Current Debug settings
  include arm64e; Release uses arm64/x86_64. Do not assume one successful host
  test proves a dext's ABI or signing compatibility.
- [ ] If an explicitly authorized ad-hoc lab route is chosen, resolve its OS
  security/development-mode prerequisites with the operator first. The current
  driver configuration enables Hardened Runtime; an ad-hoc dext requires a
  deliberately reviewed configuration with Hardened Runtime disabled. Do not
  alter SIP/boot arguments automatically or apply lab instructions to signed
  hosted releases. No signing or security changes were made in Stage 2A.
- [ ] Inspect the signed artifact before install. Install only via the supported
  app path after explicit approval; verify exactly one intended extension is
  active. On startup failure, check DiagnosticReports early rather than treating
  an enumerated but crashed service as a working driver.

### C. Live plumbing and registration gate

- [ ] Review and reserve a collision-free, host/boot-unique notification aperture
  against every existing local handler. Wire the hub through LocalRequestDispatch
  and a dedicated control queue only in the approved test build. Prove queue
  drain/teardown ordering before testing removal; add no automatic activation.
- [ ] Read only the 48-byte static DCP descriptor first; compare it to Stage 0.
  Do not inspect response window `+0x30` speculatively. Abort on identity/layout
  mismatch, unknown source attribution, queue overflows or unexpected ownership.
- [ ] If owner is not the characterized none sentinel, **stop** and report its
  value. Never CAS foreign-to-ours, force-clear, or treat an address match after
  a crash as proof of current ownership.
- [ ] With explicit registration approval, allocate/arm the receiver first, then
  perform one none-to-ours CAS. Capture request operands, returned previous owner,
  route generation and resulting address. This itself is a physical mutation;
  do not describe it as passive/read-only. Stop for review before commands.

### D. First command gate (separate approval)

- [ ] Agree on one verified readable control-store position and expected response;
  use a diagnostic read, not routing/mixer/volume or meter-scaling experiments.
  Account for the engine's existing nonzero-status reset ritual and one retry:
  a logical read can issue more than one request. Abort the experiment on any
  unexpected status; do not expand the request budget on the fly.
- [ ] Capture the request, 16-byte notification and its source/generation, paired
  bounded response-area read, echo/length/counter checks and completion outcome.
  Confirm no response-area reads without a valid in-flight notification.
- [ ] Test stop/reset/unplug only in later separately approved steps, with the
  dependency/queue drain instrumentation in place. Revalidate after rediscovery;
  do not assume that the firmware drops ownership on bus reset.

### E. Abort, rollback and recovery

- [ ] Close command admission, remove the local lease and drain control work.
  Do not issue the unverified release CAS on teardown. Expect a stale owner to
  remain until a verified recovery procedure clears it.
- [ ] Disconnect the test device if transactions misbehave. If ownership is stuck,
  stop the provider and obtain approval to power-cycle the device. A bus reset is
  NOT a proven substitute; do not reset a bus carrying active storage/recording.
- [ ] Use the supported app removal/reactivation flow to restore the saved
  known-good build; reboot if required. Verify installed version, healthy startup
  and ordinary audio behavior before returning the system to recording use.
- [ ] Archive logs, signatures, hashes, descriptor/ownership observations and exact
  transaction count. Document failures as failures; do not mark an assumption
  verified solely because the simulator passed.

## 5. Stop condition

Stop after host-only integration, review and tests. No commit, push, Stage 2 PR,
driver build/install, security-setting changes or live mailbox access is implied
by this report. First physical validation remains a separate authorization gate.
