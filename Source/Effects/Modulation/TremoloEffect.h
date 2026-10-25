#pragma once

#include "Effects/PedalEffect.h"

/*==============================================================================
  Tremolo

  Algorithm: hand written amplitude modulation - gain = 1 - depth * (0.5 - 0.5 *
  lfo), with the LFO from pedal::Modulator. juce::dsp::Gain exists but only
  applies a static gain, so there is nothing in the module to borrow here.

  Tremolo is the amplitude cousin of the vibrato: the LFO moves loudness instead
  of pitch, so the signal path has no delay line at all and no comb filtering.

  THERE IS NO MIX KNOB ON PURPOSE. Tremolo is not a parallel effect path - it is
  the chain signal multiplied by a time varying gain, so a "dry" copy blended
  against it would simply be an un-modulated signal leaking through and weakening
  the depth. The class therefore writes straight into the chain buffer instead of
  using the wet scratch buffer.

  The square shape is slew limited (1.5 ms one pole) because an instantaneous
  gain jump on every half period is a click, not an effect.

  Spread offsets the right channel's LFO phase up to 180 degrees. At 0 the whole
  image pulses together; at 100 % the two sides breathe in opposition, which is
  the wide stereo tremolo setting.
==============================================================================*/
namespace pedal
{

class TremoloEffect : public PedalEffect
{
public:
    enum Params { rate, depth, shape, spread, numParams };

    /** Slew time for the square wave's discontinuous gain steps. */
    static constexpr float squareSlewMs = 1.5f;

    static const char* const* shapeLabels()
    {
        static const char* const names[] = { "Sine", "Triangle", "Square" };
        return names;
    }

    TremoloEffect()
        : PedalEffect (tremolo, "Tremolo",
            {
                { "Rate",   0.1f,  20.0f,  5.0f, "Hz" },
                { "Depth",  0.0f, 100.0f, 90.0f, "%" },
                { "Shape",  0.0f,   2.0f,  0.0f, "", 3, shapeLabels() },
                { "Spread", 0.0f, 100.0f, 30.0f, "%" },
            })
    {
    }

    void doPrepare (double newSampleRate, int newBlockSize) override
    {
        static_cast<void> (newBlockSize);

        modulator.prepare (newSampleRate);
        slewCoeff = timeCoefficient (newSampleRate, squareSlewMs);
        coefficientRate = static_cast<float> (newSampleRate);
    }

    void resetState() noexcept override
    {
        modulator.reset();
        gainL = gainR = 1.0f;
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);

        if (sampleRate != coefficientRate)
        {
            // Both the LFO step and the click-free square slew are rate dependent.
            modulator.prepare (sampleRate);
            slewCoeff = timeCoefficient (sampleRate, squareSlewMs);
            coefficientRate = static_cast<float> (sampleRate);
        }

        const auto rateHz  = clampf (getParameter (rate), 0.01f, 60.0f);
        const auto depth   = clamp01 (getParameter (Params::depth) * 0.01f);
        const auto lfoShape = shapeFor (getParameter (shape));
        const auto slewed  = lfoShape == LfoShape::square;
        const auto spreadPhase = clamp01 (getParameter (Params::spread) * 0.01f) * 0.5f;

        // There is no wet scratch buffer here: the chain buffer is multiplied in
        // place, which is what a tremolo is.
        float* pointers[chainChannels] = {};

        for (int c = 0; c < numChannels; ++c)
            pointers[c] = audio.getWritePointer (c);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto phase = modulator.step (rateHz, lfoShape);
            const auto lfoL  = modulator.valueAt (phase, lfoShape);
            const auto lfoR  = modulator.valueAt (phase + spreadPhase, lfoShape);

            // LFO +1 -> unity gain, LFO -1 -> 1 - depth, so 100 % depth goes to
            // silence at the trough.
            auto targetL = 1.0f - depth * (0.5f - 0.5f * lfoL);
            auto targetR = 1.0f - depth * (0.5f - 0.5f * lfoR);

            if (slewed)
            {
                gainL += (targetL - gainL) * slewCoeff;   // one pole on the gain
                gainR += (targetR - gainR) * slewCoeff;
                targetL = gainL;
                targetR = gainR;
            }
            else
            {
                gainL = targetL;                          // stays in step for shape switches
                gainR = targetR;
            }

            if (numChannels > 0)
                pointers[0][i] *= targetL;
            if (numChannels > 1)
                pointers[1][i] *= targetR;
        }
    }

private:
    static LfoShape shapeFor (float knobValue) noexcept
    {
        switch (static_cast<int> (knobValue + 0.5f))
        {
            case 1:  return LfoShape::triangle;
            case 2:  return LfoShape::square;
            default: return LfoShape::sine;
        }
    }

    Modulator modulator;
    float gainL = 1.0f, gainR = 1.0f, slewCoeff = 0.2f, coefficientRate = 0.0f;
};

} // namespace pedal
