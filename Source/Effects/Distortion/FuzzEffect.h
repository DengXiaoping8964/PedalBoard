#pragma once

#include "Common/PedalOversampler.h"
#include "Effects/PedalEffect.h"

#include <array>
#include <cmath>

/*==============================================================================
  Fuzz

  Transfer curve: asymmetric saturation (shaping::asymmetricFuzz). The positive
  and negative halves of the waveform break at different levels, which generates
  even harmonics on top of the odd ones - the reason a fuzz sounds spitty and
  gated where the symmetrical distortion pedal sounds smooth.

  Bias 0 %  -> symmetrical curve
  Bias 100 % -> one polarity saturates far earlier than the other

  A germanium style fuzz also loses its low end because the transistors are
  biased into conduction by the signal itself, so the input is high passed before
  the gain stage and the gain is enormous (up to about 40 dB). The 3.5 kHz
  low pass that follows it is the input capacitance of the transistor pair: a
  Fuzz Face's 470 pF input cap rolls off the top before the clipping stage, and
  without it the curve would generate more aliasing than the 4x oversampler can
  remove.

  Each polarity of the curve is divided by its own knee so both halves reach the
  same ceiling: the Bias knob then changes the harmonic content without also
  changing the level by 10 dB.

  Bass is a low shelf at 150 Hz between the input high pass and the gain stage.
  A fuzz that is too thin can be filled back in, and one that flubs out on the
  low strings can be tightened, without changing the 3.5 kHz transistor roll-off
  that gives the pedal its spit.

  The clipper is first order antiderivative anti-aliased
  (pedal::adaa::AsymmetricFuzz, following Chowdhury). Its antiderivative depends
  on the two knees, so changing Bias restarts the ADAA history - one sample of
  difference on a user gesture, not a running artifact.

  4x oversampling.
==============================================================================*/
namespace pedal
{

class FuzzEffect : public PedalEffect
{
public:
    enum Params { fuzz, bias, tone, level, mix, bass, numParams };

    FuzzEffect()
        : PedalEffect (fuzz, "Fuzz",
            {
                { "Fuzz",   0.0f, 100.0f, 72.0f },
                { "Bias",   0.0f, 100.0f, 45.0f },
                { "Tone",   0.0f, 100.0f, 58.0f },
                { "Level",  0.0f, 150.0f, 55.0f, "%" },
                { "Mix",    0.0f,   1.0f,  1.0f },
                { "Bass", -12.0f,  12.0f,  0.0f, "dB" },
            })
    {
    }

    void doPrepare (double, int) override
    {
        oversampler.prepare (chainChannels, blockSize, 2);   // 4x
        oversampleRate = oversampler.oversampledRate (sampleRate);

        for (auto& filter : inputLowPass)
        {
            filter.setLowPass (oversampleRate, 3500.0);
            filter.reset();
        }

        for (auto& shelf : bassShelf)
        {
            shelf.makeLowShelf (oversampleRate, 150.0, getParameter (bass), 0.8);
            shelf.reset();
        }

        cachedBassDb = getParameter (bass);
        cachedBassRate = oversampleRate;

        for (auto& blocker : dcBlocker)
        {
            blocker.makeHighPass (oversampleRate, 12.0, 0.7071);
            blocker.reset();
        }

        prepareLatencyCompensatedDry (getLatencySamples());
    }

    void resetState() noexcept override
    {
        oversampler.reset();
        for (auto& filter : inputFilter)  filter.reset();
        for (auto& filter : inputLowPass) filter.reset();
        for (auto& tone    : toneFilter)  tone.reset();
        for (auto& shelf   : bassShelf)   shelf.reset();
        for (auto& blocker : dcBlocker)   blocker.reset();
        for (auto& clipper : shaper)      clipper.reset();
    }

    int getLatencySamples() const noexcept override { return oversampler.getLatencySamples(); }

    void process (juce::AudioBuffer<float>& audio) override
    {
        auto& wet = wetBufferForBlock();

        for (int c = 0; c < chainChannels; ++c)
            wet.copyFrom (c, 0, audio, c, 0, audio.getNumSamples());

        updateBass();

        const auto preGain = 2.0f + getParameter (fuzz) * 1.0f;
        const auto bias = clamp01 (getParameter (Params::bias) * 0.01f);

        // One knee stays put while the other collapses, so the curve tilts.
        const auto positiveKnee = 0.12f + (1.0f - bias) * 0.5f;
        const auto negativeKnee = 0.12f + bias * 0.5f;

        const auto outLevel = getParameter (level) * 0.005f;   // 150 % is a +6 dB ceiling
        const auto toneHz = 400.0f * std::pow (28.0f, getParameter (tone) * 0.01f);

        const auto block = oversampler.up (wet, audio.getNumSamples());
        const auto numSamples = static_cast<int> (block.getNumSamples());

        for (int c = 0; c < static_cast<int> (block.getNumChannels()); ++c)
        {
            auto* data = block.getChannelPointer (static_cast<size_t> (c));
            auto& hp   = inputFilter[static_cast<std::size_t> (c)];
            auto& tilt = inputLowPass[static_cast<std::size_t> (c)];
            auto& tone = toneFilter[static_cast<std::size_t> (c)];
            auto& dc   = dcBlocker[static_cast<std::size_t> (c)];
            auto& clipper = shaper[static_cast<std::size_t> (c)];

            hp.setHighPass (oversampleRate, 120.0);
            tone.setLowPass (oversampleRate, toneHz);
            clipper.setKnees (positiveKnee, negativeKnee);

            for (int i = 0; i < numSamples; ++i)
            {
                // Thin first, then amplify: a fuzz loses its low end because the
                // transistors are driven by the signal, so the high pass has to
                // sit in front of the gain, not behind it.
                const auto thin = tilt.process (bassShelf[static_cast<std::size_t> (c)]
                                                    .process (hp.process (data[i]))) * preGain;
                const auto curved = clipper.process (thin);
                const auto half = curved >= 0.0f ? positiveKnee : negativeKnee;

                data[i] = limit1 (dc.process (tone.process (curved / half)) * outLevel * 0.8f);
            }
        }

        oversampler.down (wet, audio.getNumSamples());
        mixWithDelayedDry (audio, wet, getParameter (mix));
    }

private:
    /** Low shelf before the gain stage; cached on knob and oversampled rate. */
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
    std::array<OnePole, chainChannels> inputFilter;
    std::array<OnePole, chainChannels> inputLowPass;
    std::array<OnePole, chainChannels> toneFilter;
    std::array<Biquad, chainChannels> dcBlocker;
    std::array<Biquad, chainChannels> bassShelf;
    std::array<adaa::AsymmetricFuzz, chainChannels> shaper;
    float cachedBassDb = 1.0e9f;
    double cachedBassRate = -1.0;
};

} // namespace pedal
