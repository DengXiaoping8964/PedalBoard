#pragma once

#include "Effects/PedalEffect.h"
#include "Effects/Pitch/PolyphonicOctaver.h"

#include <vector>

/*==============================================================================
  Octaver

  One polyphonic engine for both directions. The previous version used a
  zero-crossing divider (monophonic) and a full-wave rectifier (intermodulation
  on chords); on a real guitar's harmonics the divider toggled several times per
  period and the result was heard as buzz. This now uses the phase-scaling
  filterbank in PolyphonicOctaver: analytic bands, no note tracking, no grain
  windows, and both octaves follow chords.

  Down and Up are independent levels into the same wet bus. Blend is the usual
  dry/wet control. The engine has no look-ahead, so it reports zero latency and
  can sit anywhere in the chain.
==============================================================================*/
namespace pedal
{

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
        engine.prepare (sampleRate);

        mono.assign (static_cast<std::size_t> (blockSize), 0.0f);
        sub.assign (static_cast<std::size_t> (blockSize), 0.0f);
        upper.assign (static_cast<std::size_t> (blockSize), 0.0f);
    }

    void resetState() noexcept override
    {
        engine.reset();
    }

    juce::String getWarningText() const override
    {
        return tr ("Polyphonic octaver - both octaves follow chords, with no note tracking.");
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

        engine.setLevels (upLevel, downLevel);
        engine.process (mono.data(), upper.data(), sub.data(), numSamples);

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
    PolyphonicOctaver engine;
    std::vector<float> mono, sub, upper;
};

} // namespace pedal
