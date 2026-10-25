#pragma once

#include <array>
#include <atomic>

#include <juce_audio_devices/juce_audio_devices.h>


#include "Common/PedalLockFree.h"
#include "Effects/EffectFactory.h"

#include <vector>
#include <memory>

namespace pedal
{

class VBCableRouter;

/*==============================================================================
  Thread model, stated exactly

    inputManager  owns a device whose thread calls InputRingWriter. That callback
                  copies the guitar into inputRing and returns. It touches nothing
                  else, so a stalled output device cannot stop the input.
    outputManager owns the thread that runs the effect chain. The chain is clocked
                  by the physical monitor device, which is the one device that has
                  to be reliable.
    the router    owns a third device (VB-CABLE "CABLE Input") with its own thread,
                  which drains virtualRing.

  The three threads are joined only by two AudioRingBuffer instances and a set of
  atomics. No thread ever waits for another: producers drop the oldest samples when
  a queue is full and consumers emit silence when it is empty, both of which are
  counted and shown in the status bar instead of being hidden behind a lock.
==============================================================================*/

/** Input side callback: copies the incoming device channels into the ring and does
    nothing else. Runs on the input device's own thread. */
class InputRingWriter : public juce::AudioIODeviceCallback
{
public:
    InputRingWriter (AudioRingBuffer& destination, std::atomic<double>& rate,
                     const std::atomic<bool>* consumerRunning) noexcept
        : ring (destination), sampleRate (rate), running (consumerRunning) {}

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* /*outputChannelData*/,
                                           int /*numOutputChannels*/,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        if (numInputChannels <= 0 || numSamples <= 0)
            return;

        /*  With the monitor path stopped nobody drains the ring, so writing anyway
            fills it and then overflows it one block at a time. The status bar showed
            that as a drop counter climbing at about 130 a second - which reads like
            a real-time budget problem and is actually a device that stopped. The
            producer stops too, and the ring is flushed on the way back. */
        if (running != nullptr && ! running->load (std::memory_order_relaxed))
            return;

        // A guitar is one mono channel: duplicate it so the stereo chain never has
        // a silently zero-filled right side, which would make every stereo effect
        // sound like it was wired to only one ear.
        const float* channels[chainChannels] = { inputChannelData[0],
                                                 numInputChannels > 1 ? inputChannelData[1]
                                                                      : inputChannelData[0] };

        ring.write (channels, chainChannels, numSamples);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        sampleRate.store (device != nullptr ? device->getCurrentSampleRate() : 0.0,
                          std::memory_order_relaxed);
    }

    void audioDeviceStopped() override
    {
        sampleRate.store (0.0, std::memory_order_relaxed);
    }

private:
    AudioRingBuffer& ring;
    std::atomic<double>& sampleRate;
    const std::atomic<bool>* running;
};

/** Reads CC1 (mod wheel, the usual expression fallback) and CC11 (expression) and
    publishes 0..1 into an atomic. JUCE delivers MIDI on its own thread, so this
    callback stays a controller test and one relaxed store. */
class ExpressionMidiReader : public juce::MidiInputCallback
{
public:
    explicit ExpressionMidiReader (std::atomic<float>& position) noexcept : target (position) {}

    // JUCE 8's MidiInputCallback pure virtual is handleIncomingMidiMessage with
    // the source device as the first argument; the older single-argument
    // midiInputCallback spelling does not override anything.
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message) override
    {
        if (! message.isController())
            return;

        const auto number = message.getControllerNumber();

        if (number == 11 || number == 1)
            target.store (static_cast<float> (message.getControllerValue()) / 127.0f,
                          std::memory_order_relaxed);
    }

private:
    std::atomic<float>& target;
};

/** The engine. Owns both device managers, the effect pool and the three signal
    paths. The UI never touches a device directly. */
class AudioEngine : private juce::AudioIODeviceCallback
{
public:
    /** Published to the audio thread whenever the chain changes. Plain data, so
        the seqlock copy is a 16 pointer memcpy. */
    struct ChainSnapshot
    {
        std::array<PedalEffect*, maxChainSlots> slots {};
        int numSlots = 0;
    };

    /** The dry compensation line is capped, because a long granular window plus
        four oversampled effects plus a convolution can theoretically exceed one
        buffer of delay. Saturation is reported instead of being hidden. */
    static constexpr int maxLatencyCompensation = 16384;

    AudioEngine();
    ~AudioEngine() override;

    JUCE_DECLARE_NON_COPYABLE (AudioEngine)

    //==============================================================================
    /** Opens both device managers. An empty preferred name means "ask the OS".
        Returns an error string, empty on success. Never assumes a default device
        works: the caller shows the chooser when this reports a problem. */
    juce::String initialise (const juce::String& preferredInputType,
                             const juce::String& preferredInputDevice,
                             const juce::String& preferredOutputType,
                             const juce::String& preferredOutputDevice,
                             double preferredSampleRate,
                             int preferredBufferSize);
    void shutdown();

    bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

    //==============================================================================
    //  Device enumeration - message thread only
    //==============================================================================
    /** Device types that expose at least one input (for the input selector) or at
        least one output (for the output and virtual selectors).

        Members rather than statics because JUCE only builds the type list through
        a manager; it is read off inputManager, which need not be running. Both
        managers see the same type list. */
    juce::StringArray listDeviceTypes (bool wantInputs, bool wantOutputs);
    juce::StringArray listDevicesOfType (const juce::String& typeName, bool wantInputs);

    /** Rescans every device type - the only supported way to notice that
        VB-CABLE was installed or removed since launch. */
    void rescanDevices();

    juce::String setInputDevice  (const juce::String& typeName, const juce::String& deviceName);
    juce::String setOutputDevice (const juce::String& typeName, const juce::String& deviceName);

    juce::String getInputDeviceName() const   { return inputDeviceName; }
    juce::String getOutputDeviceName() const  { return outputDeviceName; }
    juce::String getInputDeviceType() const   { return inputTypeName; }
    juce::String getOutputDeviceType() const  { return outputTypeName; }

    juce::Array<double> getAvailableSampleRates() const;
    juce::Array<int>    getAvailableBufferSizes() const;

    /** Applied to both managers, because one clock mismatch would turn the
        monitoring path into a pitch shifter. Returns an error if either refused. */
    juce::String setSampleRate (double newRate);
    juce::String setBufferSize (int newSizeInSamples);

    double getSampleRate() const noexcept { return outputSampleRate.load (std::memory_order_relaxed); }
    double getInputSampleRate() const noexcept { return inputSampleRate.load (std::memory_order_relaxed); }
    int    getBufferSizeSamples() const noexcept { return outputBlockSize.load (std::memory_order_relaxed); }

    //==============================================================================
    //  Latency, CPU and xruns - all read by the status bar
    //==============================================================================
    double getInputLatencyMs() const noexcept;      ///< input device buffer + driver report
    double getOutputLatencyMs() const noexcept;     ///< physical monitor path
    double getChainLatencyMs() const noexcept;      ///< effect group delay, compensated
    double getVirtualLatencyMs() const noexcept;    ///< extra delay of the CABLE path
    float  getCpuUsage() const noexcept;
    int    getInputDrops() const noexcept  { return inputRing.getOverflowCount() + inputRing.getUnderflowCount(); }
    /** The two halves of getInputDrops: overflow means the producer ran ahead
        (the consumer stalled), underflow means the consumer starved. They point
        at different causes, so the status bar shows both. */
    int    getInputOverflows() const noexcept  { return inputRing.getOverflowCount(); }
    int    getInputUnderflows() const noexcept { return inputRing.getUnderflowCount(); }
    int    getVirtualDrops() const noexcept;
    /** Same split for the cable ring: overflow means the chain outran the cable,
        underflow means the cable outran the chain. */
    int    getVirtualOverflows() const noexcept  { return virtualRing.getOverflowCount(); }
    int    getVirtualUnderflows() const noexcept { return virtualRing.getUnderflowCount(); }
    bool   getLatencyCompensationSaturated() const noexcept
    {
        return latencyCompensationSaturated.load (std::memory_order_relaxed);
    }

    //==============================================================================
    //  Chain - message thread
    //==============================================================================
    int getNumSlots() const noexcept { return static_cast<int> (activeChain.size()); }
    PedalEffect* getSlot (int index) const noexcept
    {
        return index >= 0 && index < static_cast<int> (activeChain.size())
             ? activeChain[static_cast<std::size_t> (index)]
             : nullptr;
    }
    EffectTypeID getSlotType (int index) const noexcept;

    bool addEffect (EffectTypeID type, int insertIndex = -1);
    bool removeSlot (int index);
    bool moveSlot (int from, int to);
    void publishChain();

    /** Two effects of the same type cannot both be in the chain: the pool holds
        one instance of each, so its parameters would be shared. */
    bool isTypeInUse (EffectTypeID type) const noexcept;

    //==============================================================================
    //  Global controls
    //==============================================================================
    void setMonitorEnabled (bool on) noexcept { monitorEnabled.store (on ? 1.0f : 0.0f, std::memory_order_relaxed); }
    bool isMonitorEnabled() const noexcept { return monitorEnabled.load (std::memory_order_relaxed) > 0.5f; }

    void  setGlobalWet (float w) noexcept { globalWet.store (clamp01 (w), std::memory_order_relaxed); }
    float getGlobalWet() const noexcept   { return globalWet.load (std::memory_order_relaxed); }

    void  setInputTrimDb (float db) noexcept { inputTrimDb.store (clampf (db, -24.0f, 24.0f), std::memory_order_relaxed); }
    float getInputTrimDb() const noexcept    { return inputTrimDb.load (std::memory_order_relaxed); }

    //==============================================================================
    //  Meters - the audio thread stores the block peak, the UI does the smoothing
    //==============================================================================
    float getPhysicalPeak() const noexcept { return physicalPeak.load (std::memory_order_relaxed); }
    float getVirtualPeak()  const noexcept { return virtualPeak.load (std::memory_order_relaxed); }

    /** Post-trim chain input, held with a slow decay. Unlike the physical meter
        this is updated with monitoring off, so the UI can show whether a guitar
        is actually arriving. */
    float getInputPeak() const noexcept { return inputPeak.load (std::memory_order_relaxed); }

    //==============================================================================
    //  MIDI expression pedal
    //==============================================================================
    static juce::StringArray getMidiInputNames();
    juce::String openExpressionMidiInput (const juce::String& name);
    void closeExpressionMidiInput();
    bool hasExpressionMidiInput() const noexcept { return midiInput != nullptr; }
    /** Empty when no controller is open, so the settings file can round-trip the
        choice the user made in the device panel. */
    juce::String getExpressionMidiInputName() const noexcept
    {
        return midiInput != nullptr ? midiInput->getName() : juce::String();
    }
    float getExpressionPosition() const noexcept { return expressionPosition.load (std::memory_order_relaxed); }
    void setExpressionFromKnob (float position) noexcept { expressionPosition.store (clamp01 (position), std::memory_order_relaxed); }

    //==============================================================================
    VBCableRouter& getVirtualRouter() const noexcept { return *router; }
    EffectFactory& getEffects() noexcept { return *factory; }
    AudioRingBuffer& getVirtualRing() noexcept { return virtualRing; }

    /** The only way the interface should switch the recording path. It has to go
        through the engine rather than straight to the router because the producer
        of the ring is the audio thread: turning the path back on has to be heard
        there, and the stale backlog has to be abandoned by the thread that owns the
        write end. */
    void setVirtualOutputEnabled (bool shouldStream);

    /** The 0..1 controller value the wah reads; nullptr when the app has no MIDI. */
    std::atomic<float>* getExpressionAtomic() noexcept { return &expressionPosition; }

    //==============================================================================
    //  Persistence. Deliberately a plain text file next to the executable rather
    //  than %APPDATA%, because every byte of this project has to stay on one drive.
    //==============================================================================
    struct Settings
    {
        /** The file records no language until the user has answered the question,
            which is what distinguishes "English" from "English, chosen". */
        static constexpr int noLanguage = -1;

        juce::String inputType, inputDevice, outputType, outputDevice, midiDevice;
        /** 0 = a file written before the parameter/default overhauls. Version 2
            added the pitch/formant layout and the corrected pedal defaults. */
        int version = 0;
        double sampleRate = 0.0;
        int bufferSize = 0;
        bool monitor = true, virtualEnabled = true;
        int language = noLanguage;
        bool devicesChosen = false;
        /** True when the file contained a chain key, even an empty one: "I removed
            every pedal" must not read back as "no preference, use the default". */
        bool hasChain = false;
        float globalWet = 1.0f, inputTrimDb = 0.0f;
        juce::Array<EffectTypeID> chain;
        juce::String parameterStrings[numEffectTypes];
        /** External cabinet response, kept outside params.* because it is a path,
            not a number. Empty when a built-in IR is selected. */
        juce::String cabinetIR;

        /** VST3 effect loaded by the Plugin Host card. The plugin's own state is
            stored beside the settings file; this is only which file to reopen. */
        juce::String pluginPath;
    };

    /** False when there is no file, i.e. on the very first launch, which is when
        the device chooser has to appear. */
    static bool loadFromFile (Settings& result);
    void saveSettings();
    void applySettings (const Settings& settings);

    /** The two settings the engine carries only because it owns the file: which
        interface language is showing, and whether the user has ever been through
        the device chooser. The second one matters because saveSettings() also runs
        when the window closes, so "the file exists" is not a first-run test -
        a user who dismissed the chooser without applying anything would never see
        it again. */
    void setStoredLanguage (int languageId) noexcept { storedLanguage = languageId; }
    void setDevicesChosen() noexcept { devicesChosen = true; }
    void setDevicesChosenFrom (bool chosen) noexcept { devicesChosen = chosen; }

    /** The chain the settings file asked for, used by the card list after a
        restart. Empty when the default chain was used instead. */
    juce::Array<EffectTypeID> getStoredChain() const;

    static juce::File getSettingsDirectory();
    static juce::File getSettingsFile();
    juce::String getSettingsSummary() const;

    /** Opens the devices and the chain implied by a set of settings, and reports
        anything that failed so the UI can show it instead of guessing. */
    juce::String applyAndStart (const Settings& settings);

private:
    //==============================================================================
    //  Audio thread: the physical output device clocks everything
    //==============================================================================
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    void fillChainBuffer (juce::AudioBuffer<float>& destination, int numSamples) noexcept;
    void runChain (juce::AudioBuffer<float>& audio, const ChainSnapshot& snapshot) noexcept;
    void pushToVirtualRing (const juce::AudioBuffer<float>& out) noexcept;
    void updateDryCompensation (const juce::AudioBuffer<float>& input,
                                juce::AudioBuffer<float>& destination,
                                int totalLatency,
                                int numSamples) noexcept;

    double sampleRateOfChain() const noexcept;

    EffectContext makeContext();

    juce::AudioDeviceManager inputManager, outputManager;

    AudioRingBuffer inputRing, virtualRing;
    InputRingWriter inputCallback { inputRing, inputSampleRate, &running };

    std::unique_ptr<EffectFactory> factory;
    std::unique_ptr<VBCableRouter> router;

    juce::dsp::ConvolutionMessageQueue convolutionQueue;

    SeqSnapshot<ChainSnapshot> chainSnapshot;
    std::vector<PedalEffect*> activeChain;           // message thread only

    // Working buffers, all preallocated before any device starts.
    juce::AudioBuffer<float> chainBuffer, dryBuffer, mixedBuffer;
    juce::AudioBuffer<float> resampleScratch;
    std::array<juce::LagrangeInterpolator, chainChannels> resamplers;

    // Dry path group delay compensation.
    std::vector<float> dryDelayStorage;
    int dryDelayMask = 0, dryDelayWrite = 0;
    float dryDelayCurrent = 0.0f, dryDelayTarget = 0.0f, dryDelayStep = 0.0f;

    // Fractional remainder of the input/output sample rate ratio, so the two clocks
    // cannot drift apart block by block.
    double resampleCarry = 0.0;

    juce::Array<EffectTypeID> storedChain;

    std::atomic<double> inputSampleRate { 0.0 }, outputSampleRate { 44100.0 };
    std::atomic<int>    outputBlockSize { 512 };

    /** The file's clock choice. When no output device opens, the live values are
        the manager defaults; saving those would silently rewrite a 441-sample
        request as 512 the next time the interface is unplugged. */
    double requestedSampleRate = 0.0;
    int    requestedBufferSize = 0;
    std::atomic<float>  monitorEnabled { 1.0f }, globalWet { 1.0f }, inputTrimDb { 0.0f };
    std::atomic<float>  expressionPosition { 0.5f };
    std::atomic<bool>   expressionMidiActive { false };
    std::atomic<float>  physicalPeak { 0.0f }, virtualPeak { 0.0f }, inputPeak { 0.0f };
    std::atomic<double> chainLatencyMs { 0.0 };
    std::atomic<bool>   latencyCompensationSaturated { false };
    std::atomic<bool>   running { false };

    /** Set by the message thread when it swaps the input device, and consumed by
        the audio thread, which is the only thread allowed to move the input ring's
        read head. Resetting the ring from here would race with the input callback
        that is already running against the new device. */
    std::atomic<bool>   flushInputRequested { false };

    /** Set when the recording path is switched back on and cleared by the audio
        thread as it consumes it; see pushToVirtualRing(). */
    std::atomic<bool>   virtualFlushRequested { false };

    juce::String inputDeviceName, outputDeviceName, inputTypeName, outputTypeName;
    bool callbacksRegistered = false;

    /** Written straight through to the settings file; see setStoredLanguage. */
    int storedLanguage = Settings::noLanguage;
    bool devicesChosen = false;

    /** Set by applyAndStart() before initialise() runs: an empty saved chain is a
        choice, not a request for the default pedals. */
    bool startFromStoredChain = false;

    std::unique_ptr<juce::MidiInput> midiInput;
    std::unique_ptr<ExpressionMidiReader> midiReader;
};

} // namespace pedal
