#pragma once

#include "Effects/PedalEffect.h"
#include "Effects/Distortion/OverdriveEffect.h"
#include "Effects/Modulation/ChorusEffect.h"
#include "Effects/TimeSpace/DelayEffect.h"
#include "Effects/TimeSpace/ReverbEffect.h"

#include <array>

/*==============================================================================
  Custom Chain

  A single pedal assembled from four proven stages that are otherwise separate
  effects: drive (ADAA tanh overdrive), chorus (modulated delay), delay (tape
  style at zero modulation) and reverb (Dattorro plate). The eight knobs are the
  controls a player actually reaches for across those categories; the internal
  stages are always fully wired, so moving one knob cannot leave an undefined
  routing behind.

    Drive        overdrive input gain   (0..100)
    Tone         overdrive tone low-pass (0..100)
    Chorus       chorus wet amount      (0..100)
    Delay Time   delay tap              (40..1200 ms)
    Feedback     delay feedback         (0..90 %)
    Reverb       reverb wet amount      (0..100)
    Decay        reverb decay           (0..100)
    Mix          dry / chain blend      (0..1)

  This is the "custom effect" for players who do not want to load a plugin: it
  is a fixed signal path with all the useful controls exposed, so it behaves
  like a preset made into a pedal rather than a blank rack. The Plugin Host
  card is the fully open version of the same idea.

  The composite is fully wet inside; the global Mix is latency compensated for
  the overdrive's 4x oversampler. Chorus, delay and reverb report no latency,
  so the composite reports exactly the overdrive's latency.
==============================================================================*/
namespace pedal
{

class CustomChainEffect : public PedalEffect
{
public:
    enum Params { driveAmount, tone, chorusAmount, delayTime, feedback, reverbAmount, decay, mix, numParams };

    CustomChainEffect()
        : PedalEffect (customChain, "Custom Chain",
            {
                { "Drive",       0.0f, 100.0f,  30.0f },
                { "Tone",        0.0f, 100.0f,  55.0f },
                { "Chorus",      0.0f, 100.0f,  25.0f, "%" },
                { "Delay Time", 40.0f, 1200.0f, 350.0f, "ms" },
                { "Feedback",    0.0f,  90.0f,  30.0f, "%" },
                { "Reverb",      0.0f, 100.0f,  25.0f, "%" },
                { "Decay",       0.0f, 100.0f,  50.0f, "%" },
                { "Mix",         0.0f,   1.0f,   0.55f },
            })
    {
    }

    void doPrepare (double, int) override
    {
        drive.prepare (sampleRate, blockSize);
        chorus.prepare (sampleRate, blockSize);
        delay.prepare (sampleRate, blockSize);
        reverb.prepare (sampleRate, blockSize);

        latencySamples = drive.getLatencySamples();
        prepareLatencyCompensatedDry (latencySamples);
    }

    int getLatencySamples() const noexcept override { return latencySamples; }

    juce::String getWarningText() const override
    {
        return tr ("One custom chain: drive, tone, chorus, delay, reverb - all stages share these knobs.");
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        const auto numSamples = audio.getNumSamples();

        if (numSamples <= 0)
            return;

        auto& wet = wetBufferForBlock();

        for (int c = 0; c < chainChannels; ++c)
            wet.copyFrom (c, 0, audio, c, 0, numSamples);

        updateStages();

        // The stages use their own scratch buffers; this view only bounds the
        // block that is being processed, so no stage sees the stale tail of the
        // maximum-size scratch buffer.
        juce::AudioBuffer<float> view (wet.getArrayOfWritePointers(), chainChannels, numSamples);

        drive.process (view);
        chorus.process (view);
        delay.process (view);
        reverb.process (view);

        mixWithDelayedDry (audio, view, getParameter (mix));
    }

private:
    void updateStages() noexcept
    {
        drive.setParameter (OverdriveEffect::drive, getParameter (driveAmount));
        drive.setParameter (OverdriveEffect::tone, getParameter (tone));
        drive.setParameter (OverdriveEffect::level, 50.0f);
        drive.setParameter (OverdriveEffect::mix, 1.0f);
        drive.setParameter (OverdriveEffect::bass, 0.0f);

        chorus.setParameter (ChorusEffect::rate, 0.45f);
        chorus.setParameter (ChorusEffect::depth, 38.0f);
        chorus.setParameter (ChorusEffect::delayMs, 12.0f);
        chorus.setParameter (ChorusEffect::mix, getParameter (chorusAmount) * 0.01f);
        chorus.setParameter (ChorusEffect::spread, 80.0f);

        delay.setParameter (DelayEffect::time, getParameter (delayTime));
        delay.setParameter (DelayEffect::feedback, getParameter (feedback) * 0.01f);
        delay.setParameter (DelayEffect::tone, 60.0f);
        delay.setParameter (DelayEffect::mix, 0.30f);
        delay.setParameter (DelayEffect::mode, 0.0f);
        delay.setParameter (DelayEffect::modulation, 0.0f);

        reverb.setParameter (ReverbEffect::decay, getParameter (decay) * 0.01f);
        reverb.setParameter (ReverbEffect::damping, 0.40f);
        reverb.setParameter (ReverbEffect::width, 0.80f);
        reverb.setParameter (ReverbEffect::mix, getParameter (reverbAmount) * 0.01f);
        reverb.setParameter (ReverbEffect::preDelay, 20.0f);
    }

    OverdriveEffect drive;
    ChorusEffect chorus;
    DelayEffect delay;
    ReverbEffect reverb;
    int latencySamples = 0;
};

} // namespace pedal
