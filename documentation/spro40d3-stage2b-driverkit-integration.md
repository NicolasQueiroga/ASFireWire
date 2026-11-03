# FW-203 Stage 2B — gated DriverKit integration preparation

## Scope and stop gate

Stage 2B is stacked on FW-203 Stage 2A. It compiles a production-compatible
integration path, but it remains **disabled by default** and no live mailbox
operation is reachable through device discovery, the control provider, or the
UserClient.

This stage performed **no physical FireWire transaction, mailbox ownership CAS,
notification registration with a device, mixer/routing write, driver install or
replacement, signing change, SIP change, or audio-installation modification**.
All bus behavior below was exercised only by host mocks. Do not treat a build or
host test as evidence of DriverKit runtime or firmware compatibility.

## Production architecture

```text
DriverWiring::EnsureSbp2Deps (service lifetime)
  SPro40D3DriverKitManager
    dedicated IODispatchQueue: com.asfw.dice.spro40d3.control
    SPro40D3NotificationHub (ingress copies only)
    SPro40D3NotificationEndpoint
      AddressSpaceManager (controller-lifetime reserved software range)
      LocalRequestDispatch -> Sbp2LocalHandler -> provenance callback
    cached diagnostic snapshot / hard-deny activation policy

Future separately-authorized control job (not enabled in Stage 2B)
  SPro40D3Session + SPro40D3MailboxEngine
    SPro40D3Transport -> ProtocolRegisterIO
      existing async bus transaction, route, speed and cancellation policy
```

`SPro40D3Transport` now uses the canonical `ProtocolRegisterIO` seam rather
than operating the raw bus port itself. The new
`CompareSwap64DcpOwner`/raw-response operation preserves the mailbox owner word
as **low BE32 followed by high BE32** for expected, replacement, and returned
owner. It deliberately does not use `CompareSwap64BE`. The transport remains
bounded, single-submit, no-retry, route-validated, and cancellation-safe; its
shared completion state survives a late completion without retaining an adapter
or caller span.

The blocking mailbox seam is reserved for the dedicated control queue only.
Bus completions remain on ASFW's existing completion path. The manager currently
has no active session, so its timer/drain work is intentionally not armed; a
future activation must schedule a deferred tick onto this queue even when no
notification arrives, so engine deadlines can expire without ingress.

### Inbound notification destination

ASFW already has a real receive path for device-originated async writes:
OHCI request reception -> `PacketRouter` -> `LocalRequestDispatch` ->
`Sbp2LocalHandler` -> `AddressSpaceManager`. Stage 2B reuses this path; it does
not manufacture an aperture or add a parallel packet router.

`SPro40D3NotificationEndpoint` reserves one 16-byte, 16-byte-aligned software
range through the existing controller-lifetime `AddressSpaceManager` auto
allocator, explicitly requesting **16-byte alignment**. The allocator retains
its existing 8-byte default/minimum for other consumers; its optional final
`alignment` argument accepts powers of two at least 8. First-fit aligns both
the initial candidate and each candidate after an occupied range, then checks
the entire requested length against the gap/window before allocating. Invalid
alignment or exhaustion leaves output arguments and reservations unchanged;
alignment padding is not reserved. That allocator provides collision exclusion against all of its other
ranges. The endpoint binds a hub lease to exactly that returned address. The
range is **never deallocated** during manager lifetime, including after reset,
removal, stop, or endpoint destruction. This prevents a late mailbox write from
being delivered to a future owner. Its owner token is the address manager itself,
not the short-lived endpoint pointer, so unrelated owner cleanup cannot reclaim
a tombstone when malloc reuses a destroyed endpoint's storage. The callback is weakly owned, runs
outside the address-manager lock, and returns `AddressError` after retirement
or destruction.

The dynamic callback receives destination, source node, generation, tCode and
payload. The hub accepts only the active exact address, full expected source ID,
current route generation, block-write tCode, and 16-byte payload. It copies into
its bounded FIFO; it never calls an engine from bus ingress. Other ranges are
not claimed by the hub.

The current allocator's automatic window is controller-local
`0xFFFF:0010_0000..0xFFFF:0FFF_FFFF`, outside the hardware physical-response
domain and separate from the fixed CSR/FCP/DICE/MOTU/Fireworks handlers. This
is a concrete controller-lifetime allocation strategy, not a claim of cross-host
or firmware-level address uniqueness. Firmware interpretation of this address,
host-node address composition, and exact live frame form remain hardware gates.

## Lifecycle and activation boundary

The manager is constructed from `DriverWiring::EnsureSbp2Deps`, before local
request dispatch installation, and is retained in both `ServiceContext` and
`ControllerCore`. Diagnostics take a lock-protected shared lifetime snapshot;
device removal uses a copied callback with a weak manager reference that is
promoted for the duration of the call. The manager serializes lifecycle and
cached-registry access with its own lock, and no longer accesses its registry
after `Stop()` returns, even if a diagnostic retains it through service teardown.
Endpoint binding, retirement and ingress are linearized under one endpoint
lock, with lock order endpoint -> hub; the address-manager lock is released
before invoking either. It reserves a local range and creates a dedicated queue, but
does **not** create a session, read a descriptor, bind a route, register an
owner, or send a transaction.

- Bus reset/removal synchronously revokes ingress on the control queue before
  lifecycle handling proceeds.
- Device removal is fanned into the manager before AudioCoordinator retires its
  runtime state. The dormant manager only revokes ingress; a future active
  session must call terminal `DeviceRemoved()` on this queue and never send an
  ownership-release CAS.
- Service reset first closes the manager, drains its queue, removes its
  controller/audio references, then releases the address manager, registry and
  async subsystem. The retained local address dies only with the address manager
  at full controller teardown.

The existing generic audio factory/catalog remains unchanged: there is no
`IDeviceProtocol` factory case, no automatic provider activation, no controls
published through `IControlProvider`, and no streaming change. Exact TCD3070
admission remains the Stage 2A session rule: the catalog's
`FocusriteSPro40Tcd3070` identity (including the `0x00DE`/GUID-product
exception), DICE family, required interface characteristics, current route and
validated descriptor must all pass before any future ownership step. Original
TCD2220 Saffire Pro 40 and other DICE devices are not admitted.

### Diagnostic authorization ladder

`DiagnosticScope` distinguishes, without implication between levels:

1. passive descriptor evidence;
2. notification ownership registration;
3. read-only mailbox commands;
4. mixer/routing writes.

`SPro40D3DriverKitManager::RequestActivation` is an executable hard deny for
every scope in Stage 2B. `ASFW_SPRO40D3_EXPERIMENTAL_ACTIVATION=0` is set in
both project configurations; changing that macro alone is deliberately
insufficient because no privileged UserClient activation entry exists.

Selector **1018** returns `ASFWDiagSPro40D3Mailbox`, a cached-only snapshot of
discovery identity, route generation/node, staged local notification allocation,
activation/session state, last local status, and safe counters. It never calls
`ReadDcpDescriptor`, allocates a hub lease, submits a transaction, performs a
CAS, or exposes mixer/routing controls. Descriptor/firmware/owner values remain
unavailable unless a future separately authorized experiment records them; zero
is not evidence of a device value.

## Build and signing findings

`project.yml` now includes the SPro40D3 sources in the dext target so production
compilation can catch DriverKit API/lifetime errors. The integration remains
runtime-disabled as above. XcodeGen must regenerate `ASFW.xcodeproj` whenever
these source files change.

The existing signing split is retained:

- Dext: automatic signing for team `F6YA6B56LR`, with DriverKit, PCI transport,
  AudioDriverKit, MIDI and SCSI family entitlements in
  `ASFWDriver.entitlements`; Debug only broadens the PCI matcher.
- App Release: sandboxed, with audio input, system-extension installation, and
  `net.asfw.driver` UserClient access. It has no sandbox network entitlement.
- App Debug: sandbox disabled, but this does not replace a properly provisioned
  app+dext pair for DriverKit installation.

`security find-identity -v -p codesigning` reports **0 valid identities** in
this environment. Signed build, dext install/load, UserClient entitlement
verification, and a Release MCP listener are therefore blocked. A supported
future development workflow requires an Apple Development/Developer ID signing
identity and matching DriverKit/system-extension provisioning for the app and
dext separately. If a sandboxed Release app serves MCP over the network, make a
product decision and add only the appropriate app sandbox networking entitlement;
do not add network entitlement to the dext. Do not disable SIP or use signing
bypasses.

## Host validation

**Full Release host regression:** 4072 registered tests, **4066 passed**, six
pre-existing fixture/descriptor skips, **zero failures**. The focused transport,
notification, manager and session suite accounts for 86 passing tests. Including
the unchanged Stage 1 engine and codec, **152/152 mailbox tests pass under
ASan+UBSan**; the same sanitizer run also passes **14/14 allocator tests**
(**166/166 total**), with no sanitizer report:

- canonical transport: low-quadlet CAS encoding, exact response shape,
  route loss, cancellation and late completion lifetime;
- notification hub: ingress/source/generation/tCode/length validation and
  bounded FIFO isolation;
- notification endpoint: manager collision avoidance, retirement/no reuse,
  post-destruction callback safety;
- manager: dormant startup, denial of every authorization scope, concurrent
  snapshot/reset/stop, and a retained stopped manager after dependency destruction;
- existing session lifecycle regression suite.

XcodeGen regenerated `ASFW.xcodeproj` (44 expected source-registration lines),
and repeat generation produced the same project hash. Both Debug and Release
`ASFWDriver` DriverKit targets built successfully with `CODE_SIGNING_ALLOWED=NO`
(Debug: `x86_64`/`arm64e`; Release: `x86_64`/`arm64`). Xcode also reported
four existing static-analyzer issues in `PostResetTimingCoordinator.cpp` and
`ASFWAvcAudioStream.cpp`, plus a size-conversion warning in the unchanged Stage 1
`SPro40D3MailboxTypes.hpp:362`. These are recorded, not silently fixed in this
integration delta. This is a compilation check only, not signing, installation,
loading, or live runtime validation.

Suggested host-only commands:

```sh
./bump.sh refresh
cmake -S tests -B build/fw203-stage2b-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/fw203-stage2b-tests --target \
  SPro40D3TransportTests SPro40D3NotificationHubTests \
  SPro40D3NotificationEndpointTests SPro40D3DriverKitManagerTests \
  SPro40D3SessionTests
ctest --test-dir build/fw203-stage2b-tests --output-on-failure \
  -R 'SPro40D3(Transport|NotificationHub|NotificationEndpoint|DriverKitManager|Session)'
xcodegen generate
xcodebuild -project ASFW.xcodeproj -target ASFWDriver -configuration Debug \
  -sdk driverkit CODE_SIGNING_ALLOWED=NO build
```

Final logs are retained in the ignored `build/fw203-stage2b-validation/`
directory. Sanitizer configuration uses `-DASFW_ENABLE_ASAN=ON` and builds all
seven `SPro40D3*Tests` targets before `ctest -R SPro40D3`. The concurrent tests
exercise retirement and teardown interleavings; ASan/UBSan do not constitute
a ThreadSanitizer proof or a DriverKit dispatch scheduling test.

### Final-review alignment correction

The original auto allocator guaranteed only 8-byte alignment, which was
insufficient for the hub's 16-byte external-lease requirement. The endpoint now
requests the stricter alignment directly through the existing API; there is no
over-allocation, unusable padding reservation, or change to retirement policy.
This correction does not change activation or control-queue behavior.

Four additional regression tests cover:

- an 8-byte predecessor, interleaved endpoint/legacy consumers, reuse of padding
  by legacy consumers, and exclusion of retired notification ranges;
- collision skipping with explicit 16-byte alignment and unchanged 8-byte default;
- invalid alignment and alignment rounding past the window, with unchanged
  outputs, first address and next handle after failure;
- two separated 8-byte holes, full-window exhaustion, failed endpoint creation,
  recovery to the last aligned 16-byte gap, successful hub binding, and retirement.

The endpoint exhaustion fixture occupies the real allocation window except its
last 40 bytes; it does not shrink or alter the production allocator for testing.
Affected allocator/hub/endpoint tests: **35/35 passed**. Full host regression,
mailbox-plus-allocator ASan+UBSan, and unsigned Debug/Release DriverKit builds
were rerun after the correction. Correction logs are retained separately in
`build/fw203-stage2b-alignment-validation/`.

Source safety review found and addressed raw manager lifetime escapes in
diagnostics/removal, endpoint lease synchronization, and delayed ingress
revocation. A full host build also caught an AudioCoordinator-to-mailbox link
dependency; service wiring now supplies a weakly owned removal callback instead.
A bounded follow-up review found no blockers for the current dormant deny-all
configuration. Queue/hub internals are private. Before any future session work,
redesign or explicitly enforce the lifecycle queue contract: current lifecycle
methods hold the manager lock while synchronously dispatching to its empty
control queue. Calling them from that queue, or adding work on that queue that
acquires the same lock, would deadlock. This stage installs no such work.

## First live experiment — proposal only

This is not authorization to act. Require a separate explicit approval for each
checkpoint and stop on unexpected evidence.

1. Preserve the working signed Release app/dext installer and record its app,
   dext, system-extension, macOS, controller, device GUID/ROM, and audio setup
   versions. Verify a known-good rollback installer before replacement.
2. Verify provisioned app+dext signatures, entitlements and architecture slices;
   confirm a sanctioned UserClient diagnostic path. Do not rely on ad-hoc Debug
   behavior, an Apple ID alone, or an entitlement bypass.
3. With audio idle, routing/mixer configuration backed up, monitor levels down,
   no mounted FireWire storage, and logs prepared, authorize exactly one
   scope: first cached identity, then passive descriptor read. Stop on identity,
   descriptor, firmware, or transport mismatch.
4. Before ownership registration, verify the dynamic local address reservation,
   source/generation validation instrumentation, timeout control tick and a
   foreign-owner readout. Any owner other than the protocol's exact none
   sentinel is a stop condition; never
   take over and never attempt unverified release CAS.
5. Authorize at most one registration experiment only after a reviewed recovery
   plan. No control reads or writes are implied. On reset/power-cycle, treat the
   session as invalid, preserve the retained address policy, rediscover, and
   re-evaluate from the first gate.
6. If extension activation fails, use the recorded rollback installer or the
   app's normal system-extension recovery path, restart the known-good audio
   setup, and inspect for an orphaned owner before attempting any later test.
   Never repair an owner with an unverified CAS.

## Remaining blockers and readiness

Hardware-unverified: firmware acceptance of the allocated host address; exact
notification framing/source behavior; real DriverKit queue timing; local address
reservation over resets/sleep/wake; signed service startup; descriptor and
ownership semantics; ownership retention/release; and all mailbox/mixer effects.
The environment also lacks a signing identity.

**Stage 2B readiness: ready for source review only.** The integration compiles,
is host-tested, and has an explicit disabled default. It is not ready for driver
deployment or a physical mailbox experiment without the approval checkpoints
above and resolution of the listed blockers.
