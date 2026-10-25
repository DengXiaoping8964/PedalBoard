#pragma once

#include "Effects/PedalEffect.h"

#if defined (_MSC_VER)
 #pragma warning(push)
 // Third-party template header: narrows a few double literals to float and uses
 // names that shadow its own locals. Not our code to rewrite.
 #pragma warning(disable: 4127 4244 4456 4458)
#endif
#include <signalsmith-stretch/signalsmith-stretch.h>
#if defined (_MSC_VER)
 #pragma warning(pop)
#endif

#include <vector>

/*==============================================================================
  Pitch Shifter

  Algorithm: Signalsmith Stretch 1.3.2 (MIT), a spectral phase-vocoder pitch
  shifter. This replaces the hand written granular engine that shipped here
  before: two Hann grains crossfading at every hop can only line up if their
  phases happen to match, so held notes warbled and a low E never locked. The
  library is the successful published solution this pedal needed.

  Formant: the library separates the spectral envelope from the harmonic
  positions, so Formant can be moved independently of Semitones. At 0 the
  guitar keeps its body at every interval; moving it changes the apparent size
  of the instrument without changing the note.

  Window/latency: the analysis window is fixed at 80 ms with a 20 ms hop,
  configured once in doPrepare(). Reconfiguring a spectral engine allocates,
  which cannot happen on the audio thread, so the earlier Window knob is gone.
  Its replacement is Formant; old settings are migrated in the engine (the
  `version=2` settings marker). Reported latency is the library's own
  input+output latency, measured on the bench against an impulse.

  The chain is stereo but the guitar is mono. The engine is configured with one
  channel and the result is written to both sides, so the two sides cannot
  drift apart when their phases are tracked independently.
==============================================================================*/
namespace pedal
{

class PitchShifterEffect : public PedalEffect
{
public:
    enum Params { semitones, formant, mix, numParams };

    PitchShifterEffect()
        : PedalEffect (pitchShifter, "Pitch Shifter",
            {
                { "Semitones", -12.0f, 12.0f, 12.0f, "st" },
                { "Formant",   -12.0f, 12.0f,  0.0f, "st" },
                { "Mix",         0.0f,  1.0f,  1.0f },
            })
    {
    }

    void doPrepare (double, int) override
    {
        const auto interval = juce::jmax (64, static_cast<int> (sampleRate * 0.020));
        const auto block    = juce::jmax (interval * 2, static_cast<int> (sampleRate * 0.080));

        stretch.configure (1, block, interval);
        stretch.setFormantBase (0.0f);          // let the library detect the fundamental
        configured = true;

        latencySamples = stretch.inputLatency() + stretch.outputLatency();

        // Sizes the dry compensation line; allocating it in process() would be an
        // audio-thread allocation.
        prepareLatencyCompensatedDry (latencySamples);

        monoIn.assign (static_cast<std::size_t> (blockSize), 0.0f);
        monoOut.assign (static_cast<std::size_t> (blockSize), 0.0f);

        applyControls();
    }

    // PedalEffect::prepare() calls resetState() before doPrepare(), so the first
    // call arrives while the spectral engine still has zero channels configured;
    // resetting it then reads past its empty band tables. doPrepare() configures
    // (and resets) it itself.
    void resetState() noexcept override
    {
        if (configured)
            stretch.reset();
    }

    int getLatencySamples() const noexcept override { return latencySamples; }

    juce::String getWarningText() const override
    {
        return tr ("Spectral pitch shifter: clean tracking on chords and single notes, about 80 ms latency.");
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        applyControls();

        const auto numSamples = audio.getNumSamples();

        if (numSamples <= 0)
            return;

        const auto* left  = audio.getReadPointer (0);
        const auto* right = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : left;

        for (int i = 0; i < numSamples; ++i)
            monoIn[static_cast<std::size_t> (i)] = 0.5f * (left[i] + right[i]);

        float* inChannels[1]  = { monoIn.data() };
        float* outChannels[1] = { monoOut.data() };

        stretch.process (inChannels, numSamples, outChannels, numSamples);

        auto& wet = wetBufferForBlock();

        for (int c = 0; c < chainChannels; ++c)
        {
            auto* out = wet.getWritePointer (c);

            for (int i = 0; i < numSamples; ++i)
                out[i] = monoOut[static_cast<std::size_t> (i)];
        }

        // The wet path is the shifted signal, so the dry side has to arrive at the
        // same time; without this Mix between 0 and 1 would comb.
        mixWithDelayedDry (audio, wet, getParameter (mix));
    }

private:
    void applyControls() noexcept
    {
        stretch.setTransposeSemitones (clampf (getParameter (semitones), -12.0f, 12.0f));
        stretch.setFormantSemitones (clampf (getParameter (formant), -12.0f, 12.0f));
    }

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    std::vector<float> monoIn, monoOut;
    int latencySamples = 0;
    bool configured = false;
};

} // namespace pedal
