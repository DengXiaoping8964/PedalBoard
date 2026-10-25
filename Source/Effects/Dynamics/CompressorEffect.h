#pragma once

#include "Effects/PedalEffect.h"

#include <algorithm>
#include <cmath>

/*==============================================================================
  Compressor

  Algorithm: hand written feed-forward compressor. juce::dsp::Compressor exists
  in JUCE 8 but only offers threshold / ratio / attack / release, with a fixed
  hard knee and no makeup gain, so the standard text book maths is done here.

  Detection is the RMS of the two channels, smoothed by a 2 ms window and then
  converted to dB and followed by an envelope that works in the dB domain, which
  is what makes attack and release behave the same at every input level. The
  2 ms window is the fastest the detector can react, so it, not the attack knob,
  sets the shortest possible attack; that is the price of an RMS detector and it
  is what keeps the gain from tracking individual cycles of a low note. Gain
  reduction uses the RBJ style soft knee: inside the knee window the curve is
  quadratic, outside it the plain straight line ratio applies.

  Sidechain is a high pass in front of the detector only. A bass-heavy guitar
  otherwise makes the whole mix duck on every low note; the default 120 Hz lets
  the pick attack and the mids drive the compressor while the low strings pass
  through at a steadier level. It is the same two-knob idea as a hardware
  compressor's sidechain filter, and it does not change the audio path.

  One gain is derived per sample and applied to both channels so the stereo
  image cannot breathe. The block's largest reduction is published through an
  atomic for the card's gain reduction readout.

  The total gain is capped at +12 dB. Makeup is a boost, but +24 dB into a chain
  that is already at full scale is just a broken output, not a louder pedal.
==============================================================================*/
namespace pedal
{

class CompressorEffect : public PedalEffect
{
public:
    enum Params { threshold, ratio, attack, release, knee, makeup, mix, sidechain, numParams };

    CompressorEffect()
        : PedalEffect (compressor, "Compressor",
            {
                { "Threshold",  -60.0f,    0.0f,  -24.0f, "dB" },
                { "Ratio",        1.0f,   20.0f,    4.0f },
                /*  Attack starts at 2 ms, not 0.1: the detector's own 2 ms RMS
                    window is the floor, so a smaller number would be a knob that
                    does nothing over the bottom two thirds of its travel. */
                { "Attack",       2.0f,  100.0f,   12.0f, "ms" },
                { "Release",     10.0f, 1000.0f,  220.0f, "ms" },
                { "Knee",         0.0f,   24.0f,    6.0f, "dB" },
                /*  The gain is capped at +12 dB (see the class comment), so a
                    makeup range that went to 24 advertised a dead upper half. */
                { "Makeup",       0.0f,   12.0f,    6.0f, "dB" },
                { "Mix",          0.0f,    1.0f,    1.0f },
                { "Sidechain",   20.0f,  500.0f,  120.0f, "Hz" },
            })
    {
    }

    void doPrepare (double, int) override
    {
        updateCoefficients (true);
        updateSidechain (true);
        rmsCoefficient = timeCoefficient (sampleRate, 2.0f);
    }

    void resetState() noexcept override
    {
        envDb = floorDb;     // start from silence so the first block never pumps
        rmsPower = 0.0f;
        gainReductionDb.store (0.0f, std::memory_order_relaxed);

        for (auto& filter : sidechainFilter)
            filter.reset();
    }

    juce::String getStatusText() const override
    {
        // A bypassed effect is skipped by the engine, so the last block's value
        // would otherwise stay on the card as if the compressor were still working.
        if (isBypassed())
            return {};

        return trf ("Gain reduction %1 dB",
                    juce::String (gainReductionDb.load (std::memory_order_relaxed), 1));
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        updateCoefficients (false);
        updateSidechain (false);

        const auto thresholdDb = getParameter (threshold);
        const auto kneeDb      = getParameter (knee);
        const auto slope       = 1.0f / std::max (1.0f, getParameter (ratio)) - 1.0f;
        const auto makeupDb    = getParameter (makeup);

        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);
        auto& wet = wetBufferForBlock();

        float peakReductionDb = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float meanSquare = 0.0f;
            for (int c = 0; c < numChannels; ++c)
            {
                const auto sample = sidechainFilter[c].process (audio.getReadPointer (c)[i]);
                meanSquare += sample * sample;
            }

            if (numChannels > 0)
                meanSquare /= static_cast<float> (numChannels);

            rmsPower += (meanSquare - rmsPower) * rmsCoefficient;
            const auto level = std::sqrt (juce::jmax (0.0f, rmsPower));

            // dB domain follower: attack while the signal is louder than the env.
            const auto levelDb = gainToDb (level);
            envDb += (levelDb - envDb) * (levelDb > envDb ? attackCoeff : releaseCoeff);

            const auto over = envDb - thresholdDb;
            auto reductionDb = 0.0f;

            if (kneeDb > 0.0f && over > -0.5f * kneeDb && over < 0.5f * kneeDb)
            {
                // quadratic knee section, continuous with the line at both edges
                const auto t = over + 0.5f * kneeDb;
                reductionDb = slope * (t * t) / (2.0f * kneeDb);
            }
            else if (over > 0.0f)
            {
                reductionDb = slope * over;
            }

            peakReductionDb = std::min (peakReductionDb, reductionDb);

            // one gain for both channels, so the image cannot breathe
            const auto gain = clampf (dbToGain (reductionDb + makeupDb), 0.0f, 4.0f);

            for (int c = 0; c < numChannels; ++c)
                wet.getWritePointer (c)[i] = audio.getReadPointer (c)[i] * gain;
        }

        // Reported as a positive number: "gain reduction 8.0 dB" is the reading
        // every compressor meter shows, while the internal reduction is negative.
        gainReductionDb.store (-peakReductionDb, std::memory_order_relaxed);
        crossFade (audio, wet, getParameter (mix));
    }

private:
    /** Attack / release become filter coefficients, so they only depend on the
        knobs and the sample rate; recomputed at most once per block. */
    void updateCoefficients (bool force) noexcept
    {
        const auto newAttack  = getParameter (attack);
        const auto newRelease = getParameter (release);

        if (! force && newAttack == cachedAttack && newRelease == cachedRelease
            && sampleRate == coefficientRate)
            return;

        attackCoeff  = timeCoefficient (sampleRate, newAttack);
        releaseCoeff = timeCoefficient (sampleRate, newRelease);
        cachedAttack = newAttack;
        cachedRelease = newRelease;
        coefficientRate = static_cast<float> (sampleRate);
    }

    /** Sidechain high pass in front of the detector. Cached so the filter is
        only re-designed when the knob or device rate moves; state is kept, so
        turning the knob does not click. */
    void updateSidechain (bool force) noexcept
    {
        const auto hz   = getParameter (sidechain);
        const auto rate = static_cast<float> (sampleRate);

        if (! force && hz == cachedSidechainHz && rate == sidechainRate)
            return;

        for (auto& filter : sidechainFilter)
            filter.setHighPass (sampleRate, hz);

        cachedSidechainHz = hz;
        sidechainRate = rate;
    }

    static constexpr float floorDb = -120.0f;

    float envDb = floorDb;
    float rmsPower = 0.0f, rmsCoefficient = 0.01f;
    std::atomic<float> gainReductionDb { 0.0f };
    float attackCoeff = 0.5f, releaseCoeff = 0.05f;
    float cachedAttack = -1.0f, cachedRelease = -1.0f;
    float coefficientRate = 0.0f;
    float cachedSidechainHz = -1.0f, sidechainRate = 0.0f;
    OnePole sidechainFilter[chainChannels];
};

} // namespace pedal
