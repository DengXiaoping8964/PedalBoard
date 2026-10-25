#pragma once

#include "Effects/PedalEffect.h"

/*==============================================================================
  Vibrato

  Algorithm: hand written - pedal::Modulator sine sweeps the read tap of a
  pedal::TappedDelay and the tapped sample *is* the output.

  The pure pitch end of the modulation family: the same short delay line as the
  flanger, but with the feedback path removed completely and no dry summing
  inside the effect. Changing the delay length sample by sample stretches and
  squeezes the waveform, which shifts the pitch up and down - a wow/no wow
  wobble. A flanger's feedback turns its comb into resonant notches; without it
  (and with the delay kept well away from zero) there is no resonance and no
  sweeping metallic peak, only pitch movement. It differs from the chorus for the
  same reason plus a much shorter centre delay: the chorus keeps a long fixed
  delay and mixes dry in, so it detunes rather than bends.

  Mix defaults to 1.0 because vibrato is only heard fully wet - blending it back
  against the dry guitar just dilutes the bend into chorus, and the dry tap is
  still available for anyone who wants that.

  Spread offsets the right channel's LFO phase up to 180 degrees. On a mono
  guitar both channels carry the same signal, so this turns a static pitch
  wobble into a wide, swirling one; at 0 the two sides move together.
==============================================================================*/
namespace pedal
{

class VibratoEffect : public PedalEffect
{
public:
    enum Params { rate, depth, centre, mix, spread, numParams };

    /** Depth is relative to Centre (0 % = no excursion, 100 % = +/- 75 % of the
        centre delay) so the pitch swing stays musically proportional at every
        Centre setting instead of being swamped by a long centre delay. */
    static constexpr float maxDepthRatio = 0.75f;

    VibratoEffect()
        : PedalEffect (vibrato, "Vibrato",
            {
                { "Rate",   0.1f,   8.0f,  4.5f, "Hz" },
                { "Depth",  0.0f, 100.0f, 24.0f, "%" },
                { "Centre", 2.0f,  12.0f,  4.0f, "ms" },
                { "Mix",    0.0f,   1.0f,  1.0f },
                { "Spread", 0.0f, 100.0f,  0.0f, "%" },
            })
    {
    }

    void doPrepare (double newSampleRate, int newBlockSize) override
    {
        static_cast<void> (newBlockSize);

        // 25 ms covers the widest case (12 ms centre + 9 ms of excursion) with
        // headroom for the capacity - 2 clamp.
        delay.prepare (chainChannels, samplesFor (newSampleRate, 25.0f));
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

        const auto rateHz        = clampf (getParameter (rate), 0.01f, 30.0f);
        const auto centreSamples = getParameter (centre) * 0.001f * static_cast<float> (sampleRate);
        const auto depthSamples  = getParameter (depth) * 0.01f * maxDepthRatio * centreSamples;
        const auto spreadPhase   = clamp01 (getParameter (Params::spread) * 0.01f) * 0.5f;
        const auto maxDelay      = static_cast<float> (juce::jmax (2, delay.getCapacity() - 2));

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

            // Two samples is the floor for the cubic read; 0.5 would land on the
            // slot the write head just filled, i.e. a stretch of the sweep with no
            // pitch shift in it at all.
            const auto tapL = clampf (centreSamples + lfoL * depthSamples, 2.0f, maxDelay);
            const auto tapR = clampf (centreSamples + lfoR * depthSamples, 2.0f, maxDelay);

            delay.write (0, inL[i]);
            delay.write (1, inR[i]);        // no feedback term at all: pure vibrato
            delay.advance();

            outL[i] = delay.readCubic (0, tapL);
            outR[i] = delay.readCubic (1, tapR);
        }

        crossFade (audio, wet, getParameter (mix));
    }

private:
    static int samplesFor (double rate, float milliseconds) noexcept
    {
        return juce::jmax (8, static_cast<int> (milliseconds * 0.001 * rate));
    }

    TappedDelay delay;
    Modulator modulator;
};

} // namespace pedal
