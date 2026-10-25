#pragma once

#include <juce_dsp/juce_dsp.h>

#include "Common/PedalConfig.h"
#include "Common/PedalDsp.h"
#include "Common/PedalLanguage.h"

#include <vector>
#include <cmath>
#include <atomic>

/*==============================================================================
  PedalBoard - effect base class.

  Contract every effect in Source/Effects obeys:

    * Parameters live in a fixed array of std::atomic<float>. The message thread
      writes them, the audio thread reads them with relaxed loads. No setter is
      ever called on the audio thread.
    * Bypass is a single atomic test performed by the engine before process() is
      entered. When an effect is bypassed the chain buffer simply keeps the audio
      that was already in it, which is the dry signal - so bypass never
      reconnects anything and never allocates.
    * Anything that needs memory (delay lines, IRs, scratch buffers,
      oversamplers) is sized in doPrepare(), which only ever runs on the audio
      thread while the device is stopped, or on the thread that owns the object
      before publication.
    * getLatencySamples() is used by the engine to delay the global dry path by
      exactly the same amount as the wet path, which is what keeps the global
      Dry/Wet control from turning into a comb filter.
==============================================================================*/
namespace pedal
{

struct ParamDescription
{
    const char* name       = "";
    float minValue         = 0.0f;
    float maxValue         = 1.0f;
    float defaultValue     = 0.5f;
    const char* unit       = "";
    int   numSteps         = 0;                  ///< 0 = continuous, >= 2 = discrete selector
    const char* const* stepLabels = nullptr;     ///< nullptr unless numSteps >= 2
};

struct EffectAction
{
    int id;
    const char* label;
};

/** Things the engine shares with the effects that need them. */
struct EffectContext
{
    /** Continuous 0..1 controller coming from a MIDI CC (expression pedal),
        written by the engine's MIDI callback, read by the wah pedal. */
    std::atomic<float>* expressionPosition = nullptr;

    /** True while a MIDI input is actually open. The wah falls back to its
        Position knob when nobody is sending CC 11, instead of sitting frozen at
        whatever value the expression atomic happened to hold. */
    std::atomic<bool>* expressionMidiActive = nullptr;

    /** Shared background message queue for juce::dsp::Convolution. It has to
        outlive every convolution that uses it, so the engine owns it. */
    juce::dsp::ConvolutionMessageQueue* convolutionQueue = nullptr;
};

class PedalEffect
{
public:
    PedalEffect (int typeID, juce::String cardTitle, std::vector<ParamDescription> descs)
        : type (typeID),
          title (std::move (cardTitle)),
          descriptors (std::move (descs))
    {
        jassert (static_cast<int> (descriptors.size()) <= maxParamsPerEffect);

        for (std::size_t i = 0; i < descriptors.size(); ++i)
            parameters[i].store (descriptors[i].defaultValue, std::memory_order_relaxed);
    }

    virtual ~PedalEffect() = default;

    //==============================================================================
    //  Audio thread
    //==============================================================================
    void prepare (double newSampleRate, int newBlockSize)
    {
        sampleRate = newSampleRate;
        blockSize  = newBlockSize;

        wetBuffer.setSize (chainChannels, newBlockSize, false, false, true);
        resetState();
        doPrepare (newSampleRate, newBlockSize);
    }

    virtual void doPrepare (double newSampleRate, int newBlockSize) = 0;

    /** Process the chain buffer in place. Only called when not bypassed. */
    virtual void process (juce::AudioBuffer<float>& audio) = 0;

    virtual void resetState() noexcept {}

    /** Group delay of this effect in samples at the current block size, used by
        the engine for dry path compensation. */
    virtual int getLatencySamples() const noexcept { return 0; }

    bool isBypassed() const noexcept { return bypassed.load (std::memory_order_relaxed); }
    void setBypassed (bool shouldBeBypassed) noexcept { bypassed.store (shouldBeBypassed, std::memory_order_relaxed); }

    //==============================================================================
    //  Message thread
    //==============================================================================
    virtual std::vector<EffectAction> actions() const { return {}; }
    virtual void handleAction (int) {}

    /** Short single line status for the card, e.g. the looper transport state or
        the loaded impulse response. Implemented from atomics or message-thread
        only members, never from audio thread state. */
    virtual juce::String getStatusText() const { return {}; }

    /** Extra guidance shown on the card, e.g. the monophonic tracking caveat. */
    virtual juce::String getWarningText() const { return {}; }

    //==============================================================================
    int getTypeID() const noexcept { return type; }
    const juce::String& getTitle() const noexcept { return title; }
    int getNumParameters() const noexcept { return static_cast<int> (descriptors.size()); }
    const std::vector<ParamDescription>& getParameters() const noexcept { return descriptors; }
    const ParamDescription& getParameterDescription (int i) const noexcept { return descriptors[static_cast<std::size_t> (i)]; }

    /** Audio thread read. */
    float getParameter (int index) const noexcept
    {
        return parameters[static_cast<std::size_t> (index)].load (std::memory_order_relaxed);
    }

    /** Message thread write. */
    void setParameter (int index, float value) noexcept
    {
        parameters[static_cast<std::size_t> (index)].store (value, std::memory_order_relaxed);
        parameterWasSet (index, value);
    }

    /** Called after setParameter(), on the message thread. Effects that mirror
        their knobs into something outside the parameter array (the plugin host)
        override this; the default keeps the existing effects unchanged. */
    virtual void parameterWasSet (int, float) {}

    std::atomic<float>& getRawParameter (int index) noexcept
    {
        return parameters[static_cast<std::size_t> (index)];
    }

    /** Reset one knob to its documented default (double click on the card). */
    void resetParameterToDefault (int index) noexcept
    {
        setParameter (index, descriptors[static_cast<std::size_t> (index)].defaultValue);
    }

    //==============================================================================
protected:
    /** Wet path scratch buffer, already sized for the current block. */
    juce::AudioBuffer<float>& wetBufferForBlock() noexcept { return wetBuffer; }

    /** out = dry * (1 - mix) + wet * mix, in place on dry. */
    static void crossFade (juce::AudioBuffer<float>& dry,
                           const juce::AudioBuffer<float>& wet,
                           float mix) noexcept
    {
        const auto numChannels = dry.getNumChannels();
        const auto numSamples  = dry.getNumSamples();
        const auto m = clamp01 (mix);
        const auto d = 1.0f - m;

        for (int c = 0; c < numChannels; ++c)
        {
            auto* out = dry.getWritePointer (c);
            const auto* in = wet.getReadPointer (c);

            for (int i = 0; i < numSamples; ++i)
                out[i] = out[i] * d + in[i] * m;
        }
    }

    /** Call from doPrepare() when the wet path has latency and process()
        crossfades it with the dry signal. Without it the dry copy would be
        latency samples ahead of the wet one, so any Mix strictly between 0 and 1
        comb filters the output - which is exactly what a 4x oversampled drive
        pedal must not do to a guitar. Sizes the delay line, so doPrepare only. */
    void prepareLatencyCompensatedDry (int latencySamples) noexcept
    {
        dryLatency = juce::jmax (0, latencySamples);
        dryDelay.prepare (chainChannels, dryLatency + 4);
        dryDelay.reset();
    }

    /** out = dry (delayed by the effect's latency) * (1 - mix) + wet * mix.
        Writes over audio, and is safe to call after audio has been copied into
        the wet buffer: it reads each dry sample before overwriting it. */
    void mixWithDelayedDry (juce::AudioBuffer<float>& audio,
                            const juce::AudioBuffer<float>& wet,
                            float mix) noexcept
    {
        mixWithDelayedDry (audio, wet, mix, dryLatency);
    }

    /** As above, but for an effect whose latency is only known after its impulse
        response finishes loading (the cabinet). The line must have been sized
        for the largest value passed here in doPrepare(). */
    void mixWithDelayedDry (juce::AudioBuffer<float>& audio,
                            const juce::AudioBuffer<float>& wet,
                            float mix,
                            int latencySamples) noexcept
    {
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);
        const auto numSamples  = audio.getNumSamples();
        const auto m = clamp01 (mix);
        const auto d = 1.0f - m;
        const auto latency = static_cast<float> (juce::jlimit (0, juce::jmax (0, dryDelay.getCapacity() - 2),
                                                              latencySamples));

        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < numChannels; ++c)
                dryDelay.write (c, audio.getReadPointer (c)[i]);

            for (int c = 0; c < numChannels; ++c)
                audio.getWritePointer (c)[i] = d * dryDelay.read (c, latency)
                                             + m * wet.getReadPointer (c)[i];

            dryDelay.advance();
        }
    }

    /** Fills a buffer with zeros without touching the heap. */
    static void silence (juce::AudioBuffer<float>& b) noexcept { b.clear(); }

    double sampleRate = 44100.0;
    int blockSize = 512;

    const int type;
    const juce::String title;
    const std::vector<ParamDescription> descriptors;

    std::atomic<float> parameters[maxParamsPerEffect];
    std::atomic<bool> bypassed { false };

private:
    juce::AudioBuffer<float> wetBuffer;
    TappedDelay dryDelay;
    int dryLatency = 0;
};

/** Parameter value formatted the way the card shows it. */
inline juce::String describeParameterValue (const ParamDescription& d, float value)
{
    if (d.numSteps >= 2 && d.stepLabels != nullptr)
    {
        const auto index = juce::jmin (d.numSteps - 1, juce::jmax (0, static_cast<int> (value + 0.5f)));
        return d.stepLabels[index];
    }

    const auto magnitude = std::abs (value);
    const auto decimals = magnitude >= 100.0f ? 0 : (magnitude >= 10.0f ? 1 : 2);
    return juce::String (value, decimals) + (d.unit[0] != '\0' ? juce::String (" ") + d.unit : juce::String());
}

} // namespace pedal
