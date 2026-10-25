#pragma once

#include <cmath>
#include <algorithm>

#include "Common/PedalConfig.h"
#include "Common/PedalDsp.h"

/*==============================================================================
  Sub-octave divider (mono).

  What it is: the classic analogue-pedal trick, not a pitch shifter. Rising
  zero-crossings of the input are counted, and a phase accumulator is snapped to
  0 on every second one, so the output frequency is exactly half the input - no
  estimation, no look-ahead, no smear. That is also why it cannot do two notes at
  once: a divider has nothing to lock to when the input has more than one
  periodicity.

  What was here before: this class lived at the bottom of GranularPitchShifter.h,
  which is a bad home for it - the two share nothing but a directory. It is here
  on its own now, and the file it came from is gone because the shifter it carried
  was replaced by a spectral engine (Signalsmith Stretch).

  Deliberate limits, stated rather than hidden:
    * The sub is a one-pole-smoothed square, so it keeps an edge. That is the
      sound of an octaver; a pure sine would be a different pedal.
    * Lock range is 30 Hz to 2 kHz of input, which covers a guitar's low E at 82 Hz
      with room for a down-tuned six-string and its harmonics.
    * When the input stops, the sub fades out over about 40 ms rather than
      continuing to oscillate on noise.
==============================================================================*/
namespace pedal
{

class SubOctaveDivider
{
public:
    void prepare (double newSampleRate, int /*blockSize*/)
    {
        sampleRate = newSampleRate;

        // A guitar's lowest fundamental is around 80 Hz, so the DC blocker has to
        // stay well below it or the sub loses the low E.
        averageCoeff = timeCoefficient (sampleRate, 40.0f);

        /*  timeCoefficient takes a time constant in milliseconds, not a cutoff in
            hertz: 1 / (2 pi 850) s is 0.19 ms. Passing 850 here the first time made
            the "shape the square" pole so slow that it averaged the waveform to
            near zero, and the bench measured a sub-octave that was exactly the
            right pitch and 50 dB too quiet to hear. */
        shapeCoeff   = timeCoefficient (sampleRate, 0.19f);
        attackCoeff  = timeCoefficient (sampleRate, 4.0f);
        releaseCoeff = timeCoefficient (sampleRate, 150.0f);
        fadeCoeff    = timeCoefficient (sampleRate, 25.0f);
        periodCoeff  = timeCoefficient (sampleRate, 60.0f);

        minPeriod = static_cast<int> (sampleRate / 2000.0);
        maxPeriod = static_cast<int> (sampleRate / 30.0);

        reset();
    }

    void reset() noexcept
    {
        runningAverage = 0.0f;
        magnitude = 0.0f;
        subFilter = 0.0f;
        subPhase = 0.0;
        gate = 0.0f;
        highState = false;
        edges = 0;
        sinceCrossing = 0;
        period = 0.0;
        subIncrement = 1.0 / 882.0;   // ~220 Hz input at 44.1 kHz, until the first lock
    }

    /** `level` is the pedal's own dry/wet amount for this path, applied here so the
        caller can mix the two octave engines without a second buffer. */
    void process (const float* in, float* out, int numSamples, float level) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const auto raw = in[i];

            runningAverage += averageCoeff * (raw - runningAverage);
            const auto centred = raw - runningAverage;

            const auto absolute = std::abs (centred);
            magnitude += (absolute > magnitude ? attackCoeff : releaseCoeff) * (absolute - magnitude);

            // Hysteresis, so a slow or noisy crossing counts once rather than eight
            // times. The threshold tracks the signal, with a floor for silence.
            const auto threshold = juce::jmax (0.008f, magnitude * 0.35f);

            if (! highState && centred > threshold)
            {
                highState = true;
                onRisingEdge (centred);
            }
            else if (highState && centred < -threshold)
            {
                highState = false;
            }

            ++sinceCrossing;

            // Nothing has crossed for too long: the string has stopped, so let the
            // sub decay instead of oscillating on the noise floor.
            const auto target = sinceCrossing > maxPeriod * 2 ? 0.0f : 1.0f;
            gate += fadeCoeff * (target - gate);

            subPhase += subIncrement;
            if (subPhase >= 1.0)
                subPhase -= 1.0;

            const auto square = subPhase < 0.5 ? 1.0f : -1.0f;
            subFilter += shapeCoeff * (square - subFilter);

            const auto envelope = clamp01 (magnitude * 6.0f);

            out[i] = subFilter * envelope * gate * level;
        }
    }

private:
    void onRisingEdge (float) noexcept
    {
        const auto gap = sinceCrossing;
        sinceCrossing = 0;

        if (gap >= minPeriod && gap <= maxPeriod)
        {
            period += periodCoeff * (static_cast<double> (gap) - period);

            if (period > 1.0)
                subIncrement = 1.0 / (2.0 * period);
        }

        /*  Every second rising edge of the input is one full cycle of the sub, so
            snapping the accumulator there is what makes the output *exactly* half
            the input frequency. Between edges the accumulator free-runs on the
            smoothed period, which is what keeps the waveform continuous. */
        ++edges;
        subPhase = (edges & 1) ? 0.0 : 0.5;
    }

    double sampleRate = 44100.0;
    float averageCoeff = 0.0f, shapeCoeff = 0.0f, attackCoeff = 0.0f, releaseCoeff = 0.0f;
    float fadeCoeff = 0.0f, periodCoeff = 0.0f;

    float runningAverage = 0.0f, magnitude = 0.0f, subFilter = 0.0f, gate = 0.0f;
    double subPhase = 0.0, subIncrement = 0.0, period = 0.0;

    int minPeriod = 1, maxPeriod = 1000, sinceCrossing = 0, edges = 0;
    bool highState = false;
};

} // namespace pedal
