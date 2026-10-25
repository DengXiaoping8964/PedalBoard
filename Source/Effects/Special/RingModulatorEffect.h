#pragma once

#include "Effects/PedalEffect.h"

/*==============================================================================
  Ring Modulator

  Algorithm: true ring modulation - the signal is multiplied sample by sample by
  an internal sine carrier from pedal::Modulator. A multiplier produces the sum
  and the difference frequency and none of the original signal, which is what
  gives the effect its metallic, non harmonic sound; the wet buffer therefore
  holds only the modulated result and Mix blends it against the dry chain signal.

  Both channels are multiplied by the same oscillator value, so the carrier
  modulates amplitude rather than steering the image - a ring mod, not a panner.

  The oscillator phase is advanced once per sample (Modulator::step adds
  Frequency / sampleRate). Stepping it once per block would quantise the carrier
  to multiples of the block rate - 86 Hz per step with a 512 sample block at
  44.1 kHz - which is audible as a wobbling, obviously wrong pitch.

  Level tops out at 200 % and a ring modulator fed with a full scale signal
  already reaches +-1, so the wet signal and therefore the output can run past
  +-1 at high Mix. That is expected for this effect: the engine has no limiter
  by design, so the user owns the output level.
==============================================================================*/
namespace pedal
{

class RingModulatorEffect : public PedalEffect
{
public:
    enum Params { frequency, level, mix, numParams };

    RingModulatorEffect()
        : PedalEffect (ringModulator, "Ring Modulator",
            {
                { "Frequency", 20.0f, 5000.0f, 440.0f, "Hz" },
                { "Level",      0.0f,  200.0f, 100.0f, "%" },
                { "Mix",        0.0f,    1.0f,   0.6f },
            })
    {
    }

    void doPrepare (double newSampleRate, int) override
    {
        // The carrier steps by Frequency / sampleRate, so it needs the rate of the
        // blocks it is about to see.
        modulator.prepare (newSampleRate);
        preparedRate = static_cast<float> (newSampleRate);
        modulator.reset();
    }

    void resetState() noexcept override
    {
        modulator.reset();
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        if (static_cast<float> (sampleRate) != preparedRate)
        {
            modulator.prepare (sampleRate);
            preparedRate = static_cast<float> (sampleRate);
        }

        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);

        const auto carrierHz = clampf (getParameter (frequency), minFrequencyHz, maxFrequencyHz);
        const auto modGain   = clampf (getParameter (level), 0.0f, 200.0f) * 0.01f;

        const auto* srcL = audio.getReadPointer (0);
        const auto* srcR = numChannels > 1 ? audio.getReadPointer (1) : srcL;

        auto& wet = wetBufferForBlock();
        auto* outL = wet.getWritePointer (0);
        auto* outR = wet.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            // One phase step per sample, one shared carrier value for both
            // channels: the definition of ring modulation.
            const auto mod = modulator.step (carrierHz, LfoShape::sine) * modGain;

            outL[i] = srcL[i] * mod;
            outR[i] = srcR[i] * mod;
        }

        crossFade (audio, wet, getParameter (mix));
    }

private:
    static constexpr float minFrequencyHz = 20.0f;
    static constexpr float maxFrequencyHz = 5000.0f;

    Modulator modulator;
    float preparedRate = 0.0f;
};

} // namespace pedal
