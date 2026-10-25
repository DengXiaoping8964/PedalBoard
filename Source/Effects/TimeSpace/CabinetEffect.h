#pragma once

#include <PedalBoardIRData.h>

#include "Effects/PedalEffect.h"

// juce::FileChooser and juce::FileBrowserComponent live in juce_gui_basics, which
// juce_dsp does not pull in: this TU needs to ask for it directly.
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <vector>
#include <array>
#include <memory>
#include <atomic>
#include <functional>

/*==============================================================================
  Cabinet Simulator

  juce::dsp::Convolution, partitioned, built with Latency { 512 } and the shared
  ConvolutionMessageQueue owned by the engine.

  Thread model, because this is the one effect that has to swap a large asset
  while audio is running. Verified against modules/juce_dsp/frequency/
  juce_Convolution.cpp in JUCE 8.0.15:

    * loadImpulseResponse() does not read the file. It stores one pending
      FixedSizeCommand and ConvolutionEngineQueue::postPendingCommand() pushes it
      to BackgroundMessageQueue, whose own thread decimates, trims and FFTs the
      response into a new engine.
    * The audio thread only ever calls getEngine() to pick up a finished engine
      and crossfades into it (CrossoverMixer). So an IR swap is click free.
    * Consequence: the request must come from the message thread (the card
      buttons), because building the command captures an AudioBuffer by value.

  The three built in responses are produced by Resources/make_irs.mjs. They are
  original synthetic responses - a speaker band shape plus a decaying body - not
  copies of any manufacturer's measurements. They demonstrate and audition the
  effect; a real recording of a real cabinet will sound like a real cabinet, so
  the Load IR button exists for that and the card always names the response that
  is currently active. With no response loaded the card warns and shows the
  request to load an IR file; it does not silently pass the signal through
  pretending to be a cabinet.
==============================================================================*/
namespace pedal
{

class CabinetEffect : public PedalEffect
{
public:
    enum Params { bass, treble, mix, numParams };

    enum Actions { loadExternal = 1, firstBuiltin = 101 };

    explicit CabinetEffect (const EffectContext& context)
        : PedalEffect (cabinet, "Cabinet Simulator",
            {
                { "Bass",   -12.0f, 12.0f,  0.0f, "dB" },
                { "Treble", -12.0f, 12.0f,  0.0f, "dB" },
                { "Mix",      0.0f,  1.0f,  1.0f },
            }),
          queue (context.convolutionQueue)
    {
    }

    ~CabinetEffect() override
    {
        // JUCE 8's FileChooser has no way to dismiss a pending async dialog, and
        // its callback captures this. Clearing the flag tells the callback the
        // object is gone: the shared_ptr keeps the flag itself alive, so reading
        // it after destruction is not a dangling access.
        if (chooserGuard != nullptr)
            chooserGuard->alive.store (false, std::memory_order_release);
    }

    //==============================================================================
    void doPrepare (double, int) override
    {
        if (queue == nullptr)
            return;

        // Built once and reused across device restarts. Re-creating it would
        // allocate on the (stopped) audio thread every time a device changes and
        // would throw away a fully loaded engine.
        if (convolution == nullptr)
            convolution = std::make_unique<juce::dsp::Convolution> (
                juce::dsp::Convolution::Latency { 512 }, *queue);

        const juce::dsp::ProcessSpec spec
        {
            sampleRate,
            static_cast<juce::uint32> (blockSize),
            static_cast<juce::uint32> (chainChannels)
        };

        convolution->prepare (spec);

        // Convolution::prepare() drains the background queue, so anything that
        // was requested before this restart is gone. Re-assert the selection.
        selected = selectedResponse.load (std::memory_order_relaxed);
        loadSelected();

        // The wet path is delayed by the partitioned engine's 512 samples. Mix
        // used to be a direct crossFade against the un-delayed dry, which combs
        // the sound at every setting except 100 % and reads as "the cab distorts".
        prepareLatencyCompensatedDry (reportedLatency);
    }

    void resetState() noexcept override
    {
        for (auto& filter : bassFilter)  filter.reset();
        for (auto& filter : trebleFilter) filter.reset();
    }

    int getLatencySamples() const noexcept override
    {
        // Re-read at most every 64 blocks; it only changes when a new response
        // finishes loading on the background thread.
        return irSize.load (std::memory_order_relaxed) > 0 ? reportedLatency : 0;
    }

    //==============================================================================
    void process (juce::AudioBuffer<float>& audio) override
    {
        if (convolution == nullptr || liveIRSize <= 0)
            return;

        if ((++latencyCounter & 63) == 0)
        {
            // The engine pointer is only installed once the background thread has
            // finished decimating, trimming and FFTing the response, so reading the
            // size here is ground truth rather than a hope. Until it turns non-zero
            // the card reports that no response is loaded.
            liveIRSize    = convolution->getCurrentIRSize();
            reportedLatency = convolution->getLatency();
            irSize.store (liveIRSize, std::memory_order_relaxed);
        }

        const auto numSamples = audio.getNumSamples();

        auto& wet = wetBufferForBlock();

        for (int c = 0; c < wet.getNumChannels(); ++c)
            wet.copyFrom (c, 0, audio, c, 0, numSamples);

        // ProcessContextReplacing takes a non-const AudioBlock reference, so the
        // block has to be a named lvalue: binding a temporary would not compile.
        // The block has to be numSamples long, not the scratch buffer's capacity:
        // the scratch is sized for the largest block ever prepared, and convolving
        // its stale tail would add the IR to audio that is not in this callback.
        juce::dsp::AudioBlock<float> block { wet.getArrayOfWritePointers(),
                                             static_cast<std::size_t> (wet.getNumChannels()),
                                             static_cast<std::size_t> (numSamples) };
        juce::dsp::ProcessContextReplacing<float> context { block };
        convolution->process (context);

        applyTone (wet, numSamples);
        mixWithDelayedDry (audio, wet, getParameter (mix), reportedLatency);
    }

    //==============================================================================
    /** Used by the settings loader: called on the message thread, before or after
        prepare(). A missing or unreadable file leaves the current response alone
        and sets the warning text instead of silently pretending to load. */
    bool restoreExternalIR (const juce::File& file)
    {
        if (! validateExternalFile (file))
            return false;

        publishExternalFile (file);
        return true;
    }

    juce::String getExternalIRPath() const
    {
        return selected >= numBuiltInIRs && externalFile.existsAsFile()
                   ? externalFile.getFullPathName()
                   : juce::String();
    }

    /** Index of the active built-in response, or -1 when an external file is
        active. The settings file stores both this and the external path. */
    int getBuiltinSelection() const
    {
        const auto index = selectedResponse.load (std::memory_order_relaxed);
        return index >= 0 && index < numBuiltInIRs ? index : -1;
    }

    void selectBuiltin (int index)
    {
        if (index < 0 || index >= numBuiltInIRs)
            return;

        externalFile = {};
        externalError.clear();
        selectedResponse.store (index, std::memory_order_relaxed);
        selected = index;
        loadSelected();
    }

    /** Fired after the user changes the response, so the owner can persist the
        non-numeric selection. Not fired while settings are being restored. */
    std::function<void()> onSettingsChanged;

    std::vector<EffectAction> actions() const override
    {
        return { { firstBuiltin + 0, "1x12 V30" },
                 { firstBuiltin + 1, "2x12 Open" },
                 { firstBuiltin + 2, "4x12 High Gain" },
                 { loadExternal,     "Load IR..." } };
    }

    void handleAction (int id) override
    {
        if (id >= firstBuiltin && id < firstBuiltin + numBuiltInIRs)
        {
            selectBuiltin (id - firstBuiltin);

            if (onSettingsChanged != nullptr)
                onSettingsChanged();

            return;
        }

        if (id == loadExternal)
            chooseExternalFile();
    }

    juce::String getStatusText() const override
    {
        if (externalError.isNotEmpty())
            return trf ("IR error: %1", tr (externalError));

        if (irSize.load (std::memory_order_relaxed) <= 0)
            return tr ("No IR loaded - use Load IR to pick a file");

        const auto index = selectedResponse.load (std::memory_order_relaxed);

        if (index >= 0 && index < numBuiltInIRs)
            return trf ("IR: %1", tr (builtinName (index)));

        return trf ("IR: %1", externalFile.getFileName());
    }

    juce::String getWarningText() const override
    {
        if (externalError.isNotEmpty())
            return tr ("The previous response is still active. Load a WAV, AIFF, FLAC, OGG or MP3 file.");

        if (irSize.load (std::memory_order_relaxed) <= 0)
            return tr ("The cabinet stays inactive until an impulse response is loaded. "
                       "WAV, AIFF, FLAC, OGG and MP3 files are supported.");

        return selectedResponse.load (std::memory_order_relaxed) < numBuiltInIRs
            ? tr ("Pick a built-in cabinet, load your own IR, then set Mix to taste (100 % is fully cabinet).")
            : tr ("External IR: resampled, trimmed and normalised to the current rate. "
                  "WAV, AIFF, FLAC, OGG and MP3 are supported.");
    }

private:
    //==============================================================================
    static const char* builtinName (int index) noexcept
    {
        static const char* const names[numBuiltInIRs] =
        {
            "1x12 V30 closed back (synthetic)",
            "2x12 open back (synthetic)",
            "4x12 high gain (synthetic)"
        };

        return index >= 0 && index < numBuiltInIRs ? names[static_cast<std::size_t> (index)]
                                                   : "external file";
    }

    struct BuiltinIR
    {
        const char* data;
        int size;
    };

    /*  The symbol names come from juce_add_binary_data: makeBinaryDataIdentifierName
        turns "ir_1x12_v30_closed.wav" into "ir_1x12_v30_closed_wav", and the matching
        length is that name with the suffix "Size" - not "_length". Verified in
        extras/Build/juce_build_tools/utils/juce_BinaryResourceFile.cpp, which emits
        "const int <name>Size = <bytes>;". */
    static BuiltinIR builtin (int index)
    {
        switch (index)
        {
            case 0:  return { PedalBoardIR::ir_1x12_v30_closed_wav, PedalBoardIR::ir_1x12_v30_closed_wavSize };
            case 1:  return { PedalBoardIR::ir_2x12_open_back_wav,  PedalBoardIR::ir_2x12_open_back_wavSize };
            default: return { PedalBoardIR::ir_4x12_high_gain_wav,  PedalBoardIR::ir_4x12_high_gain_wavSize };
        }
    }

    void loadSelected()
    {
        if (convolution == nullptr || queue == nullptr)
        {
            liveIRSize = 0;
            irSize.store (0, std::memory_order_relaxed);
            return;
        }

        const auto index = selectedResponse.load (std::memory_order_relaxed);

        if (index >= 0 && index < numBuiltInIRs)
        {
            const auto ir = builtin (index);
            convolution->loadImpulseResponse (ir.data,
                                              static_cast<size_t> (ir.size),
                                              juce::dsp::Convolution::Stereo::yes,
                                              juce::dsp::Convolution::Trim::yes,
                                              0,
                                              juce::dsp::Convolution::Normalise::yes);
            forceLatencyRefresh();
        }
        else
        {
            // An external response: read it from the published slot. Nothing is
            // opened here - Convolution only queues the request and its own
            // background thread does the reading.
            const auto slot = fileSlot.load (std::memory_order_acquire);

            if (slot >= 0)
            {
                convolution->loadImpulseResponse (fileSlots[static_cast<std::size_t> (slot)],
                                                  juce::dsp::Convolution::Stereo::yes,
                                                  juce::dsp::Convolution::Trim::yes,
                                                  0,
                                                  juce::dsp::Convolution::Normalise::yes);
                forceLatencyRefresh();
            }
            else
            {
                liveIRSize = 0;
                irSize.store (0, std::memory_order_relaxed);
            }
        }
    }

    void forceLatencyRefresh() noexcept
    {
        latencyCounter = 63;      // read the true size and latency on the next block
    }

    bool validateExternalFile (const juce::File& file)
    {
        externalError.clear();

        if (! file.existsAsFile())
        {
            externalError = "file not found";
            return false;
        }

        /*  Validate here, on the message thread, instead of letting Convolution
            queue a request that decodes to nothing and leaves the card claiming an
            IR is loaded. The manager has the same decoders Convolution uses (MP3
            is compiled in through JUCE_USE_MP3AUDIOFORMAT), so this is the honest
            test of what the app can actually read. */
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            externalError = "unsupported or unreadable audio file";
            return false;
        }

        if (reader->sampleRate <= 0.0 || reader->numChannels <= 0)
        {
            externalError = "the file has no usable audio";
            return false;
        }

        /*  Convolution reads the whole file into RAM and partitions all of it.
            A dropped song is a valid audio file but not an impulse response; ten
            seconds is already far longer than any cabinet or room IR. */
        if (reader->lengthInSamples > reader->sampleRate * 10.0)
        {
            externalError = "impulse responses longer than 10 seconds are not supported";
            return false;
        }

        return true;
    }

    void publishExternalFile (const juce::File& file)
    {
        // Publish without a lock: write the shadow slot first, then flip the
        // index. doPrepare() is the only audio-thread reader.
        const auto previous = fileSlot.load (std::memory_order_relaxed);
        const auto next = previous < 0 ? 0 : 1 - previous;

        fileSlots[static_cast<std::size_t> (next)] = file;
        externalFile = file;
        externalError.clear();
        fileSlot.store (next, std::memory_order_release);

        selectedResponse.store (numBuiltInIRs, std::memory_order_relaxed);
        selected = numBuiltInIRs;
        loadSelected();
    }

    void chooseExternalFile()
    {
        /*  Kept alive as a member: launchAsync() returns void, so the chooser has
            to outlive this call for the callback to fire.

            It opens on the IRs folder the build drops next to the executable, which
            is where a user who wants to replace a response will put the file, and
            never on whatever the working directory happens to be. An empty File makes
            the system choose instead when that folder does not exist. */
        if (chooser == nullptr)
        {
            auto startDirectory = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                     .getParentDirectory().getChildFile ("IRs");

            if (! startDirectory.isDirectory())
                startDirectory = juce::File();

            chooser = std::make_unique<juce::FileChooser> (tr ("Load impulse response"),
                                                           startDirectory,
                                                           "*.wav;*.wave;*.aif;*.aiff;*.aifc;*.flac;*.ogg;*.oga;*.mp3");
        }

        chooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this, guard = chooserGuard] (const juce::FileChooser& chosen)
                              {
                                  if (guard == nullptr
                                      || ! guard->alive.load (std::memory_order_acquire))
                                      return;

                                  const auto file = chosen.getResult();

                                  // Cancelled, or a folder, or a file that vanished:
                                  // keep the current response instead of switching
                                  // the user's sound by surprise.
                                  if (! file.existsAsFile())
                                      return;

                                  if (! validateExternalFile (file))
                                  {
                                      // The message is shown on the card; the audio
                                      // keeps the previous IR.
                                      selected = selectedResponse.load (std::memory_order_relaxed);
                                      return;
                                  }

                                  publishExternalFile (file);

                                  if (onSettingsChanged != nullptr)
                                      onSettingsChanged();
                              });
    }

    void applyTone (juce::AudioBuffer<float>& wet, int numSamples) noexcept
    {
        const auto bassDb   = getParameter (bass);
        const auto trebleDb = getParameter (treble);

        if (bassDb != cachedBass || trebleDb != cachedTreble || sampleRate != cachedRate)
        {
            for (auto& filter : bassFilter)   filter.makeLowShelf  (sampleRate, 180.0,  bassDb);
            for (auto& filter : trebleFilter) filter.makeHighShelf (sampleRate, 2800.0, trebleDb);

            cachedBass = bassDb;
            cachedTreble = trebleDb;
            cachedRate = static_cast<float> (sampleRate);
        }

        const auto numChannels  = juce::jmin (wet.getNumChannels(), chainChannels);

        for (int c = 0; c < numChannels; ++c)
        {
            auto* data = wet.getWritePointer (c);
            auto& low  = bassFilter[static_cast<std::size_t> (c)];
            auto& high = trebleFilter[static_cast<std::size_t> (c)];

            for (int i = 0; i < numSamples; ++i)
                data[i] = high.process (low.process (data[i]));
        }
    }

    //==============================================================================
    juce::dsp::ConvolutionMessageQueue* queue = nullptr;
    std::unique_ptr<juce::dsp::Convolution> convolution;
    /** Set false in the destructor; held by shared_ptr so the pending file
        dialog can read it safely even if this effect is already gone. */
    struct ChooserGuard
    {
        std::atomic<bool> alive { true };
    };

    std::shared_ptr<ChooserGuard> chooserGuard = std::make_shared<ChooserGuard>();
    std::unique_ptr<juce::FileChooser> chooser;

    std::array<Biquad, chainChannels> bassFilter;
    std::array<Biquad, chainChannels> trebleFilter;
    float cachedBass = 0.0f, cachedTreble = 0.0f, cachedRate = 0.0f;

    // The open-back 2x12 is the default: less boxy than the closed 1x12, so an
    // acoustic-electric guitar plugged straight in does not sound over-filtered.
    std::atomic<int>  selectedResponse { 1 };
    std::atomic<int>  irSize { 0 };          // audio -> message: 0 until an engine exists

    int selected = 1;                        // message-thread copy of selectedResponse
    int liveIRSize = 0;                      // audio-thread copy of irSize
    int reportedLatency = 512, latencyCounter = 0;

    // Externally chosen impulse response, published slot by slot so the message
    // thread never races the (stopped) audio thread that reads it in doPrepare().
    std::array<juce::File, 2> fileSlots;
    std::atomic<int> fileSlot { -1 };
    juce::File externalFile;                 // message thread only; read by getStatusText()
    juce::String externalError;              // message thread only; empty when fine
};

} // namespace pedal
