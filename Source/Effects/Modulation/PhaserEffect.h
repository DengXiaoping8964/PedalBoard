#pragma once

#include <array>

#include "Effects/PedalEffect.h"

#include <algorithm>
#include <cmath>

/*==============================================================================
  Phaser

  Algorithm: hand written cascade of pedal::Biquad second order all-pass sections
  (Biquad::makeAllPass, RBJ cookbook) driven by one pedal::Modulator sine.

  A phaser is the frequency-domain sibling of the flanger: instead of a modulated
  delay line it stacks all-pass filters, whose phase response adds up to a set of
  cancellation notches when the filtered copy is blended with the dry signal. No
  delay line means no comb teeth across the whole spectrum - only the notches move,
  which is why a phaser sounds hollow/phasey where a flanger sounds metallic. The
  Feedback knob feeds the cascade output back to its input and sharpens the notches
  into resonant peaks; at 0 they stay wide and subtle.

  Section count is a discrete knob (4/6/8/12). More sections means deeper, narrower
  sweeps; the sections are pre-declared in a fixed array sized for the maximum, so
  changing stages never allocates. Coefficients are rebuilt at most every 16
  samples - an LFO sweeping a few Hz does not need a per-sample trig set - and
  every section is staggered slightly so the notches fan out instead of stacking.

  Centre sets where the sweep starts (the classic Phase 90 sits around 300 Hz;
  higher centres make the effect brighter and more vowel-like). Spread offsets
  the right channel's LFO phase from 0 to 180 degrees, so the notches move
  across the stereo image instead of in lockstep.
==============================================================================*/
namespace pedal
{

class PhaserEffect : public PedalEffect
{
public:
    enum Params { rate, depth, feedback, stages, mix, centre, spread, numParams };

    /** Maximum number of all-pass sections per channel; the Stages knob selects
        how many of them are actually in the chain. */
    static constexpr int maxStages = 12;

    static const char* const* stageLabels()
    {
        static const char* const names[] = { "4", "6", "8", "12" };
        return names;
    }

    PhaserEffect()
        : PedalEffect (phaser, "Phaser",
            {
                { "Rate",     0.05f,  6.0f,  0.4f, "Hz" },
                { "Depth",    0.0f,  100.0f, 70.0f, "%" },
                { "Feedback", 0.0f,  0.92f,  0.4f },
                { "Stages",   0.0f,   3.0f,  1.0f, "", 4, stageLabels() },
                { "Mix",      0.0f,   1.0f,  0.7f },
                { "Centre", 100.0f, 2000.0f, 300.0f, "Hz" },
                { "Spread",   0.0f, 100.0f,  50.0f, "%" },
            })
    {
    }

    void doPrepare (double newSampleRate, int newBlockSize) override
    {
        static_cast<void> (newBlockSize);

        modulator.prepare (newSampleRate);
        rebuildCounter = 0;               // build coefficients on the first sample
        coefficientRate = static_cast<float> (newSampleRate);
    }

    void resetState() noexcept override
    {
        modulator.reset();

        for (auto& channel : sections)
            for (auto& section : channel)
                section.reset();

        lastOutL = 0.0f;
        lastOutR = 0.0f;
        activeStages = 0;
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);

        if (sampleRate != coefficientRate)
        {
            // The device moved to another rate; the LFO step and every corner
            // frequency depend on it.
            modulator.prepare (sampleRate);
            coefficientRate = static_cast<float> (sampleRate);
            rebuildCounter = 0;
        }

        const auto rateHz   = clampf (getParameter (rate), 0.01f, 60.0f);
        const auto fb       = clampf (getParameter (feedback), 0.0f, 0.92f);
        const auto stageCount = stageCountFor (clampf (getParameter (stages), 0.0f, 3.0f));
        const auto baseHz   = clampf (getParameter (centre), 50.0f, 2000.0f);
        const auto spreadPhase = clamp01 (getParameter (Params::spread) * 0.01f) * 0.5f;

        // Depth 100 % = 4 octaves above the base (2 ^ (100 / 25)).
        const auto depthOctaves = clamp01 (getParameter (depth) * 0.01f) * 4.0f;

        if (stageCount != activeStages)
        {
            // Sections that just became active may still hold state from an older
            // configuration; clearing them avoids a click when Stages goes up.
            for (auto& channel : sections)
                for (auto& section : channel)
                    section.reset();

            activeStages = stageCount;
            rebuildCounter = 0;
        }

        const auto* inL = audio.getReadPointer (0);
        const auto* inR = numChannels > 1 ? audio.getReadPointer (1) : inL;

        auto& wet = wetBufferForBlock();
        auto* outL = wet.getWritePointer (0);
        auto* outR = wet.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto phase = modulator.step (rateHz, LfoShape::sine);
            const auto lfoL  = modulator.valueAt (phase, LfoShape::sine);
            const auto lfoR  = modulator.valueAt (phase + spreadPhase, LfoShape::sine);
            const auto lfoPosL = 0.5f * (lfoL + 1.0f);      // -1..1 -> 0..1
            const auto lfoPosR = 0.5f * (lfoR + 1.0f);

            if (--rebuildCounter <= 0)
            {
                rebuildCounter = rebuildInterval;
                rebuildSections (baseHz * std::pow (2.0, static_cast<double> (lfoPosL) * depthOctaves),
                                 baseHz * std::pow (2.0, static_cast<double> (lfoPosR) * depthOctaves));
            }

            const auto wetL = runCascade (0, inL[i] + fb * lastOutL, stageCount);
            const auto wetR = runCascade (1, inR[i] + fb * lastOutR, stageCount);

            lastOutL = wetL;
            lastOutR = wetR;

            outL[i] = wetL;
            outR[i] = wetR;
        }

        crossFade (audio, wet, getParameter (mix));
    }

private:
    static constexpr double allPassQ = 0.7071;
    static constexpr int rebuildInterval = 16;

    static int stageCountFor (float knobValue) noexcept
    {
        switch (static_cast<int> (knobValue + 0.5f))
        {
            case 0:  return 4;
            case 1:  return 6;
            case 2:  return 8;
            default: return maxStages;
        }
    }

    void rebuildSections (double leftHz, double rightHz) noexcept
    {
        const auto ceiling = sampleRate * 0.45;
        const double corners[chainChannels] = { leftHz, rightHz };

        for (int channel = 0; channel < chainChannels; ++channel)
            for (int k = 0; k < activeStages; ++k)
            {
                // Staggering each section ~12 % higher spreads the notches over the
                // octave, which is what makes a real phaser sweep instead of one
                // very deep notch moving on its own.
                const auto f = corners[channel] * (1.0 + 0.12 * k);
                sections[static_cast<std::size_t> (channel)]
                        [static_cast<std::size_t> (k)]
                            .makeAllPass (sampleRate, std::min (f, ceiling), allPassQ);
            }
    }

    float runCascade (int channel, float in, int stageCount) noexcept
    {
        auto& row = sections[static_cast<std::size_t> (channel)];

        for (int k = 0; k < stageCount; ++k)
            in = row[static_cast<std::size_t> (k)].process (in);

        return in;
    }

    std::array<Biquad, maxStages> sections[chainChannels];   // fixed: 24 biquads, no heap
    Modulator modulator;
    float lastOutL = 0.0f, lastOutR = 0.0f;
    float coefficientRate = 0.0f;
    int activeStages = 0, rebuildCounter = rebuildInterval;
};

} // namespace pedal
