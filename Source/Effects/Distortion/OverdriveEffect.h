#pragma once

#include "Common/PedalOversampler.h"
#include "Effects/PedalEffect.h"

#include <array>
#include <cmath>

/*==============================================================================
  Overdrive

  Transfer curve: ADAA-smoothed tanh soft saturation with a small DC bias, which
  is what a tube stage does: the positive and negative halves bend around
  different points, so the curve generates even harmonics as well as odd ones
  while staying continuously differentiable. The bias is removed again by the
  output DC blocker. It is the only one of the three gain pedals without a break
  point, which is why it still sounds like an amp being pushed rather than like
  a fuzz.

  The clipper is first order antiderivative anti-aliased (pedal::adaa::Tanh,
  following Chowdhury): the curve is integrated over each oversampled interval
  instead of sampled at one point, which removes the aliasing that 4x
  oversampling alone still leaves at high drive. The curve itself is fixed and
  drive is input gain, so the ADAA history stays valid when the knob moves.

  Voicing: a fixed 90 Hz high pass before the clipper keeps the low strings from
  turning into mud; a Bass shelf at 150 Hz before the gain stage decides how much
  low end drives the clipper, which is the difference between a tight rhythm
  pedal and a full-range boost. After the clipper a 720 Hz peaking section gives
  the mid-forward voice of the classic tube-screamer family, and a second order
  tone low pass (500 Hz to 15 kHz) is the pedal's tone control.

  4x oversampling (2 half band stages) is mandatory here. Squaring a full scale
  signal generates harmonics that fold back into the audible band without it.
  The oversampler's integer latency makes getLatencySamples() exact, and the
  pedal's own dry copy runs through the same delay so Mix 50 % is a blend and
  not a comb filter.
==============================================================================*/
namespace pedal
{

class OverdriveEffect : public PedalEffect
{
public:
    enum Params { drive, tone, level, mix, bass, numParams };

    OverdriveEffect()
        : PedalEffect (overdrive, "Overdrive",
            {
                { "Drive",   0.0f, 100.0f, 35.0f },
                { "Tone",    0.0f, 100.0f, 55.0f },
                { "Level",   0.0f, 200.0f, 45.0f, "%" },
                { "Mix",     0.0f,   1.0f,  1.0f },
                { "Bass",  -12.0f,  12.0f,  0.0f, "dB" },
            })
    {
    }

    void doPrepare (double, int) override
    {
        oversampler.prepare (chainChannels, blockSize, 2);   // 4x
        oversampleRate = oversampler.oversampledRate (sampleRate);

        for (auto& filter : inputFilter)
        {
            filter.makeHighPass (oversampleRate, 90.0, 0.7071);
            filter.reset();
        }

        for (auto& filter : toneFilter)
            filter.reset();

        for (auto& shelf : bassShelf)
        {
            shelf.makeLowShelf (oversampleRate, 150.0, getParameter (bass), 0.8);
            shelf.reset();
        }

        cachedBassDb = getParameter (bass);
        cachedBassRate = oversampleRate;

        for (auto& hump : midHump)
        {
            hump.makePeak (oversampleRate, 720.0, 0.9, 2.5);
            hump.reset();
        }

        for (auto& blocker : dcBlocker)
        {
            blocker.makeHighPass (oversampleRate, 9.0, 0.7071);
            blocker.reset();
        }

        prepareLatencyCompensatedDry (getLatencySamples());
    }

    void resetState() noexcept override
    {
        oversampler.reset();
        for (auto& filter : inputFilter) filter.reset();
        for (auto& filter : toneFilter)  filter.reset();
        for (auto& shelf : bassShelf)    shelf.reset();
        for (auto& hump : midHump)       hump.reset();
        for (auto& blocker : dcBlocker)  blocker.reset();
        for (auto& clipper : shaper)     clipper.reset();
    }

    int getLatencySamples() const noexcept override { return oversampler.getLatencySamples(); }

    void process (juce::AudioBuffer<float>& audio) override
    {
        auto& wet = wetBufferForBlock();

        for (int c = 0; c < chainChannels; ++c)
            wet.copyFrom (c, 0, audio, c, 0, audio.getNumSamples());

        updateBass();

        /*  +30 dB at the top, not +44. The old curve put +24 dB in at Drive 55,
            which turned a -20 dBFS acoustic into a fully saturated fuzz and made
            this pedal the heavier of the two. The new default (Drive 35) sits at
            +10.5 dB input gain, which is edge-of-breakup territory. */
        const auto driveAmount = getParameter (drive);
        const auto preGain  = std::pow (10.0f, driveAmount * 0.015f);
        const auto hardness = 1.0f + driveAmount * 0.03f;
        const auto outLevel = getParameter (level) * 0.005f;   // 200 % is a +6 dB ceiling, not +14 dB
        const auto toneHz   = 500.0f * std::pow (30.0f, getParameter (tone) * 0.01f);

        // A tube stage is not symmetric: the two halves of the waveform bend
        // around different operating points, which adds even harmonics to the
        // odd ones. Subtracting the curve's value at the bias keeps the output
        // centred; the 9 Hz blocker removes whatever DC is left.
        constexpr float bias = 0.10f;
        const auto biasTerm = std::tanh (hardness * bias) / std::tanh (hardness);

        const auto block = oversampler.up (wet, audio.getNumSamples());
        const auto numSamples = static_cast<int> (block.getNumSamples());

        for (int c = 0; c < static_cast<int> (block.getNumChannels()); ++c)
        {
            auto* data = block.getChannelPointer (static_cast<size_t> (c));
            auto& hp   = inputFilter[static_cast<std::size_t> (c)];
            auto& tone = toneFilter[static_cast<std::size_t> (c)];
            auto& hump = midHump[static_cast<std::size_t> (c)];
            auto& dc   = dcBlocker[static_cast<std::size_t> (c)];
            auto& clipper = shaper[static_cast<std::size_t> (c)];

            clipper.setScale (hardness);
            tone.makeLowPass (oversampleRate, toneHz, 0.7071);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto excited = hp.process (bassShelf[static_cast<std::size_t> (c)]
                                                     .process (data[i])) * preGain;
                const auto shaped  = clipper.process (excited + bias) - biasTerm;
                data[i] = limit1 (dc.process (tone.process (hump.process (shaped))) * outLevel);
            }
        }

        oversampler.down (wet, audio.getNumSamples());
        mixWithDelayedDry (audio, wet, getParameter (mix));
    }

private:
    /** The bass shelf lives before the clipper, so it changes how much low end
        drives the gain stage rather than merely equalising the output. Cached
        on the knob and the oversampled rate. */
    void updateBass() noexcept
    {
        const auto db = getParameter (bass);

        if (db == cachedBassDb && oversampleRate == cachedBassRate)
            return;

        for (auto& shelf : bassShelf)
            shelf.makeLowShelf (oversampleRate, 150.0, db, 0.8);

        cachedBassDb = db;
        cachedBassRate = oversampleRate;
    }

    Oversampler oversampler;
    double oversampleRate = 44100.0 * 4.0;
    std::array<Biquad, chainChannels> inputFilter, toneFilter, midHump, dcBlocker, bassShelf;
    std::array<adaa::Tanh, chainChannels> shaper;
    float cachedBassDb = 1.0e9f;
    double cachedBassRate = -1.0;
};

} // namespace pedal
