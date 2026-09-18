/*
  SDResampleDemo - SparkFun Thing Plus RA6M5

  Streams a WAV file off the onboard SD card and plays it out the DAC,
  with a pot controlling a completely smooth, arbitrary playback-speed
  ratio - not discrete multiples, and slowing down genuinely interpolates
  new sample values rather than just repeating or stepping through
  existing ones. A phone/browser can also pick which file plays and
  watch the same live stats over BLE - see "BLE control" below.

  Wiring: an SD card (FAT32, any SDHC card - see the SD note in
  CLAUDE.md, this board's slot is wired to the RA6M5's native SDHI
  peripheral, not SPI) with one or more WAV files at its root - use
  tools/make_test_wav.py to generate a simple one, a slow sine sweep
  that makes speed changes and any resampling artifacts obvious by ear.
  Otherwise same as DACAudioTest (3.5mm jack on A0/A1/GND) plus one pot
  (RV112FF wipers on A2/A3, track ends on 3.3V/GND) and one CapTouch pad
  (D20, with the shared TSCAP capacitor on D6 - see CapTouch.h). The
  DA14531MOD Bluetooth module used for BLE control is already onboard -
  no extra wiring for that part.

  Why the DAC runs at a fixed 48kHz: an earlier version of this idea
  just changed the DAC's own output clock to change speed, tape-style -
  simple, but it directly couples "how fast can we go" to the DAC
  hardware's own conversion-time ceiling (~333kHz per the RA6M5
  datasheet, Table 53.48), which isn't the interesting limit here. This
  version fixes the DAC at a constant, comfortable 48kHz always, and
  does real-time resampling from whatever rate the SD card is actually
  being asked to supply. That decouples the speed knob from the DAC
  entirely and points it at the thing this demo is actually about: SD
  read throughput (the RA6M5's SDHI peripheral has a theoretical ~25MB/s
  ceiling per the datasheet's Table 53.34). Turn the speed ratio up far
  enough and the SD reader in loop() should eventually fail to keep the
  ring buffer fed before the DAC or CPU ever struggle - see "underrun"
  below.

  Measured on the first bench card (a plain microSD, brand/speed class
  unrecorded): underruns start right around speed ratio ~23-25x with
  this sketch's default settings, regardless of whether SD reads happen
  in 2KB or 32KB chunks and regardless of the ring buffer's size - both
  were tried and neither moved the wall, which rules out per-fread()-
  call overhead as the cause. That points at ~2-2.2MB/s as this card's
  real sustained sequential read speed - a small fraction of the SDHI
  bus's theoretical ceiling above, and apparently the card's own limit,
  not this sketch's. Worth re-measuring with a different/faster card.

  How the resampling works: RingBuffer.h holds raw samples read from SD;
  Resampler.h tracks a fractional read position into that buffer and
  advances it by a continuously-variable amount (`step`) once per
  48kHz tick, interpolating a fresh output value with a 4-point
  Catmull-Rom curve each time. The same mechanism handles speeding up
  (step > 1 - skip some source samples, but still interpolate between
  whichever two straddle the new position) and slowing down (step < 1 -
  the read position stays inside the same 4 samples across several
  output ticks, each time interpolating a genuinely new in-between
  value) with no separate code path for either direction. See both of
  those files' own comments for more detail.

  Aliasing at high speed ratios is expected, not a bug: no filter tracks
  the ratio to prevent high source frequencies folding down as `step`
  grows past ~2. A proper fix needs a cutoff that tracks the live ratio
  - real DSP scope this hand-rolled kernel deliberately skips. It's also
  a useful signpost, since it shows up audibly before the harder
  SD-bandwidth wall.

  Underrun policy: if the ring buffer can't supply what the resampler
  needs, the DAC ISR holds the last good sample rather than dropping to
  silence - that's what a real buffer-starved device actually sounds
  like (a stuck stutter), and it avoids an extra click on both entering
  and leaving the underrun. The resampler's position is frozen too, so
  playback resumes exactly where it left off with no skipped content.
  underrunTicks (printed periodically over Serial, along with live ring
  buffer occupancy) is this demo's actual headline measurement - watch
  it, and the occupancy trending toward zero, as you turn the ratio up.

  BLE control: connect with web/wav-remote.html (self-hosted with real
  HTTPS - can't be a Claude Artifact, see CLAUDE.md's Bluetooth section)
  to list the WAV files on the card, pick one, and watch the same speed/
  buffer/underrun stats live instead of over USB Serial. Reuses the
  CodeLess Binary mode/DSPS bidirectional protocol already validated in
  AudioKnobsWebBLEDemo - see that sketch and the codeless-binary-mode-
  dsps-protocol memory for the handshake itself. On top of that
  transport, this sketch speaks a small plain-text line protocol:
  `LIST` requests a `FILES:BEGIN`/one-name-per-line/`FILES:END` block;
  `PLAY:<name>` requests a switch, answered with `PLAYING:<name>` on
  success or `ERR:<name>:<reason>` on failure - a bad or unsupported
  file is a normal, reported failure, not a hang. This demo only plays
  mono 16-bit PCM; picking anything else from the pile of real-world
  samples (stereo, 8/24-bit, float, WAVE_FORMAT_EXTENSIBLE - all present
  on a real sample library) will cleanly error via BLE until WavReader
  grows support for them - a deliberately separate, not-yet-done task.

  Nothing is loaded or played until a BLE PLAY: command asks for it -
  the board boots silent, with no SD reads happening in loop() at all.
  This isn't just a UX choice: while chasing a real bug where inbound
  BLE commands never reached the board at all despite a confirmed-clean
  CodeLess Binary mode handshake (ruled out with a from-scratch bleak
  script, independent of the web page or browser entirely), the prime
  remaining suspect was loop()'s own greedy, unthrottled SD refill -
  the one thing structurally different from AudioKnobsWebBLEDemo (whose
  BLE inbound path is independently confirmed working), which never
  touches SD at all. A `wav.readSamples()` call, however brief, taking
  long enough to let Serial2's RX buffer overflow before the next
  ble.available() check would silently drop exactly this kind of small,
  rare inbound write. Not touching SD until a file is actually wanted
  removes that variable during the always-important connect/LIST moment.

  Switching files: audioCallback() runs on a hardware interrupt totally
  independent of loop(), so swapping the WavReader/ring buffer/resampler
  state under it isn't safe to do casually. loadFile() below validates
  a candidate file completely (into a throwaway WavReader) before
  touching anything shared - a bad PLAY: request never disturbs whatever
  is already playing - then masks just the audio timer's own interrupt
  vector (FspTimer::disable_overflow_irq(), confirmed to touch nothing
  but that one NVIC vector) for the brief swap, rather than a checked
  flag the ISR has to branch around. The DAC output freezes for that
  handful of milliseconds - a deliberate, acceptable click on switch,
  not a bug.

  How to test: bring this up in stages rather than expecting it to work
  end-to-end on the first try.
    1. Flash the stock Storage/examples/TestSDCARD sketch from the core
       first, unmodified, to confirm the card mounts and lists files at
       all before any of this sketch's code is involved.
    2. With this sketch, confirm the board boots silent and idle - it no
       longer auto-loads anything at boot (see "BLE control" below for
       why); it just sits there doing no SD reads at all until a BLE
       PLAY: command picks a file. A WAV file not being exactly 48000Hz
       already means the resampler is doing real, non-trivial
       interpolation from the very first sample, even before the knob
       is touched.
    3. Turn the pot slowly across its full range once a file is playing;
       confirm smooth, continuous pitch/tempo tracking with no
       zippering or clicks, symmetric behavior above/below 1x, and that
       touching the pad switches between fast/coarse and slow/fine
       sweep rates.
    4. Push the ratio hard enough to see the ring buffer's occupancy
       fall and underrunTicks start climbing - that onset point is a
       real, card-dependent number (~23-25x measured so far - see
       above) worth re-checking against whatever card is on the bench.
    5. Open web/wav-remote.html, connect, confirm LIST populates real
       filenames from the card, PLAY: on a mono-16-bit file switches
       cleanly, and PLAY: on anything else returns a clean ERR: instead
       of hanging.

  This example code is in the public domain.
*/

#include <RV112FF.h>
#include <MultiTurnKnob.h>
#include <CapTouch.h>
#include <codelessBLEPeripheral.h>
#include "SDCardBlockDevice.h"
#include "FATFileSystem.h"
#include "bsp_api.h"   // for R_DAC - the audio ISR writes it directly, same as DACAudioTest
#include "FspTimer.h"  // for the audio-rate interrupt driving that ISR
#include "RingBuffer.h"
#include "Resampler.h"
#include "WavReader.h"
#include <utility>  // for std::move in loadFile()

// -- Speed range and knob mapping ----------------------------------------
// Same shape as AudioKnobsDemo's angleToFrequency()/knobAngle(), just
// retargeted from a pitch range onto a speed-ratio range - see that
// sketch's header comment for why the exponential mapping and the
// fold-at-180-degrees behavior feel right for a knob like this.

const float MIN_RATIO = 0.1f;
const float MAX_RATIO = 128.0f;  // deliberately probing well past the SD ceiling

float angleToSpeedRatio(float angleDegrees) {
  float foldedAngle = (angleDegrees <= 180.0f) ? angleDegrees : (360.0f - angleDegrees);
  float t = foldedAngle / 180.0f;
  return MIN_RATIO * powf(MAX_RATIO / MIN_RATIO, t);
}

const float TURNS_PER_SWEEP_FAST = 0.5f;
const float TURNS_PER_SWEEP_SLOW = 2.0f;

float knobAngle(float accumulatedDegrees, float turnsPerSweep) {
  if (accumulatedDegrees == -1.0f) return -1;
  float wrapped = fmodf(accumulatedDegrees / (turnsPerSweep * 2.0f), 360.0f);
  if (wrapped < 0) wrapped += 360.0f;
  return wrapped;
}

// -- SD card and WAV source -----------------------------------------------
// Declared at file scope, not inside setup(): FATFileSystem keeps a
// reference to the block device for as long as any file on it stays
// open, which for this sketch is the entire runtime.

SDCardBlockDevice sdBlockDevice(PIN_SDHI_CLK, PIN_SDHI_CMD, PIN_SDHI_D0, PIN_SDHI_D1,
                                PIN_SDHI_D2, PIN_SDHI_D3, PIN_SDHI_CD, PIN_SDHI_WP);
FATFileSystem sdFs("sd");
WavReader wav;

bool fileLoaded = false;
String currentFilename = "NONE";
float rateFactor = 1.0f;  // wav's native sample rate / 48000 - recomputed on every file load

// -- Ring buffer and resampler ---------------------------------------------

const uint32_t RING_CAPACITY = 16384;  // ~370ms of headroom at a 44.1kHz source
AudioRingBuffer<RING_CAPACITY> ring;
Resampler resampler;

// Written by loop() at control rate, read by the audio interrupt every
// tick - a plain float read/write is atomic on this 32-bit core, the
// same "no lock needed for a single scalar" case AudioKnobsDemo's
// freq/gain pointers rely on (see RingBuffer.h's comment for why the
// ring buffer itself is a different, larger problem that does need
// real atomics).
volatile float currentStep = 1.0f;
volatile int16_t lastGoodSample = 0;
volatile uint32_t underrunTicks = 0;

// The knob-requested ratio as of the last control-rate tick, kept
// outside that block too: loadFile() runs on its own schedule (whenever
// a BLE PLAY: command arrives, not at CONTROL_INTERVAL), and needs this
// to recompute currentStep for the newly-loaded file's rateFactor
// without waiting for the next knob tick.
float lastKnownRatio = 1.0f;

// Diagnostic only: the min/max ratio actually computed at control rate
// since the last status print, to check whether currentStep is
// spiking between prints (which only show a single 500ms snapshot) or
// whether the buffer wobble seen at "steady" knob positions has some
// other cause entirely.
float minRatioSeen = 1e9f;
float maxRatioSeen = -1e9f;

// -- Audio-rate interrupt ---------------------------------------------

FspTimer audioTimer;

void audioCallback(timer_callback_args_t *args) {
  int16_t y[4];
  int16_t outSample;

  if (ring.peek4(resampler.index, y)) {
    outSample = (int16_t)catmullRom(y[0], y[1], y[2], y[3], resampler.frac);
    lastGoodSample = outSample;
    ring.release(resampler.advance(currentStep));
  } else {
    outSample = lastGoodSample;  // hold the last good sample - see header comment
    underrunTicks++;
  }

  // 16-bit signed -> 12-bit unsigned, same shift-based rescale used
  // throughout this pipeline for cheapness; a few LSBs of resolution
  // given up is inaudible against this DAC's own ~24-LSB absolute
  // accuracy (RA6M5 datasheet, Table 53.48).
  uint16_t dacValue = (uint16_t)(((int32_t)outSample + 32768) >> 4);
  R_DAC->DADR[0] = dacValue;
  R_DAC->DADR[1] = dacValue;
}

// -- File switching (BLE PLAY: command, and the boot-time default) -------

// Validates and switches to a new file. On failure, errorOut is set and
// whatever was already playing (if anything) is left completely
// untouched - a bad request never disturbs current playback. See the
// header comment for why the swap itself masks the audio timer's IRQ
// rather than using a checked pause flag.
bool loadFile(const String &filename, String &errorOut) {
  if (filename.length() == 0 || filename.indexOf('/') != -1) {
    errorOut = "invalid filename";
    return false;
  }
  String fullPath = "/sd/" + filename;

  WavReader candidate;
  if (!candidate.open(fullPath.c_str())) {
    errorOut = candidate.lastError();
    return false;
  }

  audioTimer.disable_overflow_irq();

  wav = std::move(candidate);  // closes the old file (if any), adopts the new one
  ring.reset();
  resampler.index = 0;
  resampler.frac = 0.0f;
  lastGoodSample = 0;  // don't carry a stale sample into the new file
  rateFactor = wav.nativeSampleRate() / 48000.0f;
  currentStep = lastKnownRatio * rateFactor;

  // Prime the buffer before resuming, same as the original boot-time
  // pre-fill, so playback doesn't wake into an instant underrun.
  int16_t primer[1024];
  while (ring.freeSpace() >= 1024) {
    size_t got = wav.readSamples(primer, 1024);
    if (got == 0) break;
    ring.push(primer, got);
  }

  audioTimer.enable_overflow_irq();

  currentFilename = filename;
  fileLoaded = true;
  return true;
}

// -- BLE control -------------------------------------------------------
// CodeLess Binary mode / DSPS bidirectional protocol - see the header
// comment and AudioKnobsWebBLEDemo.ino, whose handshake this copies
// exactly. Once binaryModeActive, Serial2 is a raw pipe: ble.readOutput()
// for inbound lines, Serial2.print() directly for outbound (not
// ble.sendCommand(), which expects an OK/ERROR reply Binary mode no
// longer sends).

CodelessPeripheralDevice ble;
bool bleConnected = false;
bool binaryModeActive = false;

// Set after sending anything large (currently just the file list) to
// hold off the periodic telemetry line below for a bit longer, as a
// safety margin on top of the pacing in sendFileList() itself - see
// that function's comment for why a fixed cooldown alone isn't enough.
unsigned long telemetrySuppressedUntilMillis = 0;

void sendFileList() {
  Serial2.print("FILES:BEGIN\n");
  DIR *dir = opendir("/sd");
  if (dir) {
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
      if (ent->d_type != DT_REG) continue;
      String name(ent->d_name);
      String upper = name;
      upper.toUpperCase();
      if (upper.endsWith(".WAV")) {
        Serial2.print(name);
        Serial2.print("\n");
        // Paces our own send rate to roughly match how fast the module
        // can actually relay each line over the air. Confirmed live:
        // sending all lines back-to-back with no delay reliably let
        // the periodic telemetry tick below fire while the module was
        // still mid-relay, which just serializes whatever arrives next
        // on top of it - splicing a telemetry line into the middle of
        // the client's file list. A fixed post-send cooldown alone
        // isn't enough to fix this on its own: a bigger list can
        // legitimately take longer to drain than any single fixed
        // window, so the real fix is not to outrun the link in the
        // first place. This does mean a LIST during active playback
        // pauses the SD refill for the duration of the list (a
        // deliberate, occasional user action, not a continuous one) -
        // acceptable for now, worth revisiting if it causes an audible
        // glitch in practice.
        delay(40);
      }
    }
    closedir(dir);
  }
  Serial2.print("FILES:END\n");
  // Piggybacked here rather than sent separately right after the
  // handshake: a client already reliably requests LIST once on
  // connect, so answering "what's currently playing" as part of that
  // same response needs no new early-handshake message - and an
  // earlier attempt at a connect-time-only PLAYING: sync collided with
  // the AT+BINREQACK echo (see git history) precisely because that
  // moment is far more collision-prone than this one.
  Serial2.print("PLAYING:");
  Serial2.print(fileLoaded ? currentFilename : "NONE");
  Serial2.print("\n");
  telemetrySuppressedUntilMillis = millis() + 1000;
}

void handleBLECommand(String line) {
  line.trim();
  if (line == "LIST") {
    sendFileList();
  } else if (line.startsWith("PLAY:")) {
    String filename = line.substring(5);
    filename.trim();
    String err;
    if (loadFile(filename, err)) {
      Serial2.print("PLAYING:");
      Serial2.print(filename);
      Serial2.print("\n");
    } else {
      Serial2.print("ERR:");
      Serial2.print(filename);
      Serial2.print(":");
      Serial2.print(err);
      Serial2.print("\n");
    }
  } else if (line.length() > 0) {
    Serial.print("Unrecognized BLE command: \"");
    Serial.print(line);
    Serial.println("\"");
  }
}

// -- Pot and touch pad -------------------------------------------------

RV112FF pot1(A2, A3);
MultiTurnKnob knob1(pot1);
CapTouch touch1(20);

const unsigned long CONTROL_INTERVAL = 10;  // ms (~100Hz)
unsigned long previousControlMillis = 0;

const unsigned long STATUS_INTERVAL = 500;  // ms
unsigned long previousStatusMillis = 0;

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {}

  Serial.println("SD resample demo: turn the pot for a continuously-variable playback speed.");
  Serial.println("Touch the pad while turning for fast/coarse sweeps; let go for slow/fine control.");
  Serial.println("Or connect over BLE with web/wav-remote.html to pick a file and watch live stats.");

  touch1.begin();

  if (sdFs.mount(&sdBlockDevice) != 0) {
    Serial.println("SD card mount failed - check it's inserted and FAT32-formatted.");
    while (1) {}
  }

  // Deliberately does not auto-load anything at boot: the board starts
  // silent and idle, with no SD reads happening in loop() at all, until
  // a BLE PLAY: command picks a file (see fileLoaded's use in loop()).
  // This isn't just a UX choice - loop()'s greedy SD refill (however
  // brief per call) turned out to be a real suspect for corrupting
  // inbound BLE UART traffic during testing, and never touching the SD
  // card until a file is actually wanted removes that variable entirely
  // during the always-important connect/LIST moment.
  Serial.println("No file loaded yet - waiting for a BLE PLAY command.");

  // Open and start both DAC channels through the normal Arduino path
  // once, then let the audio interrupt write the data register
  // directly each sample - same split as AudioKnobsDemo/DACAudioTest.
  analogWriteResolution(12);
  analogWrite(A0, 2048);
  analogWrite(A1, 2048);

  ble.begin();
  ble.setupDefault();
  Serial2.setTimeout(20);  // default 1000ms Stream timeout stalls every BLE round trip

  // A hardware timer fires at a fixed 48000Hz; the speed knob changes
  // how far the resampler steps through the source each tick, not this
  // timer's own rate - see the header comment on why. It starts even
  // with nothing loaded yet - audioCallback() just holds lastGoodSample
  // (0, i.e. silence) until a file is loaded.
  uint8_t timerType;
  int8_t timerIndex = FspTimer::get_available_timer(timerType);
  audioTimer.begin(TIMER_MODE_PERIODIC, timerType, timerIndex,
                    48000.0f, 50.0, audioCallback, nullptr);
  audioTimer.setup_overflow_irq(1);  // high priority: audio timing is jitter-sensitive
  audioTimer.open();
  audioTimer.start();
}

void loop() {
  // Checked unconditionally every pass, not gated to CONTROL_INTERVAL -
  // a PLAY: request shouldn't wait up to 10ms for no reason, and this
  // is a cheap check when nothing's waiting (see AudioKnobsWebBLEDemo
  // for the same reasoning).
  if (ble.available()) {
    String line = ble.readOutput();
    if (line.indexOf("+DISCONNECTED") != -1) {
      bleConnected = false;
      binaryModeActive = false;
      Serial.println("BLE central disconnected. Advertising again.");
      // Direct Serial2 write, not ble.sendCommand(): that call blocks
      // for its full timeout waiting for an OK/ERROR/+READY reply -
      // fine at boot (setupDefault()'s one-time calls), but a real
      // problem here, where it would stall loop() - and therefore
      // ble.available() itself - for a second or more right as a new
      // connection might already be starting up.
      Serial2.print("AT+ADVSTART\r");
    } else if (line.indexOf("+CONNECTED") != -1) {
      // A fresh +CONNECTED always means a fresh handshake is starting,
      // even if binaryModeActive was still true from a previous session
      // - +DISCONNECTED isn't guaranteed to arrive for every disconnect
      // (confirmed live: a browser-side reconnect can start a brand new
      // session with no +DISCONNECTED ever seen for the old one), and
      // trusting stale state here would permanently wedge the link until
      // the board itself is reset - it would never re-send
      // AT+BINREQACK for the new session's +BINREQ.
      bleConnected = true;
      binaryModeActive = false;
    } else if (!binaryModeActive) {
      if (line.indexOf("+BINREQ") != -1) {
        // Direct Serial2 write, not ble.sendCommand() - see the
        // AT+ADVSTART comment above. This one matters even more: it
        // sits directly in the path of the handshake a client is
        // actively waiting on, and blocking here risks swallowing
        // whatever the client sends immediately after (confirmed live -
        // this was intermittently eating a LIST sent right after
        // connecting, before this fix).
        Serial2.print("AT+BINREQACK\r");
        binaryModeActive = true;
        // No immediate sync write here on purpose - AudioKnobsWebBLEDemo's
        // own BINREQ handler does nothing beyond setting the flag, and
        // sending anything else right in this exact spot risks landing
        // on top of the ack itself (confirmed live: an immediate
        // PLAYING: sync here showed up merged with the ack, with no
        // separator, in a raw trace). The next periodic status tick
        // (well under a second away) already includes FILE:currentFilename,
        // so a freshly-connected client learns the current state almost
        // immediately anyway without needing a special-cased extra write.
      }
    } else {
      handleBLECommand(line);
    }
  }

  // Greedy, unthrottled SD refill: the whole point of this demo is to
  // let the SD card's real sustained read throughput be the limiting
  // factor once the speed ratio is cranked high enough, so this isn't
  // paced against anything - it just tops the buffer up as fast as
  // loop() can cycle.
  const size_t CHUNK_SAMPLES = 1024;
  if (fileLoaded && ring.freeSpace() >= CHUNK_SAMPLES) {
    int16_t chunk[CHUNK_SAMPLES];
    size_t got = wav.readSamples(chunk, CHUNK_SAMPLES);
    if (got > 0) ring.push(chunk, got);
  }

  unsigned long currentMillis = millis();

  if (currentMillis - previousControlMillis >= CONTROL_INTERVAL) {
    previousControlMillis = currentMillis;
    CapTouch::scan();

    float turnsPerSweep = touch1.isTouched() ? TURNS_PER_SWEEP_FAST : TURNS_PER_SWEEP_SLOW;
    float angle = knobAngle(knob1.read(), turnsPerSweep);
    if (angle >= 0) {
      float ratio = angleToSpeedRatio(angle);
      lastKnownRatio = ratio;
      currentStep = ratio * rateFactor;
      if (ratio < minRatioSeen) minRatioSeen = ratio;
      if (ratio > maxRatioSeen) maxRatioSeen = ratio;
    }
    // else: pot not yet calibrated - leave currentStep at the
    // native-speed passthrough default set in setup()/loadFile().
  }

  // Deliberately short and infrequent: Serial here is native USB CDC
  // (see CLAUDE.md), and a blocking print sitting in a hot loop would
  // itself steal cycles from the SD refill above, manufacturing the
  // very underrun this demo is trying to measure honestly.
  if (currentMillis - previousStatusMillis >= STATUS_INTERVAL) {
    previousStatusMillis = currentMillis;
    float ratio = currentStep / rateFactor;
    float fillPercent = 100.0f * ring.usedSpace() / RING_CAPACITY;
    Serial.print("speed ");
    Serial.print(ratio, 2);
    Serial.print("x (range ");
    if (minRatioSeen <= maxRatioSeen) {
      Serial.print(minRatioSeen, 3);
      Serial.print("-");
      Serial.print(maxRatioSeen, 3);
    } else {
      Serial.print("n/a");  // pot wasn't calibrated during this window
    }
    Serial.print(")  buffer ");
    Serial.print(fillPercent, 0);
    Serial.print("%  underruns ");
    Serial.println(underrunTicks);

    // Deliberately short and fixed-shape, unlike the Serial line above
    // (FILE is covered separately by the PLAYING: line sent on connect
    // and on every switch, so dropping it here loses no information a
    // client needs) - though the real fix for telemetry splicing into
    // a file list mid-send turned out to be telemetrySuppressedUntilMillis
    // above, not message size (an isolated short-form test still showed
    // the same symptom before that cooldown existed).
    if (binaryModeActive && millis() >= telemetrySuppressedUntilMillis) {
      String stat = "SPD:" + String(ratio, 2) +
                     ",BUF:" + String((int)fillPercent) + ",UND:" + String(underrunTicks);
      Serial2.print(stat);
      Serial2.print("\n");
    }

    minRatioSeen = 1e9f;
    maxRatioSeen = -1e9f;
  }
}
