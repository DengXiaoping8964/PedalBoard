#include "Effects/EffectFactory.h"

#include <vector>
#include <memory>

namespace pedal
{

EffectFactory::EffectFactory (const EffectContext& context)
{
    auto& slot = instances;

    slot[noiseGate]      = std::make_unique<NoiseGateEffect>();
    slot[compressor]     = std::make_unique<CompressorEffect>();
    slot[eq3Band]        = std::make_unique<EQ3BandEffect>();
    slot[autoWah]        = std::make_unique<AutoWahEffect>();
    slot[wah]            = std::make_unique<WahEffect> (context);
    slot[overdrive]      = std::make_unique<OverdriveEffect>();
    slot[distortion]     = std::make_unique<DistortionEffect>();
    slot[fuzz]           = std::make_unique<FuzzEffect>();
    slot[bitcrusher]     = std::make_unique<BitcrusherEffect>();
    slot[chorus]         = std::make_unique<ChorusEffect>();
    slot[flanger]        = std::make_unique<FlangerEffect>();
    slot[phaser]         = std::make_unique<PhaserEffect>();
    slot[tremolo]        = std::make_unique<TremoloEffect>();
    slot[vibrato]        = std::make_unique<VibratoEffect>();
    slot[octaver]        = std::make_unique<OctaveEffect>();
    slot[pitchShifter]   = std::make_unique<PitchShifterEffect>();
    slot[delay]          = std::make_unique<DelayEffect>();
    slot[reverb]         = std::make_unique<ReverbEffect>();
    slot[cabinet]        = std::make_unique<CabinetEffect> (context);
    slot[ringModulator]  = std::make_unique<RingModulatorEffect>();
    slot[looper]         = std::make_unique<LooperEffect>();
    slot[solidBody]      = std::make_unique<SolidBodyEffect>();
    slot[customChain]    = std::make_unique<CustomChainEffect>();
    slot[pluginHost]     = std::make_unique<PluginHostEffect>();
}

const std::vector<EffectMetadata>& EffectFactory::metadata()
{
    static const std::vector<EffectMetadata> table =
    {
        { noiseGate,      "Noise Gate",        "Dynamics",   "Envelope follower gate: Threshold, Attack, Release, Hold, Range." },
        { compressor,     "Compressor",        "Dynamics",   "Feed-forward RMS compressor with soft knee and makeup gain." },
        { eq3Band,        "3-Band EQ",         "Filter",     "Low shelf, mid bell with adjustable frequency and Q, high shelf." },
        { autoWah,        "Auto Wah",          "Filter",     "Playing dynamics sweep a band-pass. No LFO: the envelope is the pedal." },
        { wah,            "Wah Pedal",         "Filter",     "Band-pass swept by the card slider or a MIDI expression pedal." },
        { overdrive,      "Overdrive",         "Distortion", "tanh soft saturation, 4x oversampled. Tube-like, keeps the pick attack." },
        { distortion,     "Distortion",        "Distortion", "Piecewise hard clipping, 4x oversampled, tighter and more compressed." },
        { fuzz,           "Fuzz",              "Distortion", "Asymmetric silicon-style clipping with a 120 Hz input high-pass." },
        { bitcrusher,     "Bitcrusher",        "Distortion", "Independent bit depth (2-16) and sample hold (1-100x), 2x oversampled." },
        { chorus,         "Chorus",            "Modulation", "juce::dsp::Chorus: two tapped voices, fixed 50% mix, no feedback." },
        { flanger,        "Flanger",           "Modulation", "1-10 ms modulated delay with crossed feedback: comb, not shimmer." },
        { phaser,         "Phaser",            "Modulation", "All-pass cascade with feedback; 4, 6, 8 or 12 stages." },
        { tremolo,        "Tremolo",           "Modulation", "Amplitude modulation: sine, triangle or square, rate and depth." },
        { vibrato,        "Vibrato",           "Modulation", "Short delay line modulated in time: pitch moves, level does not." },
        { octaver,        "Octave",            "Pitch",      "Zero-crossing divider for -12 semitones, granular shifter for +12." },
        { pitchShifter,   "Pitch Shifter",     "Pitch",      "Granular overlap-add, continuous -12 to +12 semitones." },
        { delay,          "Delay",             "Time",       "1 ms to 2 s, feedback, tone and ping-pong modes." },
        { reverb,         "Reverb",            "Time",       "juce::dsp::Reverb: room size, damping, width and wet/dry." },
        { cabinet,        "Cabinet Simulator", "Time",       "Partitioned convolution with three built-in responses plus IR loading." },
        { ringModulator,  "Ring Modulator",    "Special",    "Input multiplied by a 20-5000 Hz sine: no fundamental, only sums." },
        { looper,         "Looper",            "Special",    "30 s tape, overdub, one undo level, allocated once at startup." },
        { solidBody,      "Acoustic to Electric", "Conversion", "Kills body resonance and piezo quack, adds even-order harmonics and squash." },
        { customChain,    "Custom Chain",      "Custom",     "Eight knobs across drive, tone, chorus, delay and reverb in one pedal." },
        { pluginHost,     "Plugin Host",       "Custom",     "Hosts one VST3 effect; eight macros plus the plugin's own editor." },
    };

    return table;
}

const EffectMetadata* EffectFactory::metadataFor (int type) noexcept
{
    for (auto& m : metadata())
        if (m.type == type)
            return &m;

    return nullptr;
}

const std::vector<const char*>& EffectFactory::categories()
{
    // Every category string used by metadata() has to appear here: the "add
    // effect" menu walks this list and only shows items whose category matches,
    // so a missing entry hides that effect completely rather than misfiling it.
    static const std::vector<const char*> list = { "Dynamics", "Filter", "Distortion",
                                                   "Modulation", "Pitch", "Time", "Special",
                                                   "Custom", "Conversion" };
    return list;
}

const std::vector<EffectTypeID>& EffectFactory::defaultChain()
{
    static const std::vector<EffectTypeID> order =
    {
        noiseGate, compressor, eq3Band, overdrive, delay, reverb, cabinet, looper
    };

    return order;
}

} // namespace pedal
