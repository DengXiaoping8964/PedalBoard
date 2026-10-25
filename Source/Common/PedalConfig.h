#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

/*==============================================================================
  PedalBoard - global compile-time constants.

  Everything that sizes a preallocated realtime buffer lives here, so the audio
  path never has to guess how much memory it may touch.
==============================================================================*/
namespace pedal
{

/** Every effect type is instantiated exactly once at engine construction and
    lives for the lifetime of the process. Reordering the chain therefore only
    republishes pointers, which is what makes the realtime path allocation free. */
enum EffectTypeID : int
{
    noiseGate = 0,
    compressor,
    eq3Band,
    autoWah,
    wah,
    overdrive,
    distortion,
    fuzz,
    bitcrusher,
    chorus,
    flanger,
    phaser,
    tremolo,
    vibrato,
    octaver,
    pitchShifter,
    delay,
    reverb,
    cabinet,
    ringModulator,
    looper,
    solidBody,
    customChain,
    pluginHost,
    numEffectTypes
};

/** Number of ordered slots in the signal chain. */
static constexpr int maxChainSlots = 16;

/** Parameters per effect card (knobs), including the Mix knob where it applies. */
static constexpr int maxParamsPerEffect = 8;

/** The chain always runs internally as stereo, even when the guitar is mono. */
static constexpr int chainChannels = 2;

/** Blocks never exceed this, so all delay lines and FIR buffers are sized for it. */
static constexpr int maxBlockSize = 2048;

/** Sample rate range the app is prepared for. */
static constexpr double minSampleRate = 44100.0;
static constexpr double maxSampleRate = 192000.0;

/** Capacity of the input ring buffer, in blocks of maxBlockSize.

    One extra constraint decides this number: when the input device runs at a
    higher rate than the monitor device, one output block has to drain
    maxSampleRate / minSampleRate = 192000 / 44100 = 4.35 input samples per output
    sample, so a single read can ask for about 8900 samples. The ring is a power
    of two, so eight blocks of 2048 is the smallest capacity that can never be
    exceeded by one read, which is what keeps the wrap arithmetic valid. */
static constexpr int inputRingBlocks = 8;

/** Capacity of the ring buffer that feeds the VB-CABLE output stream. Same
    reasoning: the virtual device may run at a different rate and clock. */
static constexpr int virtualRingBlocks = 8;

/** The longest read one output block can ever ask the input ring for.
    maxSampleRate / minSampleRate = 4.35 input samples per output sample, against a
    block of maxBlockSize, rounded up to a factor of five. Both rings are sized from
    this, and the wrap arithmetic in AudioRingBuffer is only valid while a single
    read cannot exceed the capacity - which is why the number lives here rather than
    inside the engine: the test bench has to size the same ring the app does. */
static constexpr int maxInputReadPerBlock = maxBlockSize * 5;

/** Ring capacity, in samples per channel, for a ring sized in blocks of maxBlockSize. */
constexpr double ringCapacityFor (int blocksOfMaxBlock) noexcept
{
    return static_cast<double> (maxBlockSize) * blocksOfMaxBlock;
}

/** Maximum recorded loop length for the looper. */
static constexpr double maxLoopSeconds = 30.0;

/** How many impulse-response slots the cabinet simulator exposes. */
static constexpr int numBuiltInIRs = 3;

/** Smallest window the interface can be shrunk to, and the height that goes with
    it. Both live here rather than in the window class because two files need to
    agree on them: DocumentWindow's resize limits, and the start-up sizing that
    fits the window to whatever the display actually offers. Two copies of 1040
    and 700 drifted apart once already.

    The width is not aesthetic. The device row is six fixed-width selectors plus
    the Devices button and measures 1066 logical pixels end to end; anything
    narrower and either the type names or the buffer label ("480 (10.0 ms)") get
    elided, which is exactly what the first builds on this machine did. */
static constexpr int minWindowWidth  = 1080;
static constexpr int minWindowHeight = 700;

/** Device name fragments used to recognise a VB-CABLE installation. */
static constexpr const char* cableOutputDeviceHint = "CABLE Input";
static constexpr const char* cableInputDeviceHint  = "CABLE Output";

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept  { return 20.0f * std::log10 (g + 1.0e-9f); }

} // namespace pedal
