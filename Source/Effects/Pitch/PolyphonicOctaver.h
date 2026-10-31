#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

/*==============================================================================
  Polyphonic octave generator

  The zero-crossing divider in the old octaver is monophonic by construction:
  on a real guitar note its comparator sees the harmonics as extra crossings and
  toggles the sub phase several times per string period. That is heard as a
  high-frequency buzz, not as an octave. The full-wave rectifier has the other
  problem: it folds the whole spectrum and produces intermodulation on chords.

  This is the phase-scaling filterbank described by Etienne Thuillier,
  "Real-Time Polyphonic Octave Doubling for the Guitar" (Aalto, 2016), and used
  by the open-source Terrarium poly-octave pedal (MIT). The input is split into
  overlapping analytic bands; within each band the complex signal is scaled by
  |y|^(g-1), which doubles or halves its phase rate without a window or a grain:
  one octave up (g = 2), one octave down (g = 0.5). Summing the bands gives a
  polyphonic result with no note tracking and no look-ahead.

  The phase of a bin with no energy is meaningless, so each band is gated below
  a magnitude floor and the summed output follows the input envelope. That is
  what keeps silence silent instead of turning the filterbank's numerical noise
  into hiss.
==============================================================================*/
namespace pedal
{

class PolyphonicOctaver
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = std::max (8000.0, newSampleRate);
        bands.resize (numBands);

        for (int i = 0; i < numBands; ++i)
        {
            auto& band = bands[static_cast<std::size_t> (i)];

            const auto centre = centreFrequency (i);
            const auto f0 = centreFrequency (i - 1);
            const auto f1 = centreFrequency (i);
            const auto f2 = centreFrequency (i + 1);
            const auto bw = std::max (8.0, 2.0 * (f2 - f1) * (f1 - f0)
                                               / std::max (1.0e-6, (f2 - f1) + (f1 - f0)));

            constexpr double pi = 3.14159265358979323846;
            const auto w0 = pi * bw / sampleRate;
            const auto cosW0 = std::cos (w0);
            const auto sinW0 = std::sin (w0);
            const auto a0 = 1.0 + std::sqrt (2.0) * sinW0 * 0.5;
            const auto g = (1.0 - cosW0) / (2.0 * a0);

            const auto w1 = 2.0 * pi * centre / sampleRate;
            const std::complex<double> e1 (std::cos (w1), std::sin (w1));
            const std::complex<double> e2 (std::cos (2.0 * w1), std::sin (2.0 * w1));

            band.d0 = static_cast<float> (g);
            band.d1 = std::complex<float> (static_cast<float> ((e1 * 2.0 * g).real()),
                                           static_cast<float> ((e1 * 2.0 * g).imag()));
            band.d2 = std::complex<float> (static_cast<float> ((e2 * g).real()),
                                           static_cast<float> ((e2 * g).imag()));
            band.c1 = std::complex<float> (static_cast<float> ((e1 * (-2.0 * cosW0) / a0).real()),
                                           static_cast<float> ((e1 * (-2.0 * cosW0) / a0).imag()));
            band.c2 = std::complex<float> (static_cast<float> ((e2 * (1.0 - std::sqrt (2.0) * sinW0 * 0.5) / a0).real()),
                                           static_cast<float> ((e2 * (1.0 - std::sqrt (2.0) * sinW0 * 0.5) / a0).imag()));
            band.reset();
        }

        // The filterbank already limits the bands; this only keeps the very top
        // of the up-octave path and any residual DC out of the output.
        outputLowPass = static_cast<float> (1.0 - std::exp (-2.0 * 3.14159265358979323846 * 7000.0 / sampleRate));
        dcCoeff = static_cast<float> (std::exp (-2.0 * 3.14159265358979323846 * 25.0 / sampleRate));

        envelopeCoeff = static_cast<float> (1.0 - std::exp (-1.0 / (0.008 * sampleRate)));
        releaseCoeff  = static_cast<float> (1.0 - std::exp (-1.0 / (0.090 * sampleRate)));
        gainCoeff     = static_cast<float> (1.0 - std::exp (-1.0 / (0.015 * sampleRate)));
        reset();
    }

    void reset()
    {
        for (auto& band : bands)
            band.reset();

        envelope = 0.0f;
        lowPassState = 0.0f;
        lowPassDownState = 0.0f;
        dcState = 0.0f;
        dcLast = 0.0f;
        dcDownState = 0.0f;
        dcDownLast = 0.0f;
        upGain = 0.0f;
        downGain = 0.0f;
    }

    /** Adds the two octave voices into up[] and down[]; both arrays are mono. */
    void process (const float* in, float* up, float* down, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const auto x = in[i];
            float sumUp = 0.0f;
            float sumDown = 0.0f;

            for (auto& band : bands)
            {
                const auto prevImag = band.y.imag();
                const auto y = band.s2 + band.d0 * x;
                band.s2 = band.s1 + band.d1 * x - band.c1 * y;
                band.s1 = band.d2 * x - band.c2 * y;
                band.y = y;

                const auto a = y.real();
                const auto b = y.imag();
                const auto power = a * a + b * b;

                if (power < silenceFloor)
                    continue;

                const auto inv = 1.0f / std::sqrt (power);

                // +1 octave: real part of y * (y/|y|)
                sumUp += (a * a - b * b) * inv;

                // -1 octave: real part of y * (y/|y|)^(-0.5), with the sign
                // restored whenever the analytic phase crosses the branch cut.
                if (band.y.real() < 0.0f
                    && (std::signbit (band.y.imag()) != std::signbit (prevImag)))
                    band.downSign = -band.downSign;

                const auto xHalf = 0.5f * a * inv;
                const auto root1 = std::sqrt (std::max (0.0f, 0.5f + xHalf));
                const auto root2 = std::sqrt (std::max (0.0f, 0.5f - xHalf)) * (b < 0.0f ? -1.0f : 1.0f);
                sumDown += band.downSign * (a * root1 + b * root2);
            }

            // A little normalisation, then the output gate. Without the gate the
            // filterbank rings after a muted string and the sum is audible hiss.
            sumUp *= bandNormalise;
            sumDown *= bandNormalise;

            const auto raw = std::abs (x);
            envelope += (raw > envelope ? envelopeCoeff : releaseCoeff) * (raw - envelope);
            const auto gate = clamp01 (envelope * 20.0f);

            upGain += gainCoeff * (upTarget - upGain);
            downGain += gainCoeff * (downTarget - downGain);

            lowPassState += outputLowPass * (sumUp - lowPassState);
            dcState = dcCoeff * (dcState + lowPassState - dcLast);
            dcLast = lowPassState;

            lowPassDownState += outputLowPass * (sumDown - lowPassDownState);
            dcDownState = dcCoeff * (dcDownState + lowPassDownState - dcDownLast);
            dcDownLast = lowPassDownState;

            up[i] = dcState * gate * upGain;
            down[i] = dcDownState * gate * downGain;
        }
    }

    void setLevels (float upLevel, float downLevel) noexcept
    {
        upTarget = clamp01 (upLevel);
        downTarget = clamp01 (downLevel);
    }

private:
    static constexpr int numBands = 80;
    static constexpr float silenceFloor = 1.0e-10f;
    static constexpr float bandNormalise = 2.0f;

    static double centreFrequency (int n) noexcept
    {
        return std::max (20.0, 480.0 * std::pow (2.0, 0.027 * n) - 420.0);
    }

    struct Band
    {
        std::complex<float> d0 {}, d1 {}, d2 {}, c1 {}, c2 {};
        std::complex<float> s1 {}, s2 {}, y {};
        float downSign = 1.0f;

        void reset() noexcept
        {
            s1 = s2 = y = {};
            downSign = 1.0f;
        }
    };

    static float clamp01 (float v) noexcept { return std::min (1.0f, std::max (0.0f, v)); }

    std::vector<Band> bands;
    double sampleRate = 44100.0;
    float outputLowPass = 0.0f, dcCoeff = 0.0f;
    float envelope = 0.0f, envelopeCoeff = 0.0f, releaseCoeff = 0.0f;
    float gainCoeff = 0.0f;
    float lowPassState = 0.0f, lowPassDownState = 0.0f;
    float dcState = 0.0f, dcLast = 0.0f;
    float dcDownState = 0.0f, dcDownLast = 0.0f;
    float upGain = 0.0f, downGain = 0.0f;
    float upTarget = 0.0f, downTarget = 0.0f;
};

} // namespace pedal
