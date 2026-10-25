# PedalBoard

**官网 · Official website: <https://dengxiaoping8964.github.io/PedalBoard/>**

A Windows-only guitar pedalboard: **guitar in → reorderable effect chain → physical
monitor out + a VB-CABLE virtual output a recording program can see.**

The 22 built-in effects are real DSP implementations with live knobs and a real
bypass, and they are joined by two ways to build a custom one: a composed **Custom
Chain** of drive/filter/modulation/time stages, and a **Plugin Host** that loads a
user-supplied VST3 effect with eight macro knobs and its native editor. Nothing in
the chain is a placeholder, and nothing is a UI switch wired to a no-op. Where JUCE
8 does not provide what the design called for, the class is written by hand and the
substitution is listed in
[Deviations](#deviations-from-the-brief-verified-against-juce-8015) rather than hidden.

## Vibe coding / 关于本项目

This project is a **vibe-coding result**: the product direction, the sound decisions,
the listening tests and the final review are human; the C++/CMake/UI code, the offline
signal bench and most of the documentation were produced with an AI coding agent
working under that direction.

本项目是 **vibe coding** 的成果：产品方向、听感取舍、实际试听与最终验收由本人负责；
C++/CMake/UI 代码、离线信号测试和大部分文档由 AI 编程代理在本人指导下完成。

Contact / 联系：**a0966681154@gmail.com**

---

## Read this first: what has and has not been verified

The tree **compiles and runs**. That was not true when this file first said otherwise,
and the two sections below separate what has been measured from what has not.

Tags: ✅ verified by a command or a capture · ⚠️ compiled, not yet verified at runtime ·
❌ known failure · ⛔ cannot be verified on this machine.

| Status | What | How |
| --- | --- | --- |
| ✅ | Builds with 0 errors and 0 warnings | MSVC 14.44.35207 + Windows SDK 10.0.26100.0, both installed on D:; `cmake --build --config RelWithDebInfo` exits 0 with `/W4` clean (the 36 `C4244` narrowing warnings were all fixed rather than suppressed) |
| ✅ | The window appears, on screen, and stays up | `EnumWindows` + `GetWindowRect` (client 1240x712 inside a frame of (55,17)-(1311,768) on a 1366x768 display), process alive with no `Application Error` event in the last 20 minutes |
| ✅ | The UI actually paints | screen capture of the window: dark theme, effect cards, knobs, meters, status bar. Both languages captured after a live switch |
| ✅ | First-run language prompt, and switching back | clicked through with injected mouse input; English → 中文 → English verified on the panel, the cards, the add-effect menu and the status bar |
| ✅ | Every JUCE 8.0.15 API touched | read against a real 8.0.15 checkout, file by file, before use |
| ✅ | The three built-in impulse responses | generated and then measured: header bytes, peak, RMS, FFT band profile, spectral centroid, −20 dB decay time. `Resources/make_irs.mjs` prints all of it and refuses to write silent or look-alike responses |
| ✅ | The pitch shifter, the sub-octave divider, all 24 effect types at their parameter extremes, the Custom Chain composition, and the ring buffer's overflow/underflow policy | `_tests\dsp_probe.exe`: 376 passes, 0 failures. See [The DSP and ring-buffer bench](#the-dsp-and-ring-buffer-bench) - it is where five real defects were found, none of them visible in the UI |
| ✅ | Loading a real VST3 effect, its editor and its state | The installed **Auburn Sounds Panagement 2** (single-file .vst3, 46 parameters, 3 samples latency) was loaded from `PedalBoardData\PedalBoard.settings`. Card mapped 8 of 46 parameters (bypass excluded); moving the first macro changed the plugin's `Far` parameter and the editor's FAR knob visibly moved; the 455-byte `customPlugin.state` (382-byte plugin blob + path header) was written and restored; an old state file without the path header was rejected and the plugin loaded at its own defaults; closing the editor, closing the main window with it open, and restarting all completed without a crash. `_tests\vst3_probe.exe` independently measured in-RMS 0.05782, max sample change 0.19591, state round-trip 382 bytes |
| ⛔ | Hearing the Custom Chain or a hosted plugin through a guitar | no listening test is possible from here; the bench and the probe measure signals, not taste |
| ⚠️ | The lock-free ring arithmetic | now measured, not just inspected: a sustained worst-case read (192 kHz input into a 44.1 kHz chain) never overflows the configured capacity, and a 17-minute idle soak against the real devices reported 0 overflows on both rings and exactly one underflow on each - the one-off phase offset while the two device callbacks start - with the counters unchanged at minutes 2, 7 and 17. What is *not* measured is behaviour under a scheduling hiccup on a loaded machine |
| ⚠️ | Real-time safety of the audio path | by inspection: allocation/IO/lock sites are enumerated in [Audio thread rules](#audio-thread-rules-the-code-actually-follows). One violation of the same family already caused a crash (see below), so inspection is not sufficient |
| ⛔ | A guitar coming out of the monitor device | no listening test is possible from here. Latency and level are read from the API, not from sound |
| ✅ | The virtual recording path | VB-CABLE is installed on this machine (this row was ⛔ when it was not). `CABLE Input` opens at 48000 Hz / 480 samples over Windows Audio and the app reports 40 ms. `_tests\virtual_path_probe.exe` pushes a 440 Hz sine through the real `VBCableRouter` and ring and records `CABLE Output`: RMS 0.1768, peak 0.2500 and a 440 Hz amplitude of 0.2500 for a 0.25 sine. A real guitar through the chain is still ⛔ |
| ⚠️ | Two clocks staying aligned over hours | 2.2 s of the probe and a 17-minute app soak are measured; hour-scale drift between the speaker clock and the cable clock is not |
| ⛔ | That the chain sounds like what its names claim | needs ears |

Sixteen runtime defects were found by running it, not by reading it, and all are fixed:

1. **Heap corruption on the first audio block.** Effects were prepared with
   `getCurrentBufferSizeSamples()`, which reports what was *requested*; a driver may
   hand the callback a larger batch, and an effect sized to the request wrote its
   scratch buffer past the end. Symptom was `0xC0000005` inside
   `CompressorEffect::process`, reached by symbolising the WER fault offset against a
   linker map. Everything is now prepared with `maxBlockSize`.
2. **No window at all.** `DocumentWindow` was constructed with `addToDesktop = false`
   and only `setVisible(true)` was called afterwards, which in JUCE 8 sets a flag and
   never creates the peer (`juce_Component.cpp:543`). The process ran happily with
   nothing on screen. The window is now added to the desktop in the constructor, and
   sized to the display, because a fixed 880-pixel height on a 768-pixel screen put the
   native title bar - close button included - above the top edge.

3. **The device selectors were blank over a running stream.** Two separate causes:
   `AudioDeviceManager` clears both device names in its stored setup whenever it
   opened a default rather than a chosen device (`juce_AudioDeviceManager.cpp:747`),
   so the engine had nothing to report and now adopts the name from the open
   `AudioIODevice` instead; and the selectors kept their selection from
   `ComboBox::getText()`, which is empty until something is selected, so the first
   populate left four full lists with no selection. The engine's own current device
   is now the fallback.
4. **The drop counters reported the wrong thing.** Switching the recording path off
   left the audio thread writing into a ring nobody read, so the status bar counted
   overflow on a path the user had deliberately stopped - and would have replayed
   that stale backlog on switching it back on. The producer now stops, the engine
   abandons the residue on re-enable, and the counters are cleared when a stream
   starts so they describe the stream that is running.
5. **The fourth status line was cut in half.** The strip was 58 pixels high for four
   lines that measured 15 pixels of leading each plus padding, so the problem line
   at the bottom was clipped by the window frame. Found by cropping the captured
   bottom edge at 4x rather than by reading the layout code. The height is now
   derived from `statusLineHeight`, and each line is drawn into its own fixed rect.
6. **The virtual drop counter climbed into the thousands with no cable installed.**
   `enabled` is the user's request to use the cable, not evidence that its stream
   opened, so the audio thread kept pushing into a ring whose consumer never ran.
   The push path now requires `router->isOpen()`; measured after the fix, the idle
   counter stays at `virtual 0`.
7. **The CABLE meter showed the chain, not the cable.** It read the chain buffer
   unconditionally, so with TO CABLE off - and even with no cable installed - it
   displayed a live level while the virtual path was stopped. Found by comparing
   the meter's pixels between two captures rather than by reading the meter code.
   It now reads zero unless the path is enabled *and* open. The virtual latency
   readout had the same flaw: it quoted +43.2 ms for a cable that was not there,
   and now reads +0.0 ms until the stream actually opens.
8. **The router could not open the cable it claimed to be running.** `VBCableRouter`
   called `setCurrentAudioDeviceType()` and `setAudioDeviceSetup()` on a *fresh*
   `AudioDeviceManager` that had never been `initialise()`d. Such a manager has no
   device types, so both calls did nothing and returned an empty error string
   (`juce_AudioDeviceManager.cpp:823` enters the delete-and-return branch). The app
   then set `open = true`, availability `running`, and displayed *"Active, adds about
   43.2 ms"* while no stream and no callback existed. With the driver now installed
   this showed up as a virtual drop counter climbing at one block per block - 958 to
   2984 in ten seconds - because every push filled a ring with no consumer. Found by
   writing `_tests\cable_probe.exe`, which reproduced "no error, no device" with the
   raw API. `start()` now calls `initialise(0, 2, nullptr, false, deviceName, nullptr)`
   (the preferred-device wildcard is what makes it pick the cable instead of the
   system speakers), then verifies `getCurrentAudioDevice() != nullptr` and reports
   the failure instead of inventing a stream.
9. **Every effect processed 2048 samples per 480-sample callback.** The chain buffers
   were allocated at `maxBlockSize` and never resized, and every effect reads
   `audio.getNumSamples()`. Each callback therefore processed the 480 new samples
   *plus* 1568 samples of stale tail: delay lines and LFOs advanced 4.27x faster than
   wall time (a 300 ms delay repeated in about 70 ms), and `pushToVirtualRing()` wrote
   2048 samples into a ring the cable drains 480 at a time - the other half of the
   virtual drop counter. Found by following that counter through `pushToVirtualRing`.
   The engine now resizes `chainBuffer`, `dryBuffer` and `mixedBuffer` to `numSamples`
   at the top of every callback (`avoidReallocating`, so no heap involvement), and
   `CabinetEffect` - the only effect that handed its whole scratch buffer to
   `Convolution` - now builds a `numSamples`-long block.
10. **The settings loader read the bypass marker as a new parameter.** A saved line
    is `v0|v1|...|b0`; when a knob was appended, the loop ran past the old values
    and `getFloatValue()` turned the trailing `b0` into 0 for the new knob, so
    Hysteresis loaded at 0 dB instead of its 3 dB default after the gate gained the
    control. Found by launching the rebuilt app against the previous settings file
    and reading the card. The loader now stops at the first token beginning with
    `b`, so appended parameters keep their defaults until the card is touched.
11. **Overdrive was the heavier pedal and Distortion the milder one.** The listening
    report ("过载弹出了强失真…默认的失真却是过载的感觉") was right: at a -20 dBFS
    input the old Overdrive put +24 dB into a hardness-3.75 tanh and measured
    36.5 % THD, while the old Distortion measured 22.1 %. The input curves and
    defaults were recalibrated; the bench now enforces 7.6 % vs 29.5 %.
12. **Unplugging the interface at launch erased the remembered devices and clock.**
    `initialise()` left `inputDeviceName`/`outputDeviceName` empty when the saved
    device could not be opened, and `saveSettings()` then wrote those empty names on
    exit; the same happened to a 441-sample buffer, which came back as the manager's
    512. The preferred names and clock are now kept before any open is attempted, and
    the live values are used only when they are real.
13. **The old pitch settings had no valid mapping to the new Formant knob.** A saved
    `Semitones|Window|Mix` line would have loaded Window 40 as Formant +40. Settings
    version 2 migrates the layout for every old file, keeping Semitones and Mix and
    starting Formant at 0.
14. **The Cabinet's Mix comb-filtered.** `Convolution` has a 512-sample latency, but
    the effect crossfaded it directly against the un-delayed dry. Every Mix below
    100 % was therefore a comb with a fixed 512-sample delay, which reads as extra
    colour or "distortion". Its dry side now uses the same latency-compensated mixer
    as the oversampled pedals.
15. **Moving delay taps were read linearly.** A linearly interpolated tap is a
    low-pass whose corner moves; on a modulated chorus, vibrato, flanger or tape
    delay that adds HF grit (reported as "8-bit" chorus and a distorted delay). All
    modulated reads now use the Catmull-Rom kernel.
16. **The Noise Gate's Attack and Release knobs did nothing while the app was
    running.** Both coefficients were computed only in `doPrepare()`, so the card
    changed atomics that `process()` never reread and the gate kept the timings from
    the moment the device opened. They are recomputed when the knobs move. In the
    same card, the lamp read "Closed" during Hold while the gain was frozen wide
    open; it now follows the applied gain. Bench: Attack 1 ms 0.1938 vs 50 ms 0.0222
    in the first 10 ms; Release 5 ms 0.0000 vs 1000 ms 0.0034 150 ms after the
    burst; Hold reads Open while held and Closed after hold + release. A bypassed
    compressor or gate now hides its last readout instead of showing stale numbers.

Also fixed while the app was being looked at rather than read: a failed device
switch now restores the previously working device and reports both errors instead of
leaving the engine on a dead device; a device selector whose remembered name does not
belong to the current device type falls back to the first device that type reports
instead of showing an empty box; the native window title and the IR file chooser are
translated; the dragged card is actually dimmed (the paint code existed, but nothing
called `setBeingDragged`); adding an effect scrolls the new card into view instead of
leaving it below the fold with no visible result; and the status bar's drop count is split into overflow and
underflow, because the two point at different causes (a stalled consumer versus a
starved one). The first read of a new stream is no longer counted as an underflow:
the output callback can legitimately run before the input callback has delivered
anything, and counting that put a permanent `1` on the status bar of a healthy
launch. A stream can still show one genuine underflow while the two device callbacks
settle - the consumer catches up right after the first block and asks for the next
one before the producer has delivered it - so the honest figure at idle is
`输入 1（溢出 0 / 不足 1）虚拟 1（溢出 0 / 不足 1）`, not zero. It was measured
unchanged at minutes 2, 7 and 17 of a soak, which is what separates the start-up
transient from a real one. The device panel was
not the last child of the window, so the effect cards painted over its lower half;
the panel's notice block advanced a hardcoded 118 pixels and collided with the line
below it, which is now measured with `TextLayout`; the first-run test was "does the
settings file exist", which the file's own save-on-exit made meaningless, so a
`chosen` flag records the visit instead; the top bar's buffer box clipped
"480 (10.0 ms)" and the row was 1048 logical pixels wide against a 1040 minimum
window, so both numbers now come from one `minWindowWidth`; and the card's move
buttons were too narrow for "Down" in either language.

### Second pass: controls that lied, and real-time hazards

These came from exercising every card in the running app and reading each effect's
parameter table against its DSP, not from a crash. Each one is now fixed and was
re-checked - `dsp_probe` (0 failures, including two new wah cases), the translation
checker, and a rebuilt app driven with injected mouse input:

1. **Looper commands touched the whole tape on the audio thread.** Record and Clear
   each zeroed 30 s of stereo (about 11.5 MB), and Overdub could copy it; that memset
   was the largest single block-time spike in the app. Reads are already bounded by
   `loopLength`, so the unused remainder never needs clearing: Clear now drops the
   length, and Undo swaps only the used range.
2. **The seqlock reader could spin forever.** `SeqSnapshot::acquire()` retried until
   two reads agreed; a writer preempted between its two sequence bumps left the audio
   thread looping without a bound. It now makes four attempts and returns the last
   consistent payload, so a torn snapshot can never stall a callback.
3. **Denormals were only flushed in some effects.** A `juce::ScopedNoDenormals` now
   wraps the whole engine callback, covering filters and feedback lines that decay
   into the denormal range at idle.
4. **The compressor's Attack knob promised 0.1 ms; the detector cannot go below
   2 ms** (its RMS window). The range now starts at 2 ms. Makeup promised 24 dB while
   the gain stage caps at +12 dB, so the top half of the knob did nothing; it is now
   0-12 dB. The gain-reduction readout was computed as a negative number and now
   reads positive (`增益衰减 13.1 dB` in the captured card).
5. **The EQ's Mid Freq knob went to 10 kHz and the code silently clamped it near
   3 kHz.** The clamp is gone; the bell sits where the knob says.
6. **A crossed Min/Max on the Wah and Auto Wah produced a dead sweep.** Setting Min
   1500 Hz and Max 800 Hz swept a 1.9x band in the wrong direction. Both effects now
   sort the bounds; the bench verifies 800 Hz to 1500 Hz for that setting.
7. **Wah Source = MIDI CC with no MIDI input left the pedal frozen.** The Position
   knob did nothing and the card said nothing about why. It now falls back to the
   knob and the card displays a warning (translated) instead of lying; verified with
   no MIDI device open: Position 50 % read 1058 Hz, dragged to 0 % read 350 Hz.
8. **The bitcrusher dithered every output sample, not every held step.** The dither
   was added when the held value was output, which smeared the staircase the effect
   exists to make. Dither and quantisation now happen once per hold.

---


## Architecture

Three `AudioDeviceManager` instances, three threads, joined only by two lock-free
single-producer/single-consumer rings and atomics. **No thread ever waits on another.**

```
guitar ──► inputManager callback ──► inputRing ──┐
                                                 │  (resampled if the clocks differ)
                    ┌────────────────────────────┘
                    ▼
        outputManager callback   ← the chain's clock
        dry copy → effect chain → global dry/wet
                    ├────────────► physical output (monitor switch)
                    └────────────► virtualRing ──► cableManager callback ──► CABLE Input
                                                                            (recording app
                                                                             reads CABLE Output)
```

* **Path A - monitoring.** The Monitor switch multiplies the physical output only. It
  never stops the chain, so muting your speakers does not mute the recording.
* **Path B - virtual.** The same processed block is always pushed into `virtualRing`;
  the router's own thread drains it into VB-CABLE's "CABLE Input" stream. If the cable
  falls behind, the cable loses audio and a counter increments - the speakers do not.
* **Path C - global dry/wet.** The dry side is a copy of the raw input delayed by the
  chain's summed group delay, through a ramped two-tap fractional delay line, so the
  blend cannot turn into a comb filter.

Chain order is published to the audio thread through a seqlock
(`SeqSnapshot<ChainSnapshot>`): the writer never blocks, the reader copies 16 pointers
once per block and retries only if it straddled a write. Effects are instantiated once
each at construction and live for the process, so reordering republishes pointers and
cannot invalidate one that is in use.

---

## Build

```bat
git clone https://github.com/DengXiaoping8964/PedalBoard.git D:\PedalBoard
```

The commands below assume that checkout at `D:\PedalBoard`.

Everything lands on `D:` - source, build output, settings, IRs, and the hosted
plugin's state. Nothing is written to `C:` by this project. A VST3 the user loads is
read from wherever it is installed; the app never copies or installs it.

### 1. Toolchain, installed on D:

Visual Studio 2022 Build Tools with the C++ workload. The installer's default target is
`C:\Program Files (x86)\...`, so point it elsewhere. It needs administrator rights: the
UAC prompt appears on the secure desktop, so start it from Explorer, not from a shell you
cannot elevate from.

```bat
vs_buildtools.exe --quiet --wait --norestart ^
  --installPath D:\BuildTools ^
  --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended
```

`--includeRecommended` is what brings the compiler, the Windows SDK, **CMake and Ninja**:
measured on this machine that one command produced MSVC 14.44.35207, SDK 10.0.26100.0,
CMake 3.31.6-msvc6 and Ninja 1.12.1, 3.4 GB on `D:`. Only the Windows SDK is forced onto
`C:` (about 2-3 GB) because its MSI target path is hard coded and `--installPath` cannot
redirect it.

CMake and Ninja are *not* added to `PATH` by that install, so use the full paths:

```bat
set CMAKE=D:\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set TMP=D:\Temp
set TMPDIR=D:\Temp
```

(`TMP` matters: MSBuild and CMake scratch files follow it, and by default that is `C:`.)

### 2. Generate the impulse responses

`CMakeLists.txt` fails with a clear message if they are missing:

```bat
node Resources\make_irs.mjs
```

This writes `Resources/IRs/ir_1x12_v30_closed.wav`, `ir_2x12_open_back.wav` and
`ir_4x12_high_gain.wav`, which `juce_add_binary_data` embeds into the executable and
also copies next to it as `IRs/` so you can inspect or replace them.

### 3. Configure and build

```bat
cmake -S D:\PedalBoard -B D:\PedalBoard\build -G "Visual Studio 17 2022" -A x64
cmake --build D:\PedalBoard\build --config Release --parallel
```

JUCE 8.0.15 comes down by `FetchContent` into `D:\PedalBoard\build\_deps` - which is on
D: only because the build directory is. To use an existing checkout instead (no
download, and the fastest way to iterate):

```bat
cmake -S D:\PedalBoard -B D:\PedalBoard\build -DPEDALBOARD_JUCE_LOCAL=D:\JUCE
```

Other knobs:

| Option | Default | Meaning |
| --- | --- | --- |
| `PEDALBOARD_JUCE_TAG` | `8.0.15` | the JUCE tag the code was written against |
| `PEDALBOARD_JUCE_LOCAL` | empty | path to a local JUCE checkout; skips FetchContent |
| `PEDALBOARD_ASIO` | `OFF` | ASIO support. The Steinberg SDK is not redistributable: put a licensed copy in `ThirdParty/asio` and turn this on, or the build fails with a message. Without it the app runs on WASAPI shared/exclusive, MME and DirectSound |
| `PEDALBOARD_EMIT_MAP` | `OFF` | asks the linker for a `.map` next to the binary, which turns a Windows Error Reporting fault offset into a function name without a debugger. Off because the map is several megabytes |

The output is `build\PedalBoard_artefacts\<config>\PedalBoard.exe` - the `artefacts`
level comes from JUCE's CMake target, and the folder is the configuration you built.
What is actually on this machine, built and run:

```
build\PedalBoard_artefacts\RelWithDebInfo\PedalBoard.exe
```

The first run asks for a language, then shows the device panel, because nothing is
assumed about your default devices.

---

## Languages

The interface is English and Simplified Chinese. The question is asked once, on the
first run, in both scripts at once:

```
Choose the language for the interface.
请选择界面语言。
```

and can be changed at any time from the bottom of the device panel, where the box
lists each language in its own script (`English` / `中文`) so it is readable before
anything has been translated. The choice is stored in `PedalBoardData\PedalBoard.settings`
as `language=0|1`; `chosen=1` is recorded separately, because the same file is written
when the window closes and "the file exists" therefore proves nothing about whether
the user ever saw the panel.

Two decisions worth knowing before editing this:

* **Translation happens where a string is drawn**, never where it is stored. Effect
  names, parameter names and categories live once in English inside the effect classes
  and `EffectFactory`; the UI wraps them in `pedal::tr()` at the point of display. That
  is what lets the language change without rebuilding the audio path, and it keeps the
  settings file language-independent.
* **The Chinese table is compiled in**, in `Source/Common/PedalLanguage.cpp`, as a
  `juce::LocalisedStrings` document. A missing or mis-encoded translation file next to
  the exe would otherwise leave a first-run user with an empty dialog.

Lookup is `juce::LocalisedStrings`, so strings that come from inside JUCE can be
translated too. A key with no entry falls back to the English text - silently, which
is why there is a checker:

```bat
node D:\PedalBoard\_toolchain_test\check_translations.mjs
```

It compares the `tr()`/`trf()` arguments and every other literal in `Source` against
the table and reports missing keys, dead entries and duplicates. Current state: 244
entries (240 with Chinese values; the four identical-name product labels are
intentionally untranslated), no missing keys.

Two constraints the table imposes on new strings:

* **Keys must be single line.** `LocalisedStrings::loadFromText` splits its input on
  newlines, so a paragraph is stored as separate keys and joined at the call site.
* **Sentences are translated whole.** `trf("Input: %1", error)` with `%1..%16`
  placeholders, not `tr("Input:") + error`: Chinese does not keep English word order,
  so concatenating translated fragments produces English grammar with Chinese words.

Chinese renders through `Microsoft YaHei UI` / `Microsoft YaHei` / `SimHei` / `DengXian`,
probed in that order at switch time by asking DirectWrite for the face and checking the
name that comes back - requesting a font that is not installed does not fail, it
silently returns a substitute with no CJK coverage. If none of the four is present the
family stays empty and the UI keeps the default face.

That fallback is not hypothetical: the first-run prompt is drawn before any language is
chosen, so it uses the default face, and its Chinese lines rendered correctly on this
machine (`msyh.ttc` present) - Windows substitutes a CJK-capable font at draw time. The
explicit family is there so the rest of the UI does not depend on that.


---

## The DSP and ring-buffer bench

`_tests/dsp_probe.cpp` compiles the real effect headers and measures them. It is not
a unit test of logic; it is a signal in, number out check for the things that can
only be wrong in the waveform.

```bat
cmake -S D:\PedalBoard\_tests -B D:\PedalBoard\_tests\build -G "Visual Studio 17 2022" -A x64
cmake --build D:\PedalBoard\_tests\build --config Release --target dsp_probe
D:\PedalBoard\_tests\build\Release\dsp_probe.exe
```

It exits non-zero if anything fails, so it can gate a build. Current state: **376
passes, 0 failures**, covering

* the spectral shifter's latency against `getLatencySamples()`, by cross-correlating
  a noise burst at 44.1 and 48 kHz - both cases agree exactly (80 ms);
* pitch accuracy at +12, -1, -12, +5 and 0 semitones on both 44.1 and 48 kHz, with
  the un-shifted leak reported separately - every case measures 0.0 cents off, the
  next strongest component between 41 and 118 dB below (the +12 case: 220.50 Hz,
  leak -105 dB);
* the sub-octave divider at 110, 330 and 82.4 Hz - within 2.5 cents of exactly half,
  at full level;
* the upper octave is a rectifier: 220 Hz in, 440.0 Hz out, at full level;
* the acoustic-voicing gate: at -20 dBFS the Overdrive default measures 7.6 % THD
  and the Distortion default 29.5 %, and the older 36.5 % vs 22.1 % inversion fails
  the bench;
* Chorus and Delay harmonic cleanliness at their defaults (1 kHz in): 0.000 % and
  0.001 % THD, so the "8-bit" grit and the "distorted" delay are measurable
  regressions from here on;
* every one of the 24 effect types with every parameter at both extremes, streamed
  in successive blocks: finite, bounded, and `Mix` at its minimum indistinguishable
  from the input. Three types are idle by design at their defaults - the cabinet
  before an IR, the looper before a recording, and the plugin host before a plugin -
  and the bench asserts that those three really do pass the signal through;
* the ring buffer's overflow and underflow policy, including the sustained
  worst-case read that the sizing argument in `PedalConfig.h` depends on;
* the Dattorro reverb's pre-delay, decay and stereo width;
* true ping-pong timing (left at Time, right at 2 x Time, no leakage);
* gate hysteresis (a level between the two thresholds holds an open gate);
* gate Attack and Release written **after** `prepare()`, measured on a burst and a
  low-level tail: 1 ms vs 50 ms reaches 0.1938 vs 0.0222 in the first 10 ms, and
  5 ms vs 1000 ms leaves 0.0000 vs 0.0034 150 ms after the burst; Hold keeps the
  reported state Open while the gain is frozen;
* compressor Attack and Release written **after** `prepare()`: 2 ms vs 100 ms
  measures 0.0141 vs 0.3542 in the first 30 ms of a burst, and 10 ms vs 1000 ms
  leaves 0.0021 vs 0.0002 150 ms after it; both dynamics cards hide their readout
  while bypassed instead of leaving the previous block's value on screen;
* first-order ADAA alias suppression on the tanh clipper;
* chorus Spread (mono at 0, decorrelated at 100).

The plugin path has a second, deliberately small probe because it cannot be folded
into the offline bench: loading a `.vst3` needs COM and a message loop, and no plugin
is guaranteed to exist on a build machine. It loads any effect file with the same
JUCE classes the card uses and prints its classes, parameters, a processing
difference and state size:

```bat
cmake --build D:\PedalBoard\_tests\build --config Release --target vst3_probe
D:\PedalBoard\_tests\build\Release\vst3_probe.exe "C:\Program Files\Common Files\VST3\Auburn Sounds Panagement 2-64.vst3"
```

Measured on this machine: 1 class, 2-in/2-out non-instrument, 46 parameters, 3
samples latency, in-RMS 0.05782 / max diff 0.19591, first macro `Far` accepts 0.5000,
state 382 bytes before and after a round-trip.

The first two bugs came out of the old granular pitch engine, which the bench
eventually condemned and which is now replaced by Signalsmith Stretch; they are
kept here because they explain why the replacement happened. The rest apply to
code that is still in the tree, and none were visible in the UI:

1. **The pitch shifter was not shifting.** Grain start positions advanced by `hop`
   per grain while each grain read its window at `hop * ratio`, so the grain train
   crawled relative to what it was reading. A one-semitone request came back as the
   input amplitude-modulated at the grain rate - peaks at 199, 202, 215 and 223 Hz
   within 0.6 dB of each other and no shift. Fixed by advancing the sequence at the
   shifted rate too.
2. **Large shifts ran out of memory.** With the pointer fixed, an absolute read
   position drifts out of its window: -12 semitones consumed input more slowly than
   it produced output until every read clamped to the oldest sample and the output
   became a 38 Hz drone. Fixed by wrapping the pointer and *searching* the wrap
   point for the waveform that best continues what the previous grain was reading,
   which is SOLA placement applied only where a jump is unavoidable.
3. **Every oversampled pedal had a feedback path through its own scratch buffer.**
   `Oversampler::up()` built a JUCE audio block over the whole `AudioBuffer`, which
   is sized for `maxBlockSize` (2048) rather than the block being processed, so
   samples past the end were stale, got filtered, and were written back by `down()`
   into the same buffer - where the next block read them again. Invisible at normal
   settings; the bitcrusher's sample-and-hold puts energy near Nyquist where the
   half-band IIR pair has the most gain, and it reached 5e12 and then NaN. Fixed by
   trimming the block to the caller's block length. This is also a small CPU win on
   every distortion, since the oversampler now does 512 samples' work instead of
   2048.
4. **Every drive pedal comb-filtered at Mix 50 %.** The oversampled wet path is
   delayed by the oversampler's latency; the dry copy was not. A half-and-half blend
   of a signal with a delayed copy of itself is a comb filter, so the pedals had a
   fixed set of notches at every mix setting below 100 %. The fix delays the dry copy
   by exactly `getLatencySamples()` (`mixWithDelayedDry`), applied to Overdrive,
   Distortion, Fuzz, Bitcrusher and Acoustic to Electric. The bench's first alignment
   test was itself wrong: a polyphase IIR oversampler rings *before* its nominal
   latency, so "no energy before latency N" is physically impossible. It now compares
   pre-latency energy with total energy (under 2 % for all five) and requires the peak
   at or after the reported latency.
5. **Distortion was loudest in the middle of its drive range.** Its hard-clip
   threshold moved down as drive went up while the input gain moved up, so the two
   crossed: the pedal got louder, then quieter, then louder again. The threshold is
   now fixed at 0.55 and drive only pushes the signal into it; the output ceiling is
   `level * 0.005`, so 100 % is about +6 dB, not +14 dB. Measured RMS on a 0.25 sine
   at minimum / default / maximum drive: Overdrive 0.050 / 0.219 / 0.221,
   Distortion 0.054 / 0.202 / 0.224, Fuzz 0.094 / 0.204 / 0.207 - all within 15 dB,
   all audible, none clipping.

One more finding was the bench's own fault and is recorded here because it is the
kind of mistake that manufactures fake bugs: the first version ran the *same* buffer
through an effect eight times to let envelopes settle, which cascades a static
filter eight times and "found" a 3-Band EQ peaking at 4e30 with a +15 dB shelf.

### Two cable probes

`dsp_probe` cannot see device management, which is where defects 8 and 9 lived. Two
small executables cover it, and both are built by the same CMake project:

```bat
cmake --build D:\PedalBoard\_tests\build --config Release --target cable_probe virtual_path_probe
D:\PedalBoard\_tests\build\Release\cable_probe.exe       :: device format and cadence
D:\PedalBoard\_tests\build\Release\virtual_path_probe.exe :: sine in, recording out
```

Close PedalBoard first: the cable's shared-mode endpoints accept one client at a time.
The second probe is the one that matters - it uses the real router class, not a copy,
and the PASS is an amplitude measurement at the frequency that was sent.

---

## Licence / 许可证

The original source in this repository is released under the **GNU Affero General
Public License v3.0 (AGPLv3)**; the full text is in [LICENSE](LICENSE). This is the
licence compatible with linking JUCE 8 under its AGPL option, which is how this
project uses it. If you hold a commercial JUCE licence you may license your own
combination differently, but the copy published in this repository stays AGPLv3.

Third-party components keep their own licences; see the next section.

---

## Third-party code

* **JUCE 8.0.15** is used under JUCE's own licence (AGPLv3 or a commercial JUCE
  licence), and nothing about it is modified. CMake downloads it with `FetchContent`
  at configure time; this working tree keeps a local checkout in the ignored
  `vendor/JUCE` folder, and `-DPEDALBOARD_JUCE_LOCAL=...` points the build at any
  other checkout.
* **Signalsmith Stretch 1.3.2** and **Signalsmith Linear 0.6.6**
  (`vendor/signalsmith-stretch`) are MIT-licensed; both `LICENSE.txt` files are
  kept next to their sources. Stretch is the pitch shifter's engine.
* **The VST3 SDK bundled inside JUCE** (Steinberg's SDK, reached through JUCE's
  `VST3PluginFormat`/`AudioPluginInstance` hosting classes) is what makes the Plugin
  Host card work. This project does not redistribute or install any VST3 plugin; it
  only reads a file the user already has. A plugin loaded by the user remains under
  its own licence.
* **VB-CABLE** is Donationware by Vincent Burel. This application does not bundle
  it, install it, or copy its driver; see the next section.

---

## VB-CABLE: what this app does and does not do

**VB-CABLE is Donationware by Vincent Burel.** This application does not bundle it,
does not install it, and does not run its installer. It only enumerates and opens a
device that is already in the system.

Detection is by output device name containing `CABLE Input`. When nothing matches, the
status bar reads:

```
VB-CABLE not detected - virtual recording unavailable
```

and the device panel shows where to get it - <https://vb-audio.com/Cable/> - states the
licence, and says the app has to be restarted afterwards, because JUCE enumerates
devices at start-up. **Everything else keeps working**: input, chain, monitoring, both
meters, all effects. The virtual path is an optional add-on, not a dependency.

To record: install VB-CABLE, restart PedalBoard, and in the recording application
select **CABLE Output** as its input.

### Measured on this machine

VB-CABLE is present here, and the whole path was exercised rather than inferred:

* `CABLE Input (VB-Audio Virtual Cable)` opens over **Windows Audio at 48000 Hz /
  480 samples** (the system speaker device is 48000/480 too, so no resampling is
  needed). Its callback runs at about 101 per second.
* `_tests\virtual_path_probe.exe` uses the production `pedal::AudioRingBuffer` and
  `pedal::VBCableRouter`: it writes a 0.25-amplitude 440 Hz sine into the ring at the
  engine's block rate, opens `CABLE Input` through the router, and records
  `CABLE Output` with a second `AudioDeviceManager`. Output:

  ```
  start: error="" open=1 availability=3 device="CABLE Input (VB-Audio Virtual Cable)" type="Windows Audio"
  status line: Virtual Device: CABLE Input (VB-Audio Virtual Cable) - Active, adds about 40 ms to the recording path
  capture aboutToStart: 48000 Hz buffer=480 name=CABLE Output (VB-Audio Virtual Cable)
  captured=105120 samples (2.19 s), rms=0.1768 peak=0.2500 tone amplitude at 440 Hz=0.2500
  virtual drops: 2
  PASS
  ```

  The RMS and the Goertzel amplitude are what a 0.25 sine must produce, so the cable
  carried the samples the router was given - not merely "a device opened".
* `_tests\cable_probe.exe` reports the raw device format and cadence when something
  looks wrong, and is what exposed the fake-success bug listed above.

Two honest details. The driver service (`VBAudioVACMME`) is **DEMAND_START**: it was
STOPPED before the cable was first opened and RUNNING afterwards, which Windows does
by itself; this app never starts, installs or registers a driver. And "installed" is
not "working": the old build could report a running cable while no device existed,
which is why `start()` now proves the device object instead of trusting an empty
error string.

### The latency of that path is not small, and it is reported

A `Status` in the code (`VBCableRouter::estimateExtraLatencyMs`) is computed per driver
model and shown as `virtual +N ms`:

| Cable opened as | Extra delay the recording sees |
| --- | --- |
| ASIO bridge | one engine block + one cable block + ~3 ms |
| MME | one engine block + max(2 cable blocks, 150 ms) |
| DirectSound | one engine block + 2 cable blocks + ~20 ms |
| WASAPI | one engine block + 2 cable blocks + ~10 ms (measured 40 ms at 48 kHz / 480) |

Do not expect low-latency recording. The monitor path is the fast one; the virtual path
adds its own buffer and, in a DAW, a further input-buffer round trip on top.

---

## The effects

24 types, each documenting the `juce::dsp` class or the algorithm it uses: 22
built-in DSP pedals, one composed Custom Chain, and one VST3 Plugin Host. Default
chain: Noise Gate → Compressor → 3-Band EQ → Overdrive → Delay → Reverb → Cabinet →
Looper. A type can appear once (one instance each), and the chain holds 16.

| Effect | Implementation | Knobs |
| --- | --- | --- |
| Noise Gate | hand-written envelope follower with a second close threshold and a detector-only sidechain high pass - `juce::dsp::NoiseGate` has none of these | Threshold, Attack, Release, Hold, Range, Mix, Hysteresis, Sidechain |
| Compressor | hand-written feed-forward RMS - `juce::dsp::Compressor` has no Knee, Makeup or sidechain; attack starts at the detector's 2 ms window, makeup stops at the +12 dB gain cap | Threshold, Ratio, Attack, Release, Knee, Makeup, Mix, Sidechain |
| 3-Band EQ | in-place RBJ biquads (low shelf / mid bell with FC+Q / high shelf) | Low, Low Freq, Mid, Mid Freq, Mid Q, High, High Freq, Mix |
| Auto Wah | envelope-driven TPT state-variable band-pass with Up/Down direction; **no LFO anywhere in it** | Sensitivity, Min Freq, Max Freq, Q, Attack, Release, Mix, Direction |
| Wah Pedal | TPT SVF swept by the card control or a MIDI CC11/CC1, with Band Pass / Low Pass modes; with no MIDI input open the Position knob drives it and the card says so | Position, Min Freq, Max Freq, Q, Source, Mix, Mode |
| Overdrive | ADAA tanh saturation with tube bias, pre-clip Bass shelf, 720 Hz mid hump, 4x oversampled; +30 dB maximum input gain, **measured 7.6 % THD at its default** so an acoustic stays at edge-of-breakup | Drive, Tone, Level, Mix, Bass |
| Distortion | ADAA piecewise hard clip, post-clip Presence shelf, 4x oversampled; +30 dB maximum gain and a lower break point, **measured 29.5 % THD at its default** (4x the overdrive) | Drive, Tighten, Tone, Level, Mix, Presence |
| Fuzz | ADAA asymmetric clipping, 120 Hz high-pass, pre-gain Bass shelf and 3.5 kHz transistor input roll-off, 4x oversampled | Fuzz, Bias, Tone, Level, Mix, Bass |
| Bitcrusher | bit depth and sample hold, independent, dither and quantisation once per held step, post Tone low-pass; 2x oversampled | Bits (2-16), Downsample (1-100x), Mix, Tone |
| Chorus | one modulated line per channel, sine LFO, stereo phase spread, Catmull-Rom taps - the BBD band limiting and its 120 Hz/8 kHz filters were removed because they made the wet signal thin and gritty | Rate, Depth, Delay, Mix, Spread |
| Flanger | 1-10 ms modulated delay with crossed feedback, quadrature stereo LFO, 180 Hz feedback high-pass and cubic taps; default Depth 85 % sweeps deeper than before | Rate, Depth, Centre, Feedback, Mix, Spread |
| Phaser | hand-written all-pass cascade with a Centre frequency and stereo phase spread; `juce::dsp::Phaser` cannot change its stage count | Rate, Depth, Feedback, Stages (4/6/8/12), Mix, Centre, Spread |
| Tremolo | amplitude modulation, sine/triangle/square, stereo phase spread; default depth 90 % | Rate, Depth, Shape, Spread |
| Vibrato | short delay line modulated in time - pitch moves, level does not - cubic taps and stereo phase spread | Rate, Depth, Centre, Mix, Spread |
| Octave | zero-crossing sub-octave divider + **full-wave rectifier for the upper octave** (Octavia-style, no grain window); **monophonic, and the card says so** | Down, Up, Blend |
| Pitch Shifter | **Signalsmith Stretch 1.3.2** spectral pitch shifter (MIT), 80 ms analysis / 20 ms hop, independent Formant; fixed latency reported and verified against noise correlation | Semitones, Formant, Mix |
| Delay | 1 ms - 2 s, feedback, tone, true equal-time ping-pong, tape wow/flutter; cubic taps and **Mod defaults to 0** so a clean repeat is the default | Time, Feedback, Tone, Mix, Mode, Mod |
| Reverb | Dattorro plate network with pre-delay, damping and modulated tank all-passes | Decay, Damping, Width, Mix, Pre-Delay |
| Cabinet | `juce::dsp::Convolution` with latency-compensated Mix, 3 built-in responses, custom IR loading (**WAV/WAVE/AIFF/AIFC/FLAC/OGG/MP3**) and a persisted selection | Bass, Treble, Mix |
| Ring Modulator | input x internal sine, 20-5000 Hz | Frequency, Level, Mix |
| Looper | 30 s tape, overdub, one undo, allocated once in `doPrepare` | Level, Tone, Mix + Record/Play/Overdub/Clear/Undo |
| Acoustic to Electric | hand-written: 110 Hz high-pass + 190/380 Hz body cuts + 2.8 kHz quack cut + 6.5 kHz speaker roll-off, then square-law even harmonics through `tanh` at 4x oversampling, blended in parallel | Body Cut, Quack Cut, Air Cut, Presence, Drive, Blend, Squash, Mix |
| Custom Chain | one pedal assembled from the Overdrive, Chorus, Delay and Dattorro reverb stages; all four stages are always wired and only their useful controls are exposed, so no knob can leave an undefined routing | Drive, Tone, Chorus, Delay Time, Feedback, Reverb, Decay, Mix |
| Plugin Host | a user-supplied VST3 effect hosted in-process by JUCE's `VST3PluginFormat`; the first 8 automatable parameters (bypass excluded) become knobs that follow the plugin, and the native editor exposes the rest; path and opaque state persisted | Param 1-8 + Load VST3 / Open Editor / Clear |

The three built-in cabinet responses are **modelled**, not microphone captures: shaped
noise bursts with a decaying envelope, sparse early reflections and a direct arrival.
Their measured character, from `node Resources/make_irs.mjs`:

| Response | Longest band | −20 dB decay | Character |
| --- | --- | --- | --- |
| 1x12 V30 closed back | 900-1800 Hz | 75 ms | dark, −24 dB above 7 kHz, boxy low-mid |
| 2x12 open back | 1800-3600 Hz | 55 ms | scooped 160-400 Hz, driest, most top-end leak |
| 4x12 high gain | 80-160 Hz | 110 ms | biggest low end, loosest, presence peak kept under the mids |

Load a measured IR from the Cabinet card for real cabinet tones; the card prompts
`请加载 IR 文件` while no response is installed, and never silently skips.

### Custom effects: what other hosts do, and what this app does

The request was a custom effect assembled from several categories of built-in
controls, plus loading an effect plugin. The common implementations that were
compared:

* **Plugin host with macro controls** - Ableton Live's Macro Controls, NI Maschine
  and Komplete Kontrol macros, Cubase Quick Controls, and patch hosts such as Blue
  Cat PatchWork and Kilohearts Snap Heap. A plugin is loaded, a handful of its
  parameters are mapped to 8 knobs, and the native editor is still available.
* **Modular builders** - VCV Rack, Voltage Modular, Reaktor Blocks. These expose
  arbitrary node graphs with dynamic routing, which is a much larger design than a
  16-slot serial pedalboard and would require a graph editor and graph serialisation.
* **Curated multi-effect pedals** - Line 6 Helix blocks and the Boss GT series.
  A fixed set of stages with only the useful controls on the panel; the player never
  wires the routing.

This app implements the first and third patterns, which fit a guitar chain:

**Custom Chain** is the curated-pedal pattern. It is a real `PedalEffect` composed of
the existing Overdrive, Chorus, Delay and Reverb classes. All four stages are always
in circuit; the eight knobs drive their documented controls, so the result cannot be
half-wired. Its reported latency is the Overdrive's 4x oversampler latency, and its
global Mix is compensated against exactly that, so Mix below 100 % is a blend rather
than a comb filter. The bench covers its defaults, both extremes of every knob and
Mix = 0 alongside the built-in pedals.

**Plugin Host** is the macro-host pattern. It loads a `.vst3` file or bundle through
`VST3PluginFormat` and `AudioPluginFormatManager` and hosts it in-process:

* Scanning, instantiation and state I/O run on the message thread. The audio thread
  reads one atomic `Hosted*` and copies the block into a prepared 2-channel buffer.
  Loading another plugin prepares the new instance first, then swaps the pointer;
  retired instances stay alive until the effect dies, so a block already inside the
  old plugin cannot crash on a dangling pointer. There is no lock or allocation on
  the audio thread.
* The eight knobs map the first eight **automatable, non-bypass** parameters. The
  bypass exclusion was added after the probe found that Panagement 2 declares
  `Bypass` as parameter 0: the naive mapping made knob 1 a mute switch. Knob moves
  call `setValueNotifyingHost` on the message thread; each processed block copies the
  plugin's values back, so the card follows the native editor.
* The native editor is a real top-level window owned by the effect and closed before
  the plugin instance is destroyed. The plugin's opaque state is saved to
  `PedalBoardData/customPlugin.state` - prefixed with the plugin's own path, so a
  file left by one plugin can never be fed to another - and the path is saved to
  `PedalBoardData/PedalBoard.settings`. This is what captures changes made in the
  editor, not just the eight macros.
* The plugin's reported latency joins the engine's dry-path compensation like any
  built-in effect. More than stereo input/output is rejected with a message rather
  than silently folded. A missing file, a non-effect plugin, an instrument, or a file
  with no VST3 class leaves the previous plugin (if any) running and prints the
  reason on the card.

Measured with the installed **Auburn Sounds Panagement 2** (46 parameters, 3 samples
latency): load and editor ✅, eight macros excluding bypass ✅, first macro write
`Far` 0.8000 → 0.5000 and the editor's FAR knob moved ✅, plugin state 382 bytes and
round-trip ✅ (saved file 455 bytes including the path header), restart with restored
path/state ✅, clean exit in 0.85 s with the editor open ✅.
`_tests/vst3_probe.cpp` performs the same load without the UI and reports the
parameter table, a non-zero processing difference and the state size; it exists so
"the plugin card says it loaded" can be checked independently of the card.

Honest limits: only VST3 is hosted (no VST2, LV2 or AU); loading is in-process and
unsandboxed, so a crashing plugin can take PedalBoard with it; a slow plugin scan
blocks the message thread while it runs; the eight knobs are the first eight
automatable parameters, not a user-editable mapping; and the plugin file itself is
read from wherever the user installed it - the app neither bundles, installs nor
copies it. The `Plugins` folder next to the executable is only the file dialog's
starting point.

Research sources:
[JUCE AudioPluginFormatManager](https://docs.juce.com/master/classjuce_1_1AudioPluginFormatManager.html) ·
[JUCE AudioPluginInstance](https://docs.juce.com/master/classjuce_1_1AudioPluginInstance.html) ·
[JUCE tutorial: hosting VST3 plugins](https://docs.juce.com/master/tutorial_plugin_host.html) ·
[NI Maschine macro controls](https://www.native-instruments.com/en/products/maschine/maschine-expansions/) ·
[Blue Cat PatchWork](https://www.bluecataudio.com/Products/Product_PatchWork/) ·
[Kilohearts Snap Heap](https://kilohearts.com/products/snap_heap)

### Borrowed designs and what they measured

The four families (time, distortion, filter, dynamics) were rebuilt against
published and open implementations rather than invented from scratch:

* **Reverb** - Jon Dattorro, *Effect Design, Part 1: Reverberator and Other
  Filters* (JAES 1997), figure 8. Delay lengths (142/107/379/277 input
  diffusers; 672/4453/1800/3720 and 908/4217/2656/3163 tank) and the seven
  output taps per side are the paper's tunings, scaled to the device rate. The
  tap table was cross-checked against the Freeverb3 implementation notes.
* **Wah / Auto Wah** - topology preserving transform state-variable filter
  (Zavalishin, *The Art of VA Filter Design*; Cytomic's SVF), which stays valid
  while the cutoff moves. Q makeup is 1/sqrt(Q) relative to the SVF's Q-times
  band gain.
* **Overdrive** - tube-screamer-style fixed 720 Hz mid hump after the clipper;
  **Fuzz** - Fuzz Face input capacitance modelled as a 3.5 kHz roll-off before
  the gain stage; **octave up** - the full-wave rectifier used by the Octavia
  and Tysegue, with a 6 kHz input filter and a 4.8 kHz output filter to keep the
  folded harmonics out of the alias band. Circuit analyses: Electrosmash (Tube
  Screamer, Fuzz Face, Octavia), anasounds CE-2 teardown, Electric Druid's BBD
  clock notes.
* **Pitch Shifter** - Signalsmith Stretch 1.3.2 (MIT), configured for spectral
  pitch/formant shifting at 80 ms block / 20 ms hop. This replaces the
  hand-written granular engine. Measured: 110 Hz +12 st -> 220.50 Hz, leak
  -105 dB; reported 80 ms latency matches noise cross-correlation exactly at
  44.1 and 48 kHz.
* **Chorus / Vibrato / Flanger / Delay taps** - Catmull-Rom reads. A linearly
  interpolated tap is a moving low-pass; on a modulated delay that adds audible
  HF grit, which is what the "8-bit" chorus and the "distorted" delay were.
* **Clippers** - first order antiderivative anti-aliasing (Chowdhury,
  *Practical Considerations for Antiderivative Anti-Aliasing*), applied inside
  the existing 4x oversampling.
* **Compressor** - Giannoulis, Massberg and Reiss, *Digital Dynamic Range
  Compressor Design - A Tutorial and Analysis*: dB-domain attack/release
  follower and the quadratic soft knee. **Gate** - the second close threshold
  that every hardware gate/expander uses, exposed as Hysteresis.

`dsp_probe` measures the borrowed properties directly:

| Measurement | Result |
| --- | --- |
| Reverb: 50 ms Pre-Delay moves the first arrival | 50.0 ms (sample 290 -> 2690 at 48 kHz) |
| Reverb: short Decay dies away | 5.15e-3 (0.05-0.2 s) -> 2.15e-6 (1.8-2.0 s) |
| Reverb: Width 0 / Width 100 | max L-R difference 0.00e+00 / 0.170 vs 0.120 signal |
| Delay ping-pong | L@100 ms 0.500, R@100 ms 0.000, R@200 ms 0.235, L@200 ms 0.000 |
| Gate hysteresis 12 dB vs 0 dB at -35 dBFS | 0.0126 vs 0.0000 held level; both close at -45 dBFS |
| Gate sidechain, 60 Hz hum | opens with a 20 Hz sidechain, rejected at 300 Hz |
| Compressor sidechain, 60 Hz bass | 29.0 dB reduction at 20 Hz vs 12.5 dB at 500 Hz |
| Wah Low Pass vs Band Pass at 100 Hz | 0.096 vs 0.029 |
| Overdrive / Fuzz Bass -12 -> +12 dB at 100 Hz | 0.012 -> 0.095 / 0.0080 -> 0.0498 |
| Distortion Presence -12 -> +12 dB at 4 kHz | 0.062 -> 0.438 |
| Bitcrusher Tone 0 vs 100 on noise | 0.020 vs 0.089 |
| ADAA tanh alias suppression | 15 kHz -12.9 dB, 7 kHz -11.8 dB |
| Chorus Spread 0 / 100 | max L-R difference 0.00e+00 / 0.097 vs 0.069 signal |
| Drive level, min/default/max | Overdrive 0.052/0.235/0.238, Distortion 0.054/0.202/0.224, Fuzz 0.093/0.204/0.207 |

A settings-migration bug was found while verifying this: an older settings line
ends with the bypass marker `b0`, and the loader was reading that marker as the
newly appended parameter's value (Hysteresis loaded as 0 instead of 3 dB). It
now stops at the first `b...` token, so appended knobs keep their defaults.

Sources:
[Dattorro 1997](https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf) ·
[ADAA](https://ccrma.stanford.edu/~jatin/Notebooks/adaa.html) ·
[Compressor tutorial](https://www.researchgate.net/publication/277772168_Digital_Dynamic_Range_Compressor_Design-A_Tutorial_and_Analysis) ·
[CE-2 analysis](https://anasounds.com/working-analog-chorus-effect-boss-ce2/) ·
[BBD clock notes](https://electricdruid.net/weird-wiggles-bbd-clock-modulation/) ·
[Freeverb3 reverb notes](https://freeverb3-vst.sourceforge.io/doc/Reverberation%20algorithms.pdf) ·
[Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) ·
[Octavia circuit notes](https://www.electrosmash.com/tube-screamer-analysis)

---

## Deviations from the brief, verified against JUCE 8.0.15

Each of these is a real gap in the framework, not a shortcut. The header of every
affected file says which JUCE file was read to establish it.

1. **`juce::dsp::PitchShifter` does not exist in JUCE 8** (it was removed with the old
   `juce::PitchShifter` processor). → Signalsmith Stretch (MIT) for the spectral pitch
   shifter, a zero-crossing divider down and a full-wave rectifier up for the octave.
   *Boundary:* the octave remains monophonic; the pitch shifter tracks chords but has
   the 80 ms latency the card reports.
2. **`juce::dsp::NoiseGate` exposes only Threshold/Attack/Release** - no Hold, no Range
   (`widgets/juce_NoiseGate.h`). → hand-written envelope-follower gate.
3. **`juce::dsp::Compressor` exposes only Threshold/Ratio/Attack/Release** - no Knee, no
   Makeup (`widgets/juce_Compressor.h`). → hand-written compressor with soft knee and
   makeup, so the brief's six parameters actually exist.
4. **`juce::dsp::Phaser` cannot change its stage count** (Rate/Depth/Centre/Feedback/Mix
   only). → hand-written all-pass cascade with a 4/6/8/12 selector.
5. **`IIR::Coefficients` allocation on every coefficient update** → the EQ, auto-wah,
   wah, cabinet tone and delay tone use in-place RBJ biquads from `PedalDsp.h`, which
   refresh coefficients on a counter rather than rebuilding a ref-counted object.
6. **`AudioIODevice` has no latency-in-seconds getter** → `getInputLatencyInSamples()` /
   `getOutputLatencyInSamples()` are converted with the device rate, and the buffer
   time is added explicitly. Physical and virtual delays are shown separately.
7. **`juce::dsp::Oversampling`'s constructor takes a stage count, not a ratio, and the
   ratio is fixed at `prepare`** → oversampling factor is chosen per effect at prepare
   time (4x for the drives, 2x for the crusher) and never changed on the fly.
8. **`LagrangeInterpolator` is now `Interpolators::Lagrange`** with a different
   `process` contract (returns samples consumed) → both sample-rate bridges
   (input → chain, chain → cable) keep a fractional carry so two clocks cannot drift.
9. **`Convolution::loadImpulseResponse` only queues the request**; its own background
   thread does the reading, resampling, trimming and FFT. → the cabinet gates audio on
   `getCurrentIRSize()`/`getLatency()`, which is the engine's real state, so "loading"
   is never faked and a failed load is visible instead of silent.
10. **`Component::removeFromParent()` no longer exists**; `removeChildComponent()`
    detaches without deleting (verified in `juce_Component.h`) → child lifetimes are
    explicit in `MainComponent`.
11. **`JUCEApplication` moved** from `juce_gui_extra` to
    `juce_gui_basics/application/juce_Application.h` → `Main.cpp` includes
    `juce_gui_basics` only.
12. **`ComboBox`'s `bool` overloads are `[[deprecated]]`** → all programmatic selection
    changes pass `juce::dontSendNotification`, so populating a box never re-enters the
    callback it was called from.
13. **`LookAndFeel_V2`'s theme setters are gone** (`setDefaultComboBoxProperties`,
    `setAlertWindowTheme`, `setDefaultScrollbarColour`, `setSliderStyleInformation` do
    not exist in 8.0.15) → `setColourScheme()` plus per-widget `setColour()`.
14. **`Graphics::drawArc` / `fillArc` are not in this version** → the knob arc is
    `Path::addCentredArc` + `strokePath`, with the same 0.75π-2.25π sweep convention
    `LookAndFeel_V4::drawRotarySlider` uses.
15. **The knobs and switches are hand-drawn, not `juce::Slider` subclasses.** JUCE 8
    gives no supported way to make a `Slider` stop painting its own arc, so a hidden
    Slider inside a custom component needs a private no-op LookAndFeel - fragile, and
    impossible to eyeball without a compiler. Drag maths is implemented directly
    (300 px full range, Shift for 5x finer, snap to interval, double-click for default,
    arrow keys when focused). *Same reasoning for the discrete step selectors.*
16. **Device selectors are themed `juce::ComboBox` widgets in a hand-made layout, not
    hand-drawn dropdowns.** The brief's actual requirements - input and output fully
    independent, no `AudioDeviceSelectorComponent`, each manager reconfigured through
    its own `setAudioDeviceSetup()` - are met; re-implementing popup lists, keyboard
    navigation and DPI behaviour for a dropdown bought nothing.
17. **`juce::String::operator bool` is private** - declared, as its comment says, to
    stop strings being converted by accident. So `if (auto error = manager.initialise(...))`
    does not compile; every error string here is tested with `isNotEmpty()` in an
    `if`-initializer. (`juce_core/text/juce_String.h`, in the private section.)
18. **`juce_add_binary_data` takes `SOURCES`, not `SOURCE_DIR`**, in 8.0.15 -
    `cmake_parse_arguments` there only knows `NAMESPACE`, `HEADER_NAME` and `SOURCES`,
    and a dropped keyword is silent until the "must be passed at least one file to
    encode" error. The generated length symbol is `<name>Size`, not `<name>_length`:
    that is what `extras/Build/juce_build_tools/utils/juce_BinaryResourceFile.cpp`
    emits, and it is what `CabinetEffect.h` now uses.
19. **`juce_add_gui_app` ignores unknown metadata keywords.** `PRODUCT_MANUFACTURER`,
    `BINARY_NAME` and `COMPANY_WEBSITE_URL` are not in `_juce_initialise_target`'s list
    and are silently dropped; the real ones used here are `PRODUCT_NAME`, `COMPANY_NAME`,
    `COMPANY_WEBSITE` and `COMPANY_COPYRIGHT`.
20. **No kernel driver, no self-written virtual audio device**, per the brief: signing
    and WHQL are not feasible here, so VB-CABLE is used and disclosed as an external
    donation-ware dependency.
21. **Plugin hosting is VST3-only and in-process.** JUCE 8's hosting classes cover
    VST3 well on Windows, but there is no sandbox: a plugin that crashes in
    `processBlock` or in its editor takes the app with it, and a slow scan blocks the
    message thread. VST2/LV2/AU are not wired in. The eight macros map the first eight
    automatable non-bypass parameters in the plugin's order; there is no
    user-editable mapping matrix yet. *Boundary:* the editor exposes every control
    the plugin publishes, and the plugin's own state is persisted, so the macro map is
    a convenience rather than the only way in.

---

## Audio thread rules the code actually follows

The constraints are structural, not aspirational:

* **Zero allocation in both callbacks.** Every engine buffer, every effect scratch
  buffer, every delay line, the looper tape and the dry compensation line are sized in
  the constructor or in `prepare()`, which runs while the device is stopped.
* **Zero locks.** Two SPSC rings + one seqlock + atomics. Overflow drops the oldest
  samples and advances the consumer deliberately; underflow zero-fills. Both are
  counted and surfaced in the status bar rather than hidden behind a wait; the one
  exception is the first read of a new stream, which is the stream starting rather
  than a consumer starving.
* **Zero file I/O.** IR loading is queued to `ConvolutionMessageQueue`; an external IR
  `File` is published through a two-slot ping-pong with an atomic index, and the
  existence test happens on the message thread.
* **Parameters are `std::atomic<float>`.** The message thread writes, the audio thread
  relaxed-loads. `setParameters`-style calls that mutate smoothed state are only made
  when a value actually moved (see the reverb and chorus parameter caches).
* **Bypass is a `continue`** at the top of the chain loop - nothing is connected or
  disconnected at run time.
* **Only the owner of a ring head may move it.** `reset()` reallocates and rewrites
  both heads, so it is legal only in the engine constructor, when no device is
  running. After an input-device switch the message thread merely sets a flag and the
  audio thread calls `flushAsConsumer()`; the cable's backlog is dropped by the cable
  thread in its own `audioDeviceAboutToStart()`. Flushing from the wrong thread is a
  race on a live queue, and a stale backlog at the old sample rate would otherwise
  burst into the speakers or the recording.
* **A block longer than `maxBlockSize` zeroes the outputs** instead of returning early,
  so an over-sized callback never leaves stale audio on the wire.
* **The chain buffers describe the callback, not their capacity.** `chainBuffer`,
  `dryBuffer` and `mixedBuffer` are allocated for `maxBlockSize` once and resized to
  `numSamples` with `avoidReallocating` at the top of every callback, because effects
  and the virtual push all read `getNumSamples()`. Leaving them at 2048 is runtime
  defect 9: 1568 samples of stale audio per block and a virtual ring pushed four times
  faster than the cable drained it.
* **File I/O in the UI** (the settings file, plugin state) happens only on the message
  thread.
* **The hosted plugin is behind one atomic pointer.** `PluginHostEffect` scans,
  instantiates, saves and restores state on the message thread; the audio thread only
  loads the pointer, copies the block into a preallocated stereo buffer, calls
  `processBlock`, and copies it back. Loading a new plugin publishes a prepared
  instance last; the previous one is kept alive because a block may still be inside
  it. The eight macro atomics are read/written from the audio thread without locks.
  The plugin's own `processBlock` is outside this project's control - a plugin that
  allocates or blocks in that callback behaves as badly here as in any DAW.

Settings live in `PedalBoardData/PedalBoard.settings` next to the executable - plain
`key=value` text - specifically so nothing is written to `%APPDATA%` and the D:-only
rule holds. The loaded plugin's path is one key in that file; its opaque binary state
is the sibling `PedalBoardData/customPlugin.state`, so a change made in the plugin's
own editor survives a restart even though it never touches the eight macro atomics.

---

## UI

One window, dark theme, no `AudioDeviceSelectorComponent`.

* **Top bar.** Input type + input device, output type + output device (four independent
  selectors), buffer size and sample rate with the buffer shown in ms, a Devices button.
  Second row: the big **MONITOR** switch, **TO CABLE**, global Dry/Wet, Input Trim,
  Expression (read-only while a MIDI pedal is driving it), and two peak meters - one for
  the physical path, one for the virtual path.
* **Chain.** Vertical column of cards in a scrollable viewport. Each card: slot number,
  title, bypass, up/down/remove, its 2-8 knobs or step selectors, an action row where the
  effect needs one (Looper transport, Cabinet IR picker), and a status/warning line. Drag
  a card by its header strip to reorder; the drop position is shown as a bar and the
  engine is touched exactly once, on release.
* **Status bar.** VB-CABLE state on the first line (green when streaming), then the four
  latencies separated - input / monitor / chain (compensated) / virtual - with the driver
  model that produced the estimate, then CPU, sample rate, block size, slot count and the
  input drops split into overflow (the producer ran ahead) and underflow (the consumer
  starved) plus the virtual drops split the same way, then any start-up problem
  verbatim. All four stay at zero while the engine is healthy; a non-zero overflow and
  a non-zero underflow have different causes and different fixes, which is why they are
  not summed.
* **First run.** The language question, then the device panel: it appears before anything
  is assumed, lists what each device type actually reports, rescans on demand, shows the
  VB-CABLE licence notice when the cable is missing, and carries the interface-language
  selector in its button row so the language can be changed later without hunting for a
  settings dialog.

## Known limitations

* It has been compiled, run and captured, but **never heard**: no listening test is
  possible from this machine, so every claim about sound is a measurement or an
  inspection, not an audition.
* `fp:fast` is on for the release build; that is deliberate for the DSP and would be a
  problem if anything here needed bit-exact maths. It does not.
* The looper's undo is one level deep, and tape length is fixed at 30 s.
* Latency figures are driver-reported where the driver tells the truth (WASAPI/ASIO) and
  buffer-derived where it does not (MME).
* Reordering during playback can cost one block at the switch point; the seqlock makes
  it a torn-block risk, not a race, and the effect objects are never destroyed.
* **Acoustic to Electric is a colouring filter plus harmonic excitation, not a
  conversion.** Its corner and cut frequencies (110 Hz high-pass, 190/380 Hz body
  cuts, 2.8 kHz quack cut, 6.5 kHz roll-off) come from published measurements of
  acoustic body resonances and piezo quack, so they are starting points for a
  generic instrument rather than a deconvolution of *your* guitar. No equaliser can
  give a vibrating string the transient behaviour of a magnet under it, and the two
  body notches are fixed frequency: a guitar whose first mode sits elsewhere needs
  the 3-Band EQ in front. It has never been listened to.
