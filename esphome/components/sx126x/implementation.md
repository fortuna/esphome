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

`RadioOperation` owns readiness and the complete operation lifecycle. The
component supplies preparation, waiting and restoration functions to its
`transmit()` runner. Those functions cannot set readiness, and an early return
from preparation or waiting always returns control to the runner. A restoration
function is a required argument, not a cleanup call that each error branch must
remember. Only this owner can transition to Idle after successful restoration.

Every send exit attempts standby, IRQ clearing, the configured packet parameters
and RX/sleep mode. If restoration fails, clearing BUSY alone is insufficient:
operations remain blocked until explicit configuration succeeds. Configuration
and mode changes use the same owner; reentry during preparation, waiting,
restoration or configuration is rejected. Failed mode changes require
reconfiguration, as does successful cold sleep. Warm sleep retains configuration.

The owner's private scope guard is a fallback for unexpected exits within the
owner itself: it leaves readiness invalid. It performs no SPI or recovery work
in its destructor. Fallible restoration stays explicit in the runner and its
failure takes precedence over the transmit result. The callbacks are templates,
run synchronously, and are never stored; there is no allocation or new thread.
The ESPHome adapter re-enables its loop after successful restoration/configuration,
so reception can resume without another transmission.

## Verification

The GPIO-backed fake chip exercises the real software SPI adapter and checks that
NSS is asserted before BUSY is read, no bytes follow a rejected command, and NSS
is released on all exits. Fault injection covers setup, TX, cleanup and RX.
Recovery tests compare the restored packet parameters and deliver a subsequent
received packet without another send. They also verify that failed restoration
blocks mode changes and packet handling until configuration restores readiness.
Compile-time checks ensure raw SPI methods are not publicly accessible.
Operation tests also verify early returns, reentry rejection and that a send
cannot be invoked without a restoration function. The owner cannot be copied,
so its readiness cannot be duplicated independently of the physical radio.
Host tests and embedded builds do not establish RF or physical-fault behavior.
