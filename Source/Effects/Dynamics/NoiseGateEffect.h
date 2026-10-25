#pragma once

#include "Effects/PedalEffect.h"

#include <algorithm>
#include <cmath>

/*==============================================================================
  Noise Gate

  Algorithm: hand written. juce::dsp::NoiseGate exists in JUCE 8 but only exposes
  threshold / ratio / attack / release on an RMS detector; it has no hold stage
  and no closed range control, both of which this pedal needs.

  Detection happens on the maximum magnitude of the two channels so the stereo
  image cannot collapse when the gate closes. The detector is deliberately fast
  (0.5 ms attack) and independent of the knobs: a gate that waits for its own
  attack time before it sees the pick has already chopped the transient it was
  supposed to pass. Attack and Release only shape the gate gain itself.

  Hold freezes the gate wherever it is for the set time after the signal drops
  below the threshold, and then the release starts; it does not count from
  inside the open state, where it would never expire.

  Hysteresis separates the open and close thresholds by a few dB. Without it a
  signal sitting exactly on the threshold toggles the gate every sample, which
  is audible as a buzz on a sustained note decaying through the setting; real
  gates and expanders all ship this second threshold. The signal must rise past
  the Threshold to open and fall past Threshold - Hysteresis to begin closing.

  Sidechain is a high pass in front of the detector only, not in the audio path.
  A 50/60 Hz mains hum or a boomy low string would otherwise hold the gate open
  and let the noise through; the default 80 Hz keeps the guitar fundamental while
  rejecting the hum. The gate gain is still applied to the full-band signal.

  The processed signal is written to the wet scratch buffer and blended into the
  chain buffer by Mix, so Mix = 0 is exactly the untouched string signal. The
  detected open/closed state is published for the card footer.
==============================================================================*/
namespace pedal
{

class NoiseGateEffect : public PedalEffect
{
public:
    enum Params { threshold, attack, release, hold, range, mix, hysteresis, sidechain, numParams };

    NoiseGateEffect()
        : PedalEffect (noiseGate, "Noise Gate",
            {
                { "Threshold", -80.0f,   0.0f, -46.0f, "dB" },
                { "Attack",      0.1f,  50.0f,    1.0f, "ms" },
                { "Release",     5.0f, 1000.0f,  180.0f, "ms" },
                { "Hold",        0.0f,  200.0f,   10.0f, "ms" },
                { "Range",       0.0f,   80.0f,   60.0f, "dB" },
                { "Mix",         0.0f,    1.0f,    1.0f },
                { "Hysteresis",  0.0f,   12.0f,    3.0f, "dB" },
                { "Sidechain",  20.0f,  500.0f,   80.0f, "Hz" },
            })
    {
    }

    void doPrepare (double, int) override
    {
        detector.setTimes (sampleRate, detectorAttackMs, detectorReleaseMs);
        updateCoefficients (true);
        holdSamples  = samplesFromMilliseconds (getParameter (hold));
        updateSidechain (true);
    }

    void resetState() noexcept override
    {
        detector.reset();
        gateGain = 1.0f;
        holdCounter = 0;
        openState = false;
        gateOpen.store (false, std::memory_order_relaxed);

        for (auto& filter : sidechainFilter)
            filter.reset();
    }

    juce::String getStatusText() const override
    {
        // The engine skips process() while bypassed; without this the last state
        // would remain on the card as if the gate were still in circuit.
        if (isBypassed())
            return {};

        return tr (gateOpen.load (std::memory_order_relaxed) ? "Open" : "Closed");
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        updateCoefficients (false);
        holdSamples = samplesFromMilliseconds (getParameter (hold));
        updateSidechain (false);

        const auto thresholdDb = getParameter (threshold);
        const auto closeDb     = thresholdDb - getParameter (hysteresis);
        const auto closedGain  = dbToGain (-getParameter (range));

        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);

        auto& wet = wetBufferForBlock();

        bool isOpen = openState;

        for (int i = 0; i < numSamples; ++i)
        {
            float level = 0.0f;
            for (int c = 0; c < numChannels; ++c)
                level = std::max (level, std::abs (sidechainFilter[c].process (audio.getReadPointer (c)[i])));

            const auto env = detector.process (level);
            const auto levelDb = gainToDb (env);

            // Two thresholds: a closed gate needs the signal to rise past
            // Threshold, an open one only falls below Threshold - Hysteresis.
            const bool open = isOpen ? levelDb > closeDb : levelDb > thresholdDb;

            if (open)
            {
                holdCounter = holdSamples;
                gateGain += (1.0f - gateGain) * attackCoeff;
            }
            else if (holdCounter > 0)
            {
                // frozen where it is: the release starts when the hold expires
                --holdCounter;
            }
            else
            {
                gateGain += (closedGain - gateGain) * releaseCoeff;
            }

            gateGain = clamp01 (gateGain);
            isOpen = open;

            for (int c = 0; c < numChannels; ++c)
                wet.getWritePointer (c)[i] = audio.getReadPointer (c)[i] * gateGain;
        }

        openState = isOpen;

        /*  "Open" means the gate is letting signal through, not merely that the
            detector is above its threshold. During Hold the detector can already
            be below the close threshold while the gate gain is frozen wide open;
            reporting Closed there would contradict what the player can hear. The
            midpoint between the closed gain and unity crosses during the release,
            so the lamp goes out when the gate has actually attenuated. */
        gateOpen.store (gateGain >= 0.5f * (1.0f + closedGain), std::memory_order_relaxed);

        crossFade (audio, wet, getParameter (mix));
    }

private:
    int samplesFromMilliseconds (float ms) const noexcept
    {
        return juce::jmax (0, static_cast<int> (ms * 0.001 * sampleRate));
    }

    /** Recomputes the gain envelope coefficients when the knobs or the device
        rate actually move. They used to be computed only in doPrepare(), so
        turning Attack or Release on the running card changed nothing until the
        audio device was restarted. */
    void updateCoefficients (bool force) noexcept
    {
        const auto newAttack  = getParameter (attack);
        const auto newRelease = getParameter (release);
        const auto rate       = static_cast<float> (sampleRate);

        if (! force && newAttack == cachedAttack && newRelease == cachedRelease
            && rate == coefficientRate)
            return;

        attackCoeff  = timeCoefficient (sampleRate, newAttack);
        releaseCoeff = timeCoefficient (sampleRate, newRelease);
        cachedAttack = newAttack;
        cachedRelease = newRelease;
        coefficientRate = rate;
    }

    /** Detector-only high pass, so mains hum below the guitar's range cannot
        hold the gate open. Cached: the one pole is rebuilt only when the knob
        or the device rate actually moves. */
    void updateSidechain (bool force) noexcept
    {
        const auto hz   = getParameter (sidechain);
        const auto rate = static_cast<float> (sampleRate);

        if (! force && hz == cachedSidechainHz && rate == sidechainRate)
            return;

        for (auto& filter : sidechainFilter)
            filter.setHighPass (sampleRate, hz);

        cachedSidechainHz = hz;
        sidechainRate = rate;
    }

    /** The detector must see a pick the moment it happens; the knobs shape the
        gain, not the detection. */
    static constexpr float detectorAttackMs = 0.5f, detectorReleaseMs = 30.0f;

    EnvelopeFollower detector;
    std::atomic<bool> gateOpen { false };
    bool openState = false;
    float gateGain = 1.0f, attackCoeff = 0.5f, releaseCoeff = 0.05f;
    float cachedAttack = -1.0f, cachedRelease = -1.0f;
    float coefficientRate = 0.0f;
    float cachedSidechainHz = -1.0f, sidechainRate = 0.0f;
    int holdCounter = 0, holdSamples = 0;
    OnePole sidechainFilter[chainChannels];
};

} // namespace pedal
