# Chasing noise on the Thing Plus RA6M5

A record of one day's debugging (2026-09-23): three separate problems that all
presented as "noise", how each was measured, what fixed it, and what didn't.
Written as raw material for a section of the finished article, so it keeps the
dead ends and the reasoning, not just the answers.

The bench: a SparkFun Thing Plus RA6M5 with two RV112FF-40 dual-wiper pots on
A2/A3 and A4/A5 (tracks on VREF/GND), two capacitive touch pads (slices of
carrot) on D20/D21, audio out of the DAC on A0/A1, an SD card streaming audio,
and the onboard WS2812 RGB LED.

The symptoms at the start:

1. **Knob crosstalk** - turning one pot made the other appear to move.
2. **Spurious touch triggers** - SDResampleDemo's transport sometimes latched on
   its own.
3. **General ADC noise** - later narrowed down to audible "crumbs" of scrubbed
   audio with the scrub knob untouched.

## Step 1: build a measuring instrument first

Before trying any fix, we wrote a diagnostic sketch, `NoiseMeter`, for the
Arduino Serial Plotter. The key design decision: **every trace measures an
unwanted signal, so a perfect board plots flat zero on all of them.** The
plotter auto-scales, so as each fix lands the traces shrink and the plotter
zooms in on what's left - you keep chasing until it's flat.

The traces:

- `touchD20`/`touchD21` - each pad's reading minus its tracked "untouched"
  baseline: exactly the number the touch library compares against its
  threshold. Each point is the *largest* excursion since the previous point,
  so a single-scan glitch can't slip between plot updates.
- `noiseA2`-`noiseA5` - RMS jitter of each pot pin over 50ms windows (long
  enough to include 50/60Hz hum).
- `xtalkA2`-`xtalkA5` - while one knob is being turned, how far the *other*
  knob's pins have drifted from where they sat just before. The sketch works
  out which knob is turning by assuming only one ever is (whichever moved
  further, above a threshold), and holds the turning knob's own traces at
  zero - its motion is the signal, not noise.

Features added along the way, each because the lack of it bit us:

- **Summary mode** (the default): one point per second - worst touch and
  crosstalk, true RMS noise. At 20 points a second the plotter's window held
  only a few seconds and the zoom jumped around; at one per second it holds
  about a minute and stays steady, which is what you want for comparing
  before and after.
- **Keyboard-toggled experiments** (`a`-`g`), so each candidate fix and each
  suspected noise source could be switched on and off live, one at a time,
  instead of reflashing for every comparison.
- **A blink of the STAT LED on every accepted keypress.** Without it there was
  no way to tell whether a keypress had arrived when its effect was too small
  to see - and early on, a command really did seem to do nothing (it turned
  out to be a click in the wrong part of the IDE window).
- **Restarting the crosstalk measurement whenever an experiment changes.**
  Toggling a fix shifts every pin's reading slightly; mid-turn, that step was
  being counted as crosstalk. The first scripted A/B run had several rows
  contaminated this way before we noticed.

## Step 2: list the candidate fixes

The advice collected beforehand: 100nF capacitors on the ADC pins, a 10uF
filter on the pot supply, op-amp buffers on the wipers, respecting ADC settling
time, and oversampling. Additions from the discussion: a series-R + C low-pass
at each pin, twisted/short wiring and a separate analog ground return, running
the laptop on battery to rule out mains ground loops, averaging over exactly one
mains period, median filtering, and reading the ADC at "quiet" moments.

One piece of standard advice was a trap for this board: **filtering the pot
supply on its own can make things worse.** When the pots are powered from the
same reference voltage the ADC measures against, supply noise shows up in both
and cancels (the readings are *ratiometric*). Filter only the pot side and that
cancellation breaks. Here the pots were already on VREF, so this was moot - but
it's worth knowing before reaching for a capacitor.

## Step 3: read the datasheet against the Arduino core

The RA6M5 User's Manual has two notes on ADC noise: section 44.3.1 (reducing
interference between the DAC and ADC by synchronizing them) and 43.6.12 (which
pins not to use as digital outputs while converting). Checking what the Arduino
core actually configures turned up more than those two notes:

- **DAC/ADC synchronization is off** - hardcoded in the core, no API. (It turned
  out not to matter; see step 5.)
- **Pin usage is fine** on this board - no pin shared with an analog input is
  toggled as a digital output, except the QSPI flash chip-select, which nothing
  here uses.
- **`analogRead()` converts every analog pin you've ever read, every time.** The
  core adds each pin to the ADC's scan list and never removes it, so every call
  converts A5, A4, A3, A2 in that order (lowest channel number first) and hands
  back only the one you asked for. So the order a sketch reads its pins in is
  irrelevant - inside the converter, A3 always follows A4 and A5 always follows
  A2 (from the previous scan).
- **The ADC's sampling time is left at the chip's reset value: 11 clock cycles,
  220ns.** The manual gives a formula for the time needed (section 43.6.14);
  for a 10k pot, whose wiper can look like 2.5k, it comes out around 0.6us or
  more. A wiper can't fully charge the ADC's shared sample-and-hold capacitor
  in 220ns, so each reading still carries some of the previous pin's voltage.

That also exposed a wrong explanation already in our own code: the RV112FF
library read each pin twice and discarded the first, with a comment saying this
gave the pin extra time to settle. On this core it can't - both reads run the
identical four-pin scan. The workaround had measurably helped, but not for the
stated reason.

Before relying on writing ADC registers directly, we checked (by disassembling
the core's precompiled vendor library) that `analogRead()` wouldn't quietly
overwrite them: it re-runs the ADC's setup on every call, but that setup never
touches the sampling-time or discharge registers.

**Theory, now testable:** the crosstalk should appear on A3 when knob2 turns,
and on A5 when knob1 turns - and not much anywhere else.

## Step 4: measure the crosstalk

First captures with knobs being turned: A3 swung smoothly between about -61 and
+59 counts, once per rotation of knob2; A5 between about -184 and +30 with
knob1; A2 and A4 barely moved. Exactly the predicted pattern, and a steady
position-dependent offset rather than random jitter - the signature of an
undersampled capacitor, not of interference. Around 5% of full scale at worst.

Then a scripted A/B run: a script on the laptop sent the toggle keys over serial
on a 20-second schedule and recorded the output, while a human turned one knob
at a time. Worst crosstalk per phase, with the DAC, SD reads and LED all running:

| Fix | Turning knob1 (A5) | Turning knob2 (A3) |
|---|---|---|
| none | ~280 | ~230 |
| longer sampling time (64 cycles, 1.28us) | 2.5 | 7.8 |
| discharge before each sample | 1.0 | 3.6 |
| both | **1.0** | **1.1** |
| both + the old double read | 1.2 | 1.0 |

Both together bring crosstalk down to the ADC's own noise floor, about 1 count -
a 200x+ improvement. Once both are on, the double read adds nothing.

"Discharge before each sample" is the ADC's *disconnection detection assist*: it
empties the sample-and-hold capacitor before every conversion, so no reading can
inherit charge from the pin before it. Whatever charging shortfall remains then
depends only on the pin's own voltage - a tiny, steady scaling error, which
self-calibrating code like RV112FF absorbs.

**Resulting code:** a new library, `RA6M5AnalogTuning` - one `begin()` call that
applies both settings, and compiles to nothing on any other chip. `RV112FF`
stays chip-independent: its double read became `setSettlingReads(n)` (default
1, the right portable choice), and the RA6M5 sketches set it to 0.

## Step 5: rule out the "lifelike" noise sources

Toggles in NoiseMeter recreated what the real sketches do: the DAC playing a
tone from a 40kHz interrupt (as the audio sketches do), the SD card being read
as fast as possible in 2KB chunks (as SDResampleDemo streams audio), and the RGB
LED lit (it dims by pulsing, so even a steady colour draws pulsed current).

**None of them measurably changed the ADC's resting noise** - about 1 count RMS
throughout. So the DAC/ADC synchronization from the datasheet, while now easy to
switch on, fixes a problem this board didn't show. SD reads raised the touch
pads' idle excursions from about 40 to about 130 counts - still more than 20x
below the trigger threshold, so not the cause of the spurious triggers either.

## Step 6: the touch spikes were a software race

The clue came from watching NoiseMeter: with nothing touched, the pads were
quiet. But **while holding touch2, touch1 showed occasional huge single-sample
spikes** - and more of them the more noise sources were switched on. Eventually
touch2 showed negative spikes too.

That pattern pointed at timing, not electronics. The touch hardware measures
pads one after another (D21 first, then D20), and the library polled it,
noting which channel the hardware was about to measure and crediting the next
result to it. But the hardware doesn't wait for the previous result to be
collected before moving to the next channel. If an interrupt held up the polling
loop at just the wrong moment, the library noted "D20" *before* collecting D21's
result - and filed D21's reading under D20. A second, related race could leave
the last result unread, for the next scan to misfile the other way. With
neither pad touched the two readings are nearly equal, so a swap is invisible;
holding one pad makes it glaringly obvious. More interrupt load means more
chances to hit the window.

**A/B, 15 seconds each, holding touch2 with all noise sources on:**

| | Spikes on touch1 | Dips on touch2 |
|---|---|---|
| old library | 16 at ~+4150 | 6 at ~-4230 |
| fixed library | 0 (stayed within +/-40) | 0 |

The spike and dip sizes matched each other - each was exactly the difference
between the two pads' readings, which is what a swap produces and random noise
wouldn't.

**The fix** was in the bookkeeping, not the hardware: results are matched to pads
by their order in the scan (the Nth result belongs to the Nth pad), any stale
result is discarded before a scan starts, and the loop waits briefly after the
scan ends for a result still owed. Blocking interrupts during the scan would also
have worked, but at the cost of audio glitches.

**And it cured the original complaint:** SDResampleDemo's spurious transport
latching, which had been parked as a hardware problem needing a new PCB, only
ever happened while touch2 was held down. It hasn't recurred.

Worth recording what the earlier analysis of that latching had concluded: the
spurious events were full-strength, and appeared on both pads at once with
nearly identical values - which had looked like a common-mode disturbance on the
shared touch reference, with no software fix possible. A swap of one pad's
reading into the other explains exactly the same observations.

A side finding: **carrot slices make good touch pads until they dry out.** Their
signal fades day by day as their resistance rises. A held touch that no longer
reaches the threshold gets slowly absorbed into the library's baseline, as it's
designed to do with drift. Metal pads (copper-clad board squares with a header
pin soldered on, copper tape on card, coins) don't fade.

## Step 7: the scrub "crumbs"

With crosstalk gone, one artifact remained: occasional tiny crumbs of garbled
audio from SDResampleDemo with the scrub knob untouched - only while the
transport was stopped, and less often than once a second.

The first theory was rare single-sample ADC outliers slipping through the scrub
deadband. **Temporary logging disproved it.** Printing every control tick where
knob2's `rpm()` crossed the 0.5 RPM deadband, plus the largest `rpm()` in each
second, over one hands-off minute:

- 91 crossings - about 1.5 per second, in both directions, spread evenly;
- the per-second maximum typically 0.4-0.8 RPM, once 1.47;
- the reported angle wandering about 0.3 degrees.

Not outliers, then - continuous noise whose peaks sat right at the deadband's
edge. `rpm()` is the change in angle over a single 10ms tick, so 0.3 degrees of
jitter becomes a few RPM before smoothing. Stopped, each crossing nudges the
play position back and forth - audible as a grain of sound. Playing, the same
nudge is a tiny speed wobble, masked by the playback itself.

Options considered:

- **Differentiate over a longer window (50ms instead of 10ms).** Would cut the
  noise about 5x, but adds up to ~25ms of lag to every scrub. **Rejected** - the
  rule for this project's controls is no UI artifact longer than 10ms.
- **Median of 3 readings.** Removes outliers, but there weren't any. Not tried.
- **Hysteresis on the deadband.** Works, costs sensitivity. Held in reserve.
- **Oversampling: average several readings per wiper, taken back to back.** No
  added lag. **Tried: 4 readings.**

Same one-minute log with 4x oversampling: **zero crossings**; the per-second
maximum dropped from a median of 0.52 RPM to 0.20 (worst 1.47 to 0.42); angle
jitter from 0.29 to 0.11 degrees - a 2.6x improvement, a bit better than the 2x
that averaging 4 readings predicts, suggesting the noise was mostly fast and
random. (One caveat: the knob was parked at a different angle for the two runs,
and the angle maths is more sensitive at some positions than others.)

Listening across a full turn in 15-degree steps still turned up a very few
crumbs. Then the **filter capacitors went onto the ADC pins**, and NoiseMeter's
noise traces dropped from around 6 to around 1.5. No crumbs in five minutes of
listening since. The scrub still feels very sensitive, so the deadband stays at
0.5.

The oversampling became a permanent, portable RV112FF feature,
`setOversampling(n)`; SDResampleDemo uses 4 on the scrub knob.

## What worked, and what didn't

| Change | Result |
|---|---|
| Longer ADC sampling time + discharge before sample | Crosstalk 230-280 to ~1 count. The fix. |
| Double read (discard the first) | Helped before the fix; useless after, and slower. Now optional, off here. |
| DAC/ADC synchronization (datasheet 44.3.1) | No measurable effect - the DAC wasn't disturbing the ADC. |
| DAC, SD card, RGB LED as noise sources | No effect on ADC noise; SD mildly raised touch idle noise. |
| Matching touch results by scan order | Cross-pad spikes 22 to 0 in 15s; spurious latching gone. |
| 4x oversampling on the scrub knob | Scrub crossings at rest 91/min to 0. |
| Capacitors from the ADC pins to ground | Noise ~6 to ~1.5 in NoiseMeter; the remaining crumbs gone. |
| Longer differentiation window for rpm() | Rejected: too much lag. |
| Median filtering | Not needed - the noise had no outliers. |
| Filtering the pot supply alone | Not done: would break ratiometric cancellation. |
| Op-amp buffers on the wipers | Not needed after all the above. |

## Lessons worth passing on

- **Build the instrument before the fix.** A plot where perfect means flat zero
  made every improvement - and every regression - visible at a glance.
- **Change one thing at a time, and always know the state you're in.** Several
  captures were useless because nobody knew which toggles were on. A scripted run
  from a known reset state fixed that, and let the laptop do the bookkeeping.
- **The first theory is often wrong.** The throwaway read "worked" for the wrong
  reason; the touch spikes looked like electrical interference but were a
  software race; the crumbs looked like outliers but were steady noise. Each time,
  a measurement designed to tell the theories apart settled it.
- **Look for the symptom's conditions.** The touch bug was invisible unless one
  pad was held, and got worse with interrupt load. Those two conditions pointed
  straight at a timing race between pads.
- **Read what the platform actually does, not what it's supposed to do.** The
  biggest finds came from reading the Arduino core's source against the chip
  manual - and, once, from disassembling a precompiled library.
- **Respect your own constraints.** The textbook fix for derivative noise (a
  longer window) was ruled out by a feel requirement; a cheaper fix that didn't
  touch latency turned up instead.

## Where the code landed

On branches `feature/noise-meter` and `feature/adc-tuning`:

- `NoiseMeter/` - the diagnostic sketch.
- `RA6M5AnalogTuning/` - the ADC fix, as a library that does nothing on other chips.
- `RV112FF` - `setSettlingReads()` and `setOversampling()`, both portable.
- `CapTouch` - the scan-order fix.
- `AudioKnobsDemo`, `AudioKnobsWebBLEDemo`, `SDResampleDemo` - call
  `RA6M5AnalogTuning::begin()` in a marked RA6M5-specific section; SDResampleDemo
  oversamples its scrub knob.
