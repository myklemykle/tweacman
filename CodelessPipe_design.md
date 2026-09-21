# CodelessPipe - design proposal (not yet built)

A proposal for wrapping everything learned while debugging the DA14531's
Binary mode / DSPS link into a reusable Arduino-compatible library, so that
neither we nor a hobbyist reading these demos has to rediscover it.

Status: **proposal only.** Nothing here is implemented. Written 2026-09-21,
after fixing `AudioKnobsWebBLEDemo` (commits `c8c77f8`, `badf1cd`, `e36c48b`,
`53ca87e` on `feature/audio-knobs-webble`). See the
`codeless-binary-mode-dsps-protocol` memory for the measured protocol facts
this rests on.

## Why a library

Three sketches on this branch touch the CodeLess link, and each shows a
different reason the knowledge needs packaging:

- `AudioKnobsWebBLEDemo` - full binary-mode consumer, now fixed.
- `SDResampleDemo` - a second binary-mode consumer with a richer protocol
  (`FILES:BEGIN`, `PLAYING:`, `ERR:`), still on the old broken model.
- `HelloWorldBLE` - never uses Binary mode at all, but has a *latent* form of
  the same bug: flash it after either of the above and the module is still
  wedged in Binary mode, so its `ATR`/`AT+ADVSTART` are swallowed as pipe data
  and it silently does nothing.

That third case is the strongest argument. The failure is not confined to
sketches that use the feature.

## What the library must own

None of this is discoverable from Renesas' documentation; all of it was
measured on the wire.

1. Boot-time escape (`+++`, guard times, then `ATR`) - Binary mode survives an
   MCU reset, because the DA14531 is a separate coprocessor.
2. Approving an unsolicited `+BINREQ` with `AT+BINREQACK`.
3. Not treating the deferred (or apparently absent) `OK` as a failure - it is
   held until the mode transition completes at disconnect.
4. Echo-probing to detect the actual mode, because once in Binary mode the
   module emits no status lines at all.
5. Splitting `readString()` bursts into individual lines before dispatch.
6. Setting the UART timeout to ~20ms; Arduino's 1000ms default stalls the
   sketch's control loop on every round trip.
7. The grant landing at disconnect, so the *central* must reconnect.
8. Browser side: serialising GATT operations, the notify-then-read doorbell on
   AT Outbound, flow control `0x01`/`0x02`, and `writeValue()` vs
   `writeValueWithoutResponse()` (Bluefy silently drops the latter).

## Proposed shape

### Two halves, shipped together

The central design claim. Items 7 and 8 live in the *browser*, not the sketch.
An Arduino-only library would leave a hobbyist with a board that negotiates
perfectly and a page that never receives anything - precisely the failure mode
we just spent a session on. So:

```
CodelessPipe/
  library.properties
  keywords.txt
  src/CodelessPipe.h / .cpp
  web/codeless-pipe.js        <- companion ES module, documented as a pair
  examples/EchoPipe/          <- board echoes back whatever the page sends
```

### Make it look like `Serial`

Subclass `Stream`, so `print()`, `println()`, `available()`, `read()` and
`readStringUntil()` come free and demo code reads like ordinary Arduino:

```cpp
CodelessPipe pipe(Serial2);

void setup() { pipe.begin(); }

void loop() {
  pipe.poll();                          // non-blocking; call often
  if (pipe.isOpen()) pipe.println("F1:440");
  while (pipe.available()) { /* ... */ }
}
```

The JS half mirrors it - `await pipe.connect()`, `pipe.onLine = ...`,
`pipe.send(...)` - with the GATT queue and the reconnect hidden inside, plus a
`log` hook so a page can display the link trace.

### Drop the SparkFun dependency

`CodelessBase::sendCommand()` causes half the trouble: it blocks for up to 2s,
uses Arduino's 1000ms default Stream timeout, and discards every reply. It is
also manually installed and absent from Library Manager, which is a real
distribution problem for hobbyists. Replacing it with a small non-blocking AT
state machine is perhaps 200 lines and removes the dependency entirely. This
is the bulk of the implementation cost and is judged worth it.

## Open decisions

1. **Probe payload.** Today the probe is the sketch's own telemetry line, which
   only works if it happens to be transmitting; a library cannot assume that.
   Proposed default: a lone `"\n"`, with the JS side ignoring blank lines. The
   cost is that the library injects bytes into the user's stream, which some
   protocols will not tolerate. Make it configurable.

2. **Can the transition be forced?** The grant lands at disconnect. We never
   tested whether the *sketch* can hurry it - if something like
   `AT+DISCONNECT` works right after the ACK, the mode flips immediately and
   the central simply reconnects into a working pipe, turning an awkward
   "wait and probe" into a crisp handshake. Unknown; worth an experiment
   before freezing the API, because it changes the design.

3. **`Stream` or a narrower API?** `Stream` is idiomatic and composes with the
   ecosystem, but carries blocking-read semantics the implementation would be
   deliberately subverting. A smaller `sendLine()`/`onLine()` surface is more
   honest about being message-oriented rather than byte-oriented.

4. **Keep the wire trace?** Promote `BLE_TRACE` from a compile-time define to
   `pipe.setTrace(Serial)`. That diagnostic is the only reason this was
   solvable at all, and hobbyists will hit module-state weirdness we have not
   seen.

## Risks

Almost all of this is empirical - one module, one firmware version. A library
enshrines behaviour Renesas never documented and could change. The mitigating
factor is that the echo probe *measures* rather than assumes, so the fragile
parts degrade into "keeps probing" rather than "silently broken".

## Sequencing

Port the fixes into `SDResampleDemo` and `HelloWorldBLE` as a straight copy
first, then extract the library from three working call sites. Designing an
API from a single consumer tends to bake in that consumer's assumptions;
`SDResampleDemo`'s file-listing protocol and `HelloWorldBLE`'s escape-only use
are exactly the pressure the design needs.
