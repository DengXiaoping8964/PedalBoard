#pragma once

#include "Effects/PedalEffect.h"
#include "Effects/Pitch/SubOctaveDivider.h"

#include <vector>
#include <algorithm>

/*==============================================================================
  Octaver

  Two analogue-style engines, because the two directions are not the same
  problem and neither is a pitch shifter:

    Octave Down  - SubOctaveDivider. A hard wired frequency divider: exact half
                   the input frequency, no look ahead, no granular smear.

    Octave Up    - full-wave rectifier, the Octavia/Tysegue trick. Rectifying an
                   AC signal doubles its fundamental and adds even harmonics;
                   a 6 kHz input filter and a 4.8 kHz output filter keep the
                   folded harmonics out of the alias band. This is what an
                   octave-up pedal sounds like, and unlike a granular shifter it
                   locks instantly and has no window warble.

  Both paths track a single note. The dry signal is the Blend knob's complement,
  so Blend left of centre is the direct guitar, right of centre is the octaves.
  The divider and the rectifier have no look-ahead, so the effect reports zero
  latency and can sit anywhere in the chain without delaying the dry path.
==============================================================================*/
namespace pedal
{

class OctaveUpRectifier
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;

        inputFilter.setLowPass (sampleRate, 6000.0);
        inputFilter.reset();
        outputFilter.setLowPass (sampleRate, 4800.0);
        outputFilter.reset();
        dcBlocker.setHighPass (sampleRate, 60.0);
        dcBlocker.reset();

        envelope.setTimes (sampleRate, 3.0f, 120.0f);
        envelope.reset();
        gate = 0.0f;
    }

    void reset() noexcept
    {
        inputFilter.reset();
        outputFilter.reset();
        dcBlocker.reset();
        envelope.reset();
        gate = 0.0f;
    }

    /** in/out are mono. level is the Up knob as 0..1. */
    void process (const float* in, float* out, int numSamples, float level) noexcept
    {
        const auto gateCoeff = timeCoefficient (sampleRate, 25.0f);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto filtered = inputFilter.process (in[i]);
            const auto rectified = std::abs (filtered);

            // The rectified waveform has a DC term proportional to level; a 60 Hz
            // high pass removes it without eating the guitar's low E octave.
            const auto shaped = outputFilter.process (dcBlocker.process (rectified));

            const auto magnitude = envelope.process (std::abs (filtered));
            const auto target = clamp01 (magnitude * 5.0f);
            gate += gateCoeff * (target - gate);

            out[i] = shaped * gate * level;
        }
    }

private:
    double sampleRate = 44100.0;
    OnePole inputFilter, outputFilter, dcBlocker;
    EnvelopeFollower envelope;
    float gate = 0.0f;
};

class OctaveEffect : public PedalEffect
{
public:
    enum Params { down, up, blend, numParams };

    OctaveEffect()
        : PedalEffect (octaver, "Octaver",
            {
                { "Octave Down", 0.0f, 100.0f, 60.0f, "%" },
                { "Octave Up",   0.0f, 100.0f, 30.0f, "%" },
                { "Blend",       0.0f,   1.0f,  0.8f },
            })
    {
    }

    void doPrepare (double, int) override
    {
        divider.prepare (sampleRate, blockSize);
        rectifier.prepare (sampleRate);

        mono.assign (static_cast<std::size_t> (blockSize), 0.0f);
        sub.assign (static_cast<std::size_t> (blockSize), 0.0f);
        upper.assign (static_cast<std::size_t> (blockSize), 0.0f);
    }

    void resetState() noexcept override
    {
        divider.reset();
        rectifier.reset();
    }

    juce::String getWarningText() const override
    {
        return tr ("Single notes track best - the divider and the rectifier are both monophonic.");
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        const auto numSamples = audio.getNumSamples();
        const auto downLevel = getParameter (down) * 0.01f;
        const auto upLevel   = getParameter (up) * 0.01f;

        const auto* left  = audio.getReadPointer (0);
        const auto* right = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : left;

        for (int i = 0; i < numSamples; ++i)
            mono[static_cast<std::size_t> (i)] = 0.5f * (left[i] + right[i]);

        divider.process (mono.data(), sub.data(), numSamples, downLevel);
        rectifier.process (mono.data(), upper.data(), numSamples, upLevel);

        auto& wet = wetBufferForBlock();

        for (int c = 0; c < chainChannels; ++c)
        {
            auto* out = wet.getWritePointer (c);

            for (int i = 0; i < numSamples; ++i)
                out[i] = sub[static_cast<std::size_t> (i)] + upper[static_cast<std::size_t> (i)];
        }

        crossFade (audio, wet, getParameter (blend));
    }

private:
    SubOctaveDivider divider;
    OctaveUpRectifier rectifier;
    std::vector<float> mono, sub, upper;
};

} // namespace pedal
