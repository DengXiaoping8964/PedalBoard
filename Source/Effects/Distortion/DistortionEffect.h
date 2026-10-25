#pragma once

#include "Common/PedalOversampler.h"
#include "Effects/PedalEffect.h"

#include <array>
#include <cmath>

/*==============================================================================
  Distortion

  Transfer curve: a piecewise linear break curve (shaping::hardClip). Below the
  knee the signal is untouched, above it the slope collapses almost vertically.
  That discontinuity in the first derivative is exactly what makes this audibly
  different from the overdrive: more odd harmonics, compressed sustain, a drier
  top end. The Tighten knob high passes the signal before it hits the curve so
  low strings do not turn into mud.

  The break point is fixed and drive only pushes the signal into it. Moving the
  threshold down with the knob as well used to make the pedal loudest in the
  middle of its range: the clamp's ceiling rose with the input while the
  threshold fell, and the two crossed.

  The clipper is first order antiderivative anti-aliased (pedal::adaa::HardClip,
  following Chowdhury): the curve is integrated over each oversampled interval
  instead of being sampled once, which suppresses the residual aliasing left by
  4x oversampling at full-scale drive. The break point and residual slope are
  constant, so the ADAA history stays valid while the Drive knob moves.

  Presence is a high shelf at 3 kHz after the tone low pass. Tone can only take
  brightness away; Presence adds the bite and pick attack back after the clipper,
  which is what lets the pedal cut through without turning Tone up into fizz.

  4x oversampling, same reasoning as the overdrive.
==============================================================================*/
namespace pedal
{

class DistortionEffect : public PedalEffect
{
public:
    enum Params { drive, tighten, tone, level, mix, presence, numParams };

    DistortionEffect()
        : PedalEffect (distortion, "Distortion",
            {
                { "Drive",    0.0f, 100.0f, 50.0f },
                { "Tighten",  0.0f, 400.0f, 130.0f, "Hz" },
                { "Tone",     0.0f, 100.0f, 45.0f },
                { "Level",    0.0f, 150.0f, 55.0f, "%" },
                { "Mix",      0.0f,   1.0f,  1.0f },
                { "Presence", -12.0f, 12.0f, 0.0f, "dB" },
            })
    {
    }

    void doPrepare (double, int) override
    {
        oversampler.prepare (chainChannels, blockSize, 2);   // 4x
        oversampleRate = oversampler.oversampledRate (sampleRate);

        for (auto& shelf : presenceShelf)
        {
            shelf.makeHighShelf (oversampleRate, 3000.0, getParameter (presence), 0.8);
            shelf.reset();
        }

        cachedPresenceDb = getParameter (presence);
        cachedPresenceRate = oversampleRate;

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
        for (auto& tone    : toneFilter)  tone.reset();
        for (auto& shelf   : presenceShelf) shelf.reset();
        for (auto& blocker : dcBlocker)   blocker.reset();
        for (auto& clipper : shaper)      clipper.reset();
    }

    int getLatencySamples() const noexcept override { return oversampler.getLatencySamples(); }

    void process (juce::AudioBuffer<float>& audio) override
    {
        auto& wet = wetBufferForBlock();

        for (int c = 0; c < chainChannels; ++c)
            wet.copyFrom (c, 0, audio, c, 0, audio.getNumSamples());

        updatePresence();

        /*  +30 dB at the top. The old 0.18 slope gave +21.7 dB at the default and
            measured 22 % THD, less than the overdrive it is supposed to out-grit;
            the new default measures about 30 %, and the curve now reaches the
            hard-clipped ceiling on an acoustic input. */
        const auto preGain = 1.0f + getParameter (drive) * 0.30f;
        const auto outLevel = getParameter (level) * 0.005f;   // 150 % is a +6 dB ceiling, not +11 dB
        const auto toneHz = 500.0f * std::pow (24.0f, getParameter (tone) * 0.01f);
        const auto tightenHz = getParameter (tighten);

        /*  The break point is fixed and drive only pushes the signal into it.
            Moving the threshold down with the knob as well used to make the
            pedal loudest in the middle of its range: the clamp's ceiling rises
            with the input while the threshold falls, and the two crossed. */
        constexpr float clipThreshold = 0.35f;

        const auto block = oversampler.up (wet, audio.getNumSamples());
        const auto numSamples = static_cast<int> (block.getNumSamples());

        for (int c = 0; c < static_cast<int> (block.getNumChannels()); ++c)
        {
            auto* data = block.getChannelPointer (static_cast<size_t> (c));
            auto& hp   = inputFilter[static_cast<std::size_t> (c)];
            auto& tone = toneFilter[static_cast<std::size_t> (c)];
            auto& dc   = dcBlocker[static_cast<std::size_t> (c)];
            auto& clipper = shaper[static_cast<std::size_t> (c)];

            if (tightenHz > 1.0f)
                hp.makeHighPass (oversampleRate, tightenHz, 0.7071);
            else
                hp.setPassthrough();

            tone.makeLowPass (oversampleRate, toneHz, 0.7071);

            clipper.threshold = clipThreshold;
            clipper.residual  = 0.06f;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto cleaned = hp.process (data[i]);
                const auto shaped = clipper.process (cleaned * preGain);
                const auto filtered = presenceShelf[static_cast<std::size_t> (c)]
                                          .process (tone.process (shaped));

                data[i] = limit1 (dc.process (filtered) * outLevel);
            }
        }

        oversampler.down (wet, audio.getNumSamples());
        mixWithDelayedDry (audio, wet, getParameter (mix));
    }

private:
    /** Post-clip high shelf, cached on the knob and oversampled rate. */
    void updatePresence() noexcept
    {
        const auto db = getParameter (presence);

        if (db == cachedPresenceDb && oversampleRate == cachedPresenceRate)
            return;

        for (auto& shelf : presenceShelf)
            shelf.makeHighShelf (oversampleRate, 3000.0, db, 0.8);

        cachedPresenceDb = db;
        cachedPresenceRate = oversampleRate;
    }

    Oversampler oversampler;
    double oversampleRate = 44100.0 * 4.0;
    std::array<Biquad, chainChannels> inputFilter, toneFilter, dcBlocker, presenceShelf;
    std::array<adaa::HardClip, chainChannels> shaper;
    float cachedPresenceDb = 1.0e9f;
    double cachedPresenceRate = -1.0;
};

} // namespace pedal
