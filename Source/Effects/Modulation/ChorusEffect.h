#pragma once

#include "Effects/PedalEffect.h"

#include <array>

/*==============================================================================
  Chorus

  Algorithm: one modulated delay line per channel, a sine LFO and a stereo
  phase offset (Spread), the topology of the Boss CE-1/CE-2 family without the
  bucket-brigade's compander.

  The earlier version also high-passed the line input at 120 Hz and low-passed
  its output at 8 kHz to imitate a BBD's bandwidth. On an acoustic guitar those
  two filters removed the body from the wet signal and left a thin, gritty
  double that measured and sounded like a cheap converter; they are gone. The
  taps are read with a Catmull-Rom kernel instead of linear interpolation,
  which removes the HF rasp a moving linear tap adds at every LFO turnaround.

  The delay never sweeps through zero (it is clamped at two samples), and there
  is no feedback, so the comb notches stay shallow and wide: this is a
  thickening detune, not the flanger's moving resonance. At Mix 100 % the output
  is the delayed signal alone; at 50 % it is the usual chorus.

  All buffers are sized in doPrepare(); process() only reads and writes.
==============================================================================*/
namespace pedal
{

class ChorusEffect : public PedalEffect
{
public:
    enum Params { rate, depth, delayMs, mix, spread, numParams };

    ChorusEffect()
        : PedalEffect (chorus, "Chorus",
            {
                { "Rate",    0.05f,   5.0f,  0.45f, "Hz" },
                { "Depth",   0.0f,  100.0f, 38.0f, "%" },
                { "Delay",   2.0f,   30.0f, 12.0f, "ms" },
                { "Mix",     0.0f,    1.0f,  0.5f },
                { "Spread",  0.0f,  100.0f, 80.0f, "%" },
            })
    {
    }

    void doPrepare (double newSampleRate, int) override
    {
        // 30 ms centre + 6 ms excursion + headroom, for both channels.
        const auto capacity = static_cast<int> (0.040 * newSampleRate) + 8;
        delay.prepare (chainChannels, capacity);

        modulator.prepare (newSampleRate);
    }

    void resetState() noexcept override
    {
        delay.reset();
        modulator.reset();
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);

        if (numSamples <= 0)
            return;

        const auto rateHz    = clampf (getParameter (rate), 0.01f, 60.0f);
        const auto depth     = clamp01 (getParameter (Params::depth) * 0.01f);
        const auto centreMs  = clampf (getParameter (delayMs), 2.0f, 30.0f);
        const auto spread    = clamp01 (getParameter (Params::spread) * 0.01f) * 0.5f;

        const auto centreSamples = centreMs * 0.001f * static_cast<float> (sampleRate);
        const auto excursionMs   = depth * std::min (6.0f, centreMs * 0.55f);
        const auto excursion     = excursionMs * 0.001f * static_cast<float> (sampleRate);
        const auto maximumDelay  = static_cast<float> (juce::jmax (2, delay.getCapacity() - 2));

        auto& wet = wetBufferForBlock();
        auto* outL = wet.getWritePointer (0);
        auto* outR = wet.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto phase = modulator.step (rateHz, LfoShape::sine);
            const auto lfoL  = modulator.valueAt (phase, LfoShape::sine);
            const auto lfoR  = modulator.valueAt (phase + spread, LfoShape::sine);

            for (int c = 0; c < numChannels; ++c)
                delay.write (c, audio.getReadPointer (c)[i]);

            const auto tapL = clampf (centreSamples + lfoL * excursion, 2.0f, maximumDelay);
            const auto tapR = clampf (centreSamples + lfoR * excursion, 2.0f, maximumDelay);

            outL[i] = delay.readCubic (0, tapL);
            outR[i] = delay.readCubic (1, tapR);

            delay.advance();
        }

        crossFade (audio, wet, getParameter (mix));
    }

private:
    TappedDelay delay;
    Modulator modulator;
};

} // namespace pedal
