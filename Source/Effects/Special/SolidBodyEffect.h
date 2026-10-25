#pragma once

#include "Common/PedalOversampler.h"
#include "Effects/PedalEffect.h"

#include <algorithm>
#include <array>
#include <cmath>

/*==============================================================================
  Acoustic to Electric

  Turns a steel string acoustic, or a piezo / undersaddle DI, towards the colour
  of a solid body electric so that the pedals after it - Drive, Cabinet - behave
  the way they would on an electric.

  The numbers below are taken from published practice rather than invented, and
  they are the reason the defaults are what they are:

    * An acoustic body has a Helmholtz air resonance around 95-105 Hz and its
      first top plate modes near 180-200 Hz. A magnetic pickup through a closed
      back cabinet has almost nothing at those frequencies, so the first thing
      this pedal does is a 2 pole high pass at 110 Hz plus two bell cuts. This is
      the move every write-up on the subject agrees on: the body resonance is
      what makes an amplified acoustic sound like an amplified acoustic.
    * "Boxiness" sits between 200 and 400 Hz, the piezo "quack" between 2 and
      4 kHz: both are narrow cuts, and DI guides cut the quack by 3 to 8 dB.
    * Acoustic "air" and brittleness live at 6-8 kHz. A guitar speaker rolls off
      far below that, so the Air Cut low pass defaults to 6.5 kHz.
    * An electric tone is both compressed and harmonically rich, in a way an
      acoustic is not. The harmonics are made with square law addition, which is
      the gentlest nonlinearity there is and produces even order products, then
      passed through the same tanh the Overdrive uses for the odd order part. It
      is blended back in parallel so the fundamental and the pick attack survive:
      serial-only saturation on an acoustic signal just buries the notes.
    * Saturation runs 4x oversampled, because squaring a full scale signal
      generates harmonics that fold straight back into the audible band.

  What it cannot do, stated on the card rather than hidden: the two body cuts are
  at fixed frequencies, so a guitar whose first mode is well off 190 or 380 Hz
  needs the 3-Band EQ in front of this to finish the job, and no equaliser can
  give a string the transient behaviour of a magnet under it.
==============================================================================*/
namespace pedal
{

class SolidBodyEffect : public PedalEffect
{
public:
    enum Params { bodyCut, quackCut, airCutHz, presence, drive, blend, squash, mix,
                  numParams };

    SolidBodyEffect()
        : PedalEffect (solidBody, "Acoustic to Electric",
            {
                { "Body Cut",   0.0f,    24.0f,  12.0f,  "dB" },
                { "Quack Cut",  0.0f,    15.0f,   8.0f,  "dB" },
                { "Air Cut",    2000.0f, 12000.0f, 6500.0f, "Hz" },
                { "Presence",   0.0f,    12.0f,   6.0f,  "dB" },
                { "Drive",      0.0f,   100.0f,  45.0f },
                { "Blend",      0.0f,   100.0f,  60.0f, "%" },
                { "Squash",     0.0f,   100.0f,  35.0f, "%" },
                { "Mix",        0.0f,     1.0f,   1.0f },
            })
    {
    }

    juce::String getWarningText() const override
    {
        return tr ("Fixed 190 Hz and 380 Hz body cuts: put a 3-Band EQ in front if "
                   "your guitar's first mode sits elsewhere, and a Cabinet after it.");
    }

    void doPrepare (double, int) override
    {
        oversampler.prepare (chainChannels, blockSize, 2);   // 4x
        oversampleRate = oversampler.oversampledRate (sampleRate);

        for (auto& blocker : dcBlocker)
        {
            blocker.makeHighPass (oversampleRate, 9.0, 0.7071);
            blocker.reset();
        }

        retune();

        for (auto& f : filters)
        {
            f.highPass.reset(); f.bodyLow.reset(); f.bodyMid.reset();
            f.quack.reset();    f.airCut.reset();  f.presence.reset();
        }

        prepareLatencyCompensatedDry (getLatencySamples());
    }

    void resetState() noexcept override
    {
        oversampler.reset();
        detector.reset();
        squashGain = 1.0f;

        for (auto& f : filters)
        {
            f.highPass.reset(); f.bodyLow.reset(); f.bodyMid.reset();
            f.quack.reset();    f.airCut.reset();  f.presence.reset();
        }
    }

    int getLatencySamples() const noexcept override { return oversampler.getLatencySamples(); }

    void process (juce::AudioBuffer<float>& audio) override
    {
        auto& wet = wetBufferForBlock();

        for (int c = 0; c < chainChannels; ++c)
            wet.copyFrom (c, 0, audio, c, 0, audio.getNumSamples());

        retune();

        const auto numSamples = audio.getNumSamples();

        // 1. Take the acoustic body out of the signal.
        for (int c = 0; c < chainChannels; ++c)
        {
            auto* data = wet.getWritePointer (c);
            auto& f = filters[static_cast<std::size_t> (c)];

            for (int i = 0; i < numSamples; ++i)
            {
                auto v = data[i];
                v = f.highPass.process (v);
                v = f.bodyLow.process (v);
                v = f.bodyMid.process (v);
                v = f.quack.process (v);
                v = f.airCut.process (v);
                data[i] = v;
            }
        }

        // 2. Squash the dynamics down to what an amplified electric does. One
        //    detector for both channels, so closing cannot move the image.
        const auto squash01 = clamp01 (getParameter (squash));
        const auto thresholdDb = -18.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float level = 0.0f;
            for (int c = 0; c < chainChannels; ++c)
                level = std::max (level, std::abs (wet.getReadPointer (c)[i]));

            const auto envDb = gainToDb (detector.process (level));
            const auto over = juce::jmax (0.0f, envDb - thresholdDb);
            const auto target = dbToGain (clampf (-over * 0.8f * squash01, -36.0f, 0.0f));

            squashGain += (target - squashGain) * (target > squashGain ? attackCoeff : releaseCoeff);

            for (int c = 0; c < chainChannels; ++c)
                wet.getWritePointer (c)[i] *= squashGain;
        }

        // 3. Add the harmonic ladder the pickup never had, in parallel.
        const auto drive01   = clamp01 (getParameter (drive));
        const auto preGain   = 1.0f + drive01 * 5.0f;
        const auto hardness  = 1.0f + drive01 * 6.0f;
        const auto evenAmount = drive01 * 0.9f;
        const auto blend01   = clamp01 (getParameter (blend));

        // softSaturation is tanh (k x) / tanh (k), whose slope for small signals
        // is k / tanh (k), not 1. Combined with preGain that would make the wet
        // branch tens of times louder than the dry one, so it is divided back
        // out. The result is a branch that tracks the input exactly until it is
        // loud enough to compress: Drive then sets harmonic density and squash at
        // a constant output level, which is what makes the bypass comparison and
        // the pedals after it honest.
        const auto normalise = std::tanh (hardness) / (preGain * hardness);

        const auto block = oversampler.up (wet, audio.getNumSamples());
        const auto osSamples = static_cast<int> (block.getNumSamples());

        for (int c = 0; c < static_cast<int> (block.getNumChannels()); ++c)
        {
            auto* data = block.getChannelPointer (static_cast<size_t> (c));
            auto& blocker = dcBlocker[static_cast<std::size_t> (c)];

            for (int i = 0; i < osSamples; ++i)
            {
                const auto x = data[i] * preGain;

                // x squared is even order only: it lifts both halves of the
                // waveform, which is the harmonic an acoustic is missing.
                const auto even = x + evenAmount * x * x;
                auto saturated = shaping::softSaturation (even, hardness);
                saturated = limit1 (blocker.process (saturated)) * normalise;

                data[i] = data[i] * (1.0f - blend01) + saturated * blend01;
            }
        }

        oversampler.down (wet, audio.getNumSamples());

        // 4. The cabinet presence bump, after the oversampler so it shapes the
        //    harmonics rather than being smeared by them.
        for (int c = 0; c < chainChannels; ++c)
        {
            auto* data = wet.getWritePointer (c);
            auto& pres = filters[static_cast<std::size_t> (c)].presence;

            for (int i = 0; i < numSamples; ++i)
                data[i] = pres.process (data[i]);
        }

        // The oversampler delays the wet branch; the dry branch has to travel
        // through the same delay or Mix, which defaults to 100 %, would comb.
        mixWithDelayedDry (audio, wet, getParameter (mix));
    }

private:
    /** Biquads are cheap but not free, so they are only re-coefficient when a
        knob actually moved. */
    void retune() noexcept
    {
        const auto newBody     = getParameter (bodyCut);
        const auto newQuack    = getParameter (quackCut);
        const auto newAir      = getParameter (airCutHz);
        const auto newPresence = getParameter (presence);
        const auto newSquash   = getParameter (squash);

        if (newBody == cachedBody && newQuack == cachedQuack && newAir == cachedAir
            && newPresence == cachedPresence && newSquash == cachedSquash
            && sampleRate == cachedRate)
            return;

        cachedBody = newBody;
        cachedQuack = newQuack;
        cachedAir = newAir;
        cachedPresence = newPresence;
        cachedSquash = newSquash;
        cachedRate = sampleRate;

        const auto bodyDepth = -clampf (newBody, 0.0f, 24.0f);

        for (int c = 0; c < chainChannels; ++c)
        {
            auto& f = filters[static_cast<std::size_t> (c)];

            f.highPass .makeHighPass (sampleRate, 110.0, 0.7071);
            f.bodyLow  .makePeak (sampleRate, 190.0, 1.4, bodyDepth * 0.6f);
            f.bodyMid  .makePeak (sampleRate, 380.0, 1.8, bodyDepth * 0.4f);
            f.quack    .makePeak (sampleRate, 2800.0, 1.2, -clampf (newQuack, 0.0f, 15.0f));
            f.airCut   .makeLowPass (sampleRate, clampd (newAir, 2000.0, 12000.0), 0.7071);
            f.presence .makePeak (sampleRate, 3200.0, 0.9, clampf (newPresence, 0.0f, 12.0f));
        }

        detector.setTimes (sampleRate, 6.0f, 120.0f);
        attackCoeff  = timeCoefficient (sampleRate, 6.0f);
        releaseCoeff = timeCoefficient (sampleRate, 120.0f);
    }

    /** The low pass corner is a float knob that drives a double filter
        parameter, so it is clamped in double precision to keep makeLowPass()
        away from nonsense values at the top of the range. */
    static double clampd (float v, double lo, double hi) noexcept
    {
        const auto d = static_cast<double> (v);
        return d < lo ? lo : (d > hi ? hi : d);
    }

    struct ChannelFilters
    {
        Biquad highPass, bodyLow, bodyMid, quack, airCut, presence;
    };

    std::array<ChannelFilters, chainChannels> filters;
    std::array<Biquad, chainChannels> dcBlocker;
    Oversampler oversampler;
    double oversampleRate = 44100.0 * 4.0;

    EnvelopeFollower detector;
    float squashGain = 1.0f, attackCoeff = 0.5f, releaseCoeff = 0.05f;

    float cachedBody = -1.0f, cachedQuack = -1.0f, cachedAir = -1.0f;
    float cachedPresence = -1.0f, cachedSquash = -1.0f;
    double cachedRate = 0.0;
};

} // namespace pedal
