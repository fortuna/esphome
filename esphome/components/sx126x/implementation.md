# SX126x command transport

This note describes implementation boundaries for driver maintainers. User-facing
configuration belongs in esphome.io; C++ borrowing and completion contracts belong
beside the public declarations in sx126x.h.

## One command, one selected interval

`SpiCommandPort` owns the SPI device privately. Its `read()` and `write()` methods
execute complete command frames. The component cannot call raw SPI transfers,
assert chip select, or bypass the BUSY check through inheritance. ESPHome's bus
configuration setters are forwarded explicitly; direct SPIDevice casts and raw
SPI calls on an SX126x instance are intentionally no longer supported.

Each exchange constructs a noncopyable selection guard, asserts NSS, then waits
for BUSY to clear. Asserting NSS first wakes a sleeping chip. No opcode, payload,
or dummy byte is clocked if the wait fails. Scope exit releases NSS on both
success and timeout. Writes apply their switching delay after release. The
20 ms BUSY limit and SPI transfers remain synchronous; the port never holds the
shared SPI bus for packet airtime. Input/output spans are borrowed only until the
call returns. The port performs no payload allocation and invokes no callbacks.

Only the port's exchange implementation can access the SPI device. Command
encoders supply header and payload bytes, while the component translates failure
into ESPHome warnings. A completed exchange means bytes were transferred; it is
not confirmation that the chip completed a radio operation.

## Failure and configuration

Commands abort on BUSY timeout. Initial configuration failure permanently fails
setup. Runtime reconfiguration invalidates chip readiness before reset; normal
operations remain blocked until the complete configuration succeeds. Synchronous
sends clear stale IRQ flags before starting TX. These rules carry forward the
regressions from the earlier BUSY-fix proposal.

Transmission temporarily invalidates readiness before changing mode or packet
length. Every send exit attempts standby, IRQ clearing, the configured packet
parameters and the configured RX/sleep mode, including failed preparation. Only
complete restoration makes the radio ready again. If restoration fails, clearing
BUSY alone is insufficient: operations remain blocked and the warning remains
until an explicit configuration succeeds. Successful restoration or configuration
re-enables the loop, so receiving can resume without another transmission.

## Verification

The GPIO-backed fake chip exercises the real software SPI adapter and checks that
NSS is asserted before BUSY is read, no bytes follow a rejected command, and NSS
is released on all exits. Fault injection covers setup, TX, cleanup and RX.
Recovery tests compare the restored packet parameters and deliver a subsequent
received packet without another send. They also verify that failed restoration
blocks mode changes and packet handling until configuration restores readiness.
Compile-time checks ensure raw SPI methods are not publicly accessible.
Host tests and embedded builds do not establish RF or physical-fault behavior.

## Radio vocabulary and applied configuration

`Sx126xCommands` encodes opcodes, register addresses, dummy bytes, packet parameters
and RX metadata. It sees only complete exchanges on `SpiCommandPort`; it has no
component lifecycle, warnings, callbacks or raw SPI access. Command results are
marked `nodiscard`, and configuration distinguishes BUSY from unsupported silicon.

`SX126x` owns reset, component status and the requested/applied `RadioProfile`
values. Setters stage the requested profile. Reset invalidates readiness; the
applied snapshot changes only after every configuration command succeeds.
Sends, RX interpretation and restoration use the applied snapshot, even if a
setter has staged a different payload length or RX policy. Sync words use eight
fixed bytes per profile; snapshots require no allocation. An oversized sync word
is rejected before reset. Cold sleep invalidates configuration, while warm sleep
retains it. A successful explicit configure is required after cold sleep.

The separation allows configuration encoders to be tested without ESPHome
lifecycle policy, and lifecycle tests to assert that incomplete configuration
never permits normal operations. Existing YAML and send/mode entry points remain
available; runtime setters now explicitly stage changes until configure succeeds.
