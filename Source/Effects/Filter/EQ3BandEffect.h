#pragma once

#include "Effects/PedalEffect.h"

/*==============================================================================
  3 Band EQ

  Algorithm: three RBJ cookbook sections in series per channel - low shelf,
  peaking mid, high shelf - built with pedal::Biquad, which is the in place
  stand in for juce::dsp::IIR::Filter because the coefficient objects of that
  class allocate a reference counted struct on every setCoefficients.

  Coefficients are only rebuilt when one of the seven EQ values or the sample
  rate actually changed, so sweeping a knob stays click free (filter state is
  kept, only the taps change) without touching the heap. Gain = 0 dB is a valid
  flat section, so the pedal is transparent with all three bands centred.

  The equalised signal is written to the wet scratch buffer and blended into the
  chain buffer by Mix, so Mix = 0 is the raw guitar.
==============================================================================*/
namespace pedal
{

class EQ3BandEffect : public PedalEffect
{
public:
    enum Params { low, lowFreq, mid, midFreq, midQ, high, highFreq, mix, numParams };

    EQ3BandEffect()
        : PedalEffect (eq3Band, "3-Band EQ",
            {
                { "Low",       -15.0f,    15.0f,   0.0f, "dB" },
                { "Low Freq",   40.0f,   250.0f,  90.0f, "Hz" },
                { "Mid",       -15.0f,    15.0f,   0.0f, "dB" },
                { "Mid Freq",  200.0f,  8000.0f, 900.0f, "Hz" },
                { "Mid Q",       0.3f,     6.0f,   1.0f },
                { "High",      -15.0f,    15.0f,   0.0f, "dB" },
                { "High Freq", 1000.0f, 16000.0f, 4500.0f, "Hz" },
                { "Mix",         0.0f,     1.0f,   1.0f },
            })
    {
    }

    void doPrepare (double, int) override
    {
        // The cache is emptied first so the rebuild below is unconditional.
        for (auto& cached : cachedValues)
            cached = -1.0e9f;

        rebuildSections();
    }

    void resetState() noexcept override
    {
        for (int c = 0; c < chainChannels; ++c)
        {
            lowShelf[c].reset();
            midPeak[c].reset();
            highShelf[c].reset();
        }
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        if (inputsChanged())
            rebuildSections();

        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);
        auto& wet = wetBufferForBlock();

        for (int c = 0; c < numChannels; ++c)
        {
            const auto* in  = audio.getReadPointer (c);
            auto* out = wet.getWritePointer (c);

            auto& ls = lowShelf[c];
            auto& mp = midPeak[c];
            auto& hs = highShelf[c];

            for (int i = 0; i < numSamples; ++i)
                out[i] = hs.process (mp.process (ls.process (in[i])));
        }

        crossFade (audio, wet, getParameter (mix));
    }

private:
    /** Slots 0..6 mirror parameters 0..6, slot 7 the sample rate, so one
        comparison decides whether any section has to be recomputed. */
    bool inputsChanged() const noexcept
    {
        if (cachedValues[7] != sampleRate)
            return true;

        for (int i = 0; i < 7; ++i)
            if (cachedValues[i] != getParameter (i))
                return true;

        return false;
    }

    void rebuildSections() noexcept
    {
        const auto lowGain   = getParameter (low);
        const auto lowCorner = getParameter (lowFreq);
        const auto midGain   = getParameter (mid);
        const auto midQValue = getParameter (midQ);
        const auto highGain  = getParameter (high);
        const auto highCorner = getParameter (highFreq);

        /*  The bell is not clamped between the shelf corners any more. It used to
            be, which silently held Mid Freq at, say, 3 kHz while the knob and its
            readout said 8 kHz - the control lied about what the filter did. A
            peaking bell deliberately overlapping a shelf is ordinary EQ behaviour
            and is visible in the gain knobs; a frequency knob that does nothing
            over its top octave is not. */
        const auto midCenter = getParameter (midFreq);

        for (int c = 0; c < chainChannels; ++c)
        {
            // identical values for both channels; out of range requests leave a
            // passthrough section instead of breaking the chain.
            lowShelf[c].makeLowShelf (sampleRate, lowCorner, lowGain);
            midPeak[c].makePeak (sampleRate, midCenter, midQValue, midGain);
            highShelf[c].makeHighShelf (sampleRate, highCorner, highGain);
        }

        for (int i = 0; i < 7; ++i)
            cachedValues[i] = getParameter (i);

        cachedValues[7] = static_cast<float> (sampleRate);
    }

    Biquad lowShelf[chainChannels], midPeak[chainChannels], highShelf[chainChannels];
    float cachedValues[8] = { 0.0f };
};

} // namespace pedal
