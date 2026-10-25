#include "AudioEngine.h"

#include <algorithm>
#include <cmath>

#include "Common/PedalLanguage.h"
#include "Effects/Pitch/PitchShifterEffect.h"
#include "Effects/TimeSpace/CabinetEffect.h"
#include "VirtualDevice/VBCableRouter.h"

#include <memory>

namespace pedal
{

//==============================================================================
AudioEngine::AudioEngine()
{
    inputRing.reset (chainChannels, static_cast<int> (ringCapacityFor (inputRingBlocks)));
    virtualRing.reset (chainChannels, static_cast<int> (ringCapacityFor (virtualRingBlocks)));

    // Every working buffer is sized for the worst case once, here, so that no
    // device restart - which happens on the audio thread - has to touch the heap
    // for the engine's own storage. Only the effects' internals are re-sized in
    // audioDeviceAboutToStart(), and that runs while the device is stopped.
    chainBuffer.setSize (chainChannels, maxBlockSize, false, true, true);
    dryBuffer.setSize  (chainChannels, maxBlockSize, false, true, true);
    mixedBuffer.setSize (chainChannels, maxBlockSize, false, true, true);
    resampleScratch.setSize (chainChannels, maxInputReadPerBlock, false, true, true);

    dryDelayStorage.assign (static_cast<std::size_t> (chainChannels) * maxLatencyCompensation, 0.0f);
    dryDelayMask = maxLatencyCompensation - 1;

    factory = std::make_unique<EffectFactory> (makeContext());

    // Constructed in the body, not the init list: the router holds references to
    // two atomics that are declared after it, and by now all of them are built.
    router = std::make_unique<VBCableRouter> (virtualRing, outputSampleRate, outputBlockSize);
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

EffectContext AudioEngine::makeContext()
{
    return { &expressionPosition, &expressionMidiActive, &convolutionQueue };
}

//==============================================================================
juce::String AudioEngine::initialise (const juce::String& preferredInputType,
                                      const juce::String& preferredInputDevice,
                                      const juce::String& preferredOutputType,
                                      const juce::String& preferredOutputDevice,
                                      double preferredSampleRate,
                                      int preferredBufferSize)
{
    /*  Keep the names the file asked for before any open is attempted. If the
        interface is unplugged at launch, setInputDevice() fails and the members
        would otherwise stay empty; the close-time save would then erase the
        user's device choice, so reconnecting the interface next time would find
        nothing to restore. The failure is still reported, and the chooser still
        opens - it just cannot destroy the remembered names. */
    inputTypeName   = preferredInputType;
    inputDeviceName = preferredInputDevice;
    outputTypeName   = preferredOutputType;
    outputDeviceName = preferredOutputDevice;
    requestedSampleRate = preferredSampleRate;
    requestedBufferSize = preferredBufferSize;

    // Two managers, two threads, deliberately separate failures: a broken
    // microphone device must not stop the monitoring path and vice versa.
    //
    // The explicit isNotEmpty() is required, not stylistic: juce::String declares
    // operator bool() as a private member precisely so that `if (someString)` will
    // not compile, so an error string has to be tested by name.
    if (auto error = inputManager.initialise (chainChannels, 0, nullptr, false); error.isNotEmpty())
        return trf ("Input audio system: %1", error);

    if (auto error = outputManager.initialise (0, chainChannels, nullptr, false); error.isNotEmpty())
        return trf ("Output audio system: %1", error);

    /*  Both calls above have already opened the system default device. When the
        file asked for nothing, that device is what is really running, so its type
        and name are adopted here: without this the selectors show blank above a
        live stream and the panel's summary reads "Input / Output" with nothing in
        either half.

        The name has to come from the open device, not from getAudioDeviceSetup():
        AudioDeviceManager clears both names in its stored setup whenever it opened
        a default rather than a chosen device (juce_AudioDeviceManager.cpp:747).
        AudioIODevice::getTypeName is the same string AudioIODeviceType::getTypeName
        contributes to listDeviceTypes, so the combo can match it. */
    if (preferredInputDevice.isEmpty())
        if (auto* device = inputManager.getCurrentAudioDevice())
        {
            inputTypeName   = device->getTypeName();
            inputDeviceName = device->getName();
        }

    if (preferredOutputDevice.isEmpty())
        if (auto* device = outputManager.getCurrentAudioDevice())
        {
            outputTypeName   = device->getTypeName();
            outputDeviceName = device->getName();
        }

    // The callbacks are registered only once the devices the user actually asked for
    // are open; otherwise the manager would start and stop a default device first and
    // the chosen one second, which is audible as a dropout on the way in.
    router->scanForCable();

    juce::String problems;

    if (preferredInputDevice.isNotEmpty())
    {
        if (auto error = setInputDevice (preferredInputType, preferredInputDevice); error.isNotEmpty())
            problems << trf ("Input: %1", error) << "\n";
    }

    if (preferredOutputDevice.isNotEmpty())
    {
        if (auto error = setOutputDevice (preferredOutputType, preferredOutputDevice); error.isNotEmpty())
            problems << trf ("Output: %1", error) << "\n";
    }

    // The chain is clocked by the output device, so publish a snapshot before the
    // callbacks start; otherwise the first block would run an empty chain. A
    // stored chain - including a deliberately empty one - replaces this.
    if (activeChain.empty() && ! startFromStoredChain)
        for (auto type : EffectFactory::defaultChain())
            addEffect (type);

    publishChain();

    /*  Output first, then input. Both managers already have their devices open, so
        the input thread starts producing the moment its callback is registered; if
        that happens before the consumer exists, the ring fills and overflows while
        the output device is still coming up - which is where the "dropped blocks"
        counter at start-up was getting its first few. */
    outputManager.addAudioCallback (this);
    inputManager.addAudioCallback (&inputCallback);
    callbacksRegistered = true;

    if (preferredSampleRate > 0 || preferredBufferSize > 0)
    {
        if (preferredSampleRate > 0)
            if (auto error = setSampleRate (preferredSampleRate); error.isNotEmpty())
                problems << trf ("Sample rate: %1", error) << "\n";

        if (preferredBufferSize > 0)
            if (auto error = setBufferSize (preferredBufferSize); error.isNotEmpty())
                problems << trf ("Buffer size: %1", error) << "\n";
    }

    return problems;
}

void AudioEngine::shutdown()
{
    if (callbacksRegistered)
    {
        // Removing a callback that was never added is an assertion in JUCE, and
        // initialise() is allowed to fail before it ever got this far.
        outputManager.removeAudioCallback (this);
        inputManager.removeAudioCallback (&inputCallback);
        callbacksRegistered = false;
    }

    router->stop();

    closeExpressionMidiInput();

    inputManager.closeAudioDevice();
    outputManager.closeAudioDevice();
}

//==============================================================================
juce::StringArray AudioEngine::listDeviceTypes (bool wantInputs, bool wantOutputs)
{
    juce::StringArray result;

    for (auto* type : inputManager.getAvailableDeviceTypes())
    {
        type->scanForDevices();

        const auto inputs  = type->getDeviceNames (true);
        const auto outputs = type->getDeviceNames (false);

        const auto usable = (wantInputs && inputs.size() > 0)
                         || (wantOutputs && outputs.size() > 0);

        if (usable)
            result.addIfNotAlreadyThere (type->getTypeName());
    }

    return result;
}

juce::StringArray AudioEngine::listDevicesOfType (const juce::String& typeName, bool wantInputs)
{
    for (auto* type : inputManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() != typeName)
            continue;

        type->scanForDevices();
        return type->getDeviceNames (wantInputs);
    }

    return {};
}

void AudioEngine::rescanDevices()
{
    for (auto* type : inputManager.getAvailableDeviceTypes())
        type->scanForDevices();

    for (auto* type : outputManager.getAvailableDeviceTypes())
        type->scanForDevices();

    router->scanForCable();
}

juce::String AudioEngine::setInputDevice (const juce::String& typeName, const juce::String& deviceName)
{
    if (deviceName.isEmpty())
        return tr ("No input device selected.");

    if (typeName.isNotEmpty() && typeName != inputManager.getCurrentAudioDeviceType())
        inputManager.setCurrentAudioDeviceType (typeName, true);

    auto setup = inputManager.getAudioDeviceSetup();

    setup.inputDeviceName = deviceName;

    // Windows device types are full duplex: one name addresses both directions.
    // Keeping the output side on the same device is what JUCE expects when
    // hasSeparateInputsAndOutputs() is false; the input callback never writes to it.
    if (auto* type = inputManager.getCurrentDeviceTypeObject();
        type == nullptr || ! type->hasSeparateInputsAndOutputs())
        setup.outputDeviceName = deviceName;
    else
        setup.outputDeviceName = {};

    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;

    if (auto error = inputManager.setAudioDeviceSetup (setup, true); error.isNotEmpty())
    {
        /*  JUCE closes the current device before it opens the new one, so a
            rejected switch leaves the input side closed. The members below still
            name the device that was running; put it back instead of leaving a
            silent chain behind a selector that promises a working device. */
        if (inputDeviceName.isNotEmpty())
        {
            if (inputTypeName.isNotEmpty())
                inputManager.setCurrentAudioDeviceType (inputTypeName, true);

            auto restore = inputManager.getAudioDeviceSetup();
            restore.inputDeviceName = inputDeviceName;

            if (auto* type = inputManager.getCurrentDeviceTypeObject();
                type == nullptr || ! type->hasSeparateInputsAndOutputs())
                restore.outputDeviceName = inputDeviceName;
            else
                restore.outputDeviceName = {};

            restore.useDefaultInputChannels = true;
            restore.useDefaultOutputChannels = true;

            if (auto restoreError = inputManager.setAudioDeviceSetup (restore, true);
                restoreError.isNotEmpty())
                return error + " (" + tr ("the previous device could not be restored") + ": "
                     + restoreError + ")";

            flushInputRequested.store (true, std::memory_order_release);
        }

        return error;
    }

    inputTypeName = inputManager.getCurrentAudioDeviceType();
    inputDeviceName = inputManager.getAudioDeviceSetup().inputDeviceName;

    /*  The device is open again by now, so the input callback may already be
        writing into the ring on its own thread. The head that has to move is the
        consumer's, and the consumer is the audio thread - so this only asks for
        the flush, and the next block performs it. Anything older than the switch
        is a fraction of a ring at the wrong sample rate, which is exactly what
        must not play back through the speakers. */
    flushInputRequested.store (true, std::memory_order_release);

    return {};
}

juce::String AudioEngine::setOutputDevice (const juce::String& typeName, const juce::String& deviceName)
{
    if (deviceName.isEmpty())
        return tr ("No output device selected.");

    if (typeName.isNotEmpty() && typeName != outputManager.getCurrentAudioDeviceType())
        outputManager.setCurrentAudioDeviceType (typeName, true);

    auto setup = outputManager.getAudioDeviceSetup();

    setup.outputDeviceName = deviceName;
    setup.inputDeviceName  = {};
    setup.useDefaultOutputChannels = true;

    if (auto error = outputManager.setAudioDeviceSetup (setup, true); error.isNotEmpty())
    {
        // Same recovery as setInputDevice: a failed switch must not leave the
        // monitoring path closed when a working device is still known.
        if (outputDeviceName.isNotEmpty())
        {
            if (outputTypeName.isNotEmpty())
                outputManager.setCurrentAudioDeviceType (outputTypeName, true);

            auto restore = outputManager.getAudioDeviceSetup();
            restore.outputDeviceName = outputDeviceName;
            restore.inputDeviceName  = {};
            restore.useDefaultOutputChannels = true;

            if (auto restoreError = outputManager.setAudioDeviceSetup (restore, true);
                restoreError.isNotEmpty())
                return error + " (" + tr ("the previous device could not be restored") + ": "
                     + restoreError + ")";
        }

        return error;
    }

    outputTypeName = outputManager.getCurrentAudioDeviceType();
    outputDeviceName = outputManager.getAudioDeviceSetup().outputDeviceName;

    return {};
}

juce::Array<double> AudioEngine::getAvailableSampleRates() const
{
    if (auto* device = outputManager.getCurrentAudioDevice())
    {
        auto rates = device->getAvailableSampleRates();
        if (rates.size() > 0)
            return rates;
    }

    return { 44100.0, 48000.0, 88200.0, 96000.0 };
}

juce::Array<int> AudioEngine::getAvailableBufferSizes() const
{
    if (auto* device = outputManager.getCurrentAudioDevice())
    {
        auto sizes = device->getAvailableBufferSizes();
        if (sizes.size() > 0)
            return sizes;
    }

    return { 64, 128, 256, 512, 1024 };
}

juce::String AudioEngine::setSampleRate (double newRate)
{
    juce::String problems;

    // Both managers have to agree. If the input device ran at a different rate the
    // monitoring path would still work - the engine resamples - but a device that
    // refuses the rate the user picked is worth reporting rather than hiding.
    auto inputSetup = inputManager.getAudioDeviceSetup();
    inputSetup.sampleRate = newRate;

    if (auto error = inputManager.setAudioDeviceSetup (inputSetup, true); error.isNotEmpty())
        problems << trf ("Input: %1", error) << "; ";

    auto outputSetup = outputManager.getAudioDeviceSetup();
    outputSetup.sampleRate = newRate;

    if (auto error = outputManager.setAudioDeviceSetup (outputSetup, true); error.isNotEmpty())
        problems << trf ("Output: %1", error);

    return problems;
}

juce::String AudioEngine::setBufferSize (int newSizeInSamples)
{
    juce::String problems;

    auto inputSetup = inputManager.getAudioDeviceSetup();
    inputSetup.bufferSize = newSizeInSamples;

    if (auto error = inputManager.setAudioDeviceSetup (inputSetup, true); error.isNotEmpty())
        problems << trf ("Input: %1", error) << "; ";

    auto outputSetup = outputManager.getAudioDeviceSetup();
    outputSetup.bufferSize = newSizeInSamples;

    if (auto error = outputManager.setAudioDeviceSetup (outputSetup, true); error.isNotEmpty())
        problems << trf ("Output: %1", error);

    return problems;
}

//==============================================================================
double AudioEngine::getInputLatencyMs() const noexcept
{
    auto* device = inputManager.getCurrentAudioDevice();
    if (device == nullptr)
        return 0.0;

    const auto rate = device->getCurrentSampleRate();
    if (rate <= 0.0)
        return 0.0;

    // The driver reports the buffer delay it adds; JUCE exposes it in samples via
    // getInputLatencyInSamples(), and the buffer itself has to be counted once more
    // because a callback only sees a full block.
    const auto driverSamples = std::max (0, device->getInputLatencyInSamples());
    const auto blockSamples  = device->getCurrentBufferSizeSamples();

    return static_cast<double> (driverSamples + blockSamples) * 1000.0 / rate;
}

double AudioEngine::getOutputLatencyMs() const noexcept
{
    auto* device = outputManager.getCurrentAudioDevice();
    if (device == nullptr)
        return 0.0;

    const auto rate = device->getCurrentSampleRate();
    if (rate <= 0.0)
        return 0.0;

    const auto driverSamples = std::max (0, device->getOutputLatencyInSamples());
    const auto blockSamples  = device->getCurrentBufferSizeSamples();

    // One extra block is the price of the ring that decouples the two threads: the
    // output callback cannot see input that has not arrived as a whole block yet.
    return static_cast<double> (driverSamples + blockSamples * 2) * 1000.0 / rate;
}

double AudioEngine::getChainLatencyMs() const noexcept
{
    return chainLatencyMs.load (std::memory_order_relaxed);
}

double AudioEngine::getVirtualLatencyMs() const noexcept
{
    // A stopped path adds no delay to anything, and quoting the cable device's
    // buffer here would put a number on screen for a signal that is not flowing.
    // "Enabled" is only the user's intent: without a cable the stream never opens,
    // and the old check quoted a hypothetical 43 ms for a dead path.
    return (router->isEnabled() && router->isOpen()) ? router->getExtraLatencyMs() : 0.0;
}

int AudioEngine::getVirtualDrops() const noexcept
{
    return router->getDropCount();
}

float AudioEngine::getCpuUsage() const noexcept
{
    return static_cast<float> (outputManager.getCpuUsage());
}

//==============================================================================
bool AudioEngine::isTypeInUse (EffectTypeID type) const noexcept
{
    for (auto* effect : activeChain)
        if (effect != nullptr && effect->getTypeID() == type)
            return true;

    return false;
}

EffectTypeID AudioEngine::getSlotType (int index) const noexcept
{
    auto* effect = getSlot (index);

    return effect != nullptr ? static_cast<EffectTypeID> (effect->getTypeID())
                             : static_cast<EffectTypeID> (numEffectTypes);
}

bool AudioEngine::addEffect (EffectTypeID type, int insertIndex)
{
    if (type < 0 || type >= numEffectTypes)
        return false;

    if (static_cast<int> (activeChain.size()) >= maxChainSlots)
        return false;

    auto* effect = factory->get (type);

    if (effect == nullptr || isTypeInUse (type))
        return false;

    const auto position = juce::jmax (0, juce::jmin (insertIndex < 0 ? static_cast<int> (activeChain.size()) : insertIndex,
                                            static_cast<int> (activeChain.size())));

    activeChain.insert (activeChain.begin() + position, effect);
    publishChain();
    return true;
}

bool AudioEngine::removeSlot (int index)
{
    if (index < 0 || index >= static_cast<int> (activeChain.size()))
        return false;

    activeChain.erase (activeChain.begin() + index);
    publishChain();
    return true;
}

bool AudioEngine::moveSlot (int from, int to)
{
    const auto count = static_cast<int> (activeChain.size());

    if (from < 0 || from >= count || to < 0 || to >= count || from == to)
        return false;

    auto* moved = activeChain[static_cast<std::size_t> (from)];
    activeChain.erase (activeChain.begin() + from);
    activeChain.insert (activeChain.begin() + to, moved);

    publishChain();
    return true;
}

void AudioEngine::publishChain()
{
    ChainSnapshot snapshot;
    snapshot.numSlots = static_cast<int> (std::min (activeChain.size(),
                                                    static_cast<std::size_t> (maxChainSlots)));

    for (int i = 0; i < snapshot.numSlots; ++i)
        snapshot.slots[static_cast<std::size_t> (i)] = activeChain[static_cast<std::size_t> (i)];

    // The only synchronisation between "the user dragged a card" and "the audio
    // thread is mid block": a version counter, no lock, no wait.
    chainSnapshot.publish (snapshot);
}

//==============================================================================
juce::StringArray AudioEngine::getMidiInputNames()
{
    juce::StringArray names;

    for (auto& info : juce::MidiInput::getAvailableDevices())
        names.add (info.name);

    return names;
}

juce::String AudioEngine::openExpressionMidiInput (const juce::String& name)
{
    closeExpressionMidiInput();

    juce::String identifier;

    for (auto& info : juce::MidiInput::getAvailableDevices())
        if (info.name == name)
            identifier = info.identifier;

    if (identifier.isEmpty())
        return trf ("Midi device not found: %1", name);

    if (midiReader == nullptr)
        midiReader = std::make_unique<ExpressionMidiReader> (expressionPosition);

    midiInput = juce::MidiInput::openDevice (identifier, nullptr);

    if (midiInput == nullptr)
        return trf ("Could not open %1", name);

    midiInput->addCallback (*midiReader);

    // JUCE 8's MidiInput::start() returns void: openDevice() failing is the only
    // error this path can report, and that is handled above.
    midiInput->start();
    expressionMidiActive.store (true, std::memory_order_relaxed);

    return {};
}

void AudioEngine::closeExpressionMidiInput()
{
    expressionMidiActive.store (false, std::memory_order_relaxed);

    if (midiInput != nullptr)
    {
        midiInput->stop();
        midiInput->removeCallback (*midiReader);
        midiInput.reset();
    }
}

//==============================================================================
//  Audio thread
//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    const auto rate  = device->getCurrentSampleRate();
    const auto block = device->getCurrentBufferSizeSamples();

    outputSampleRate.store (rate, std::memory_order_relaxed);
    outputBlockSize.store (block, std::memory_order_relaxed);

    // The compensation ramp is defined in seconds, so its per-sample step depends
    // on the rate; recompute it for the device that is starting.
    dryDelayStep = 0.0f;

    router->deviceWillRestart();

    // The device is stopped for the whole of this call, so the effects are allowed
    // to size their delay lines, oversamplers and scratch buffers here. Nothing in
    // processBlock() ever allocates.
    //
    // maxBlockSize, not `block`: getCurrentBufferSizeSamples() reports what was
    // requested, and a driver is free to hand the callback a larger batch than
    // that. The engine's own buffers are all sized for the worst case for exactly
    // this reason, and the callback above refuses anything past it - an effect
    // sized to `block` would write its scratch buffer past the end and corrupt
    // the heap (this is what the first run did, in CompressorEffect::process).
    for (int type = 0; type < numEffectTypes; ++type)
        if (auto* effect = factory->get (static_cast<EffectTypeID> (type)))
            effect->prepare (rate, maxBlockSize);

    for (auto& resampler : resamplers)
        resampler.reset();

    /*  This thread owns the read end of inputRing and the write end of
        virtualRing, so dropping the stale backlog is legal here and only here.
        reset() would realloc and rewrite both heads from the wrong thread while
        the input device and the cable are still running. */
    inputRing.flushAsConsumer();
    virtualRing.flushAsProducer();

    /*  The backlog just abandoned was a deliberate decision, not a real-time
        failure, so it must not stay on the drop counters: the status bar would
        report a number the user can never clear and cannot interpret. Measured on
        this machine with a 16-effect chain, the gap while the two devices come up
        costs about a dozen input blocks, once, and never again while running.
        Counters are relaxed atomics, so a concurrent increment can be lost here -
        acceptable for a diagnostic, and only ever during a restart. */
    inputRing.resetCounters();
    virtualRing.resetCounters();

    resampleCarry = 0.0;
    flushInputRequested.store (false, std::memory_order_relaxed);
    dryDelayCurrent = dryDelayTarget = 0.0f;
    dryDelayWrite = 0;

    running.store (true, std::memory_order_relaxed);
}

void AudioEngine::audioDeviceStopped()
{
    running.store (false, std::memory_order_relaxed);
}

void AudioEngine::fillChainBuffer (juce::AudioBuffer<float>& destination, int numSamples) noexcept
{
    float* const* destinationChannels = destination.getArrayOfWritePointers();

    // A device switch asked for the backlog to be dropped. This is the consumer
    // thread, so moving readHead here is the only correct place to do it.
    if (flushInputRequested.exchange (false, std::memory_order_acq_rel))
        inputRing.flushAsConsumer();

    const auto inRate  = inputSampleRate.load (std::memory_order_relaxed);
    const auto outRate = outputSampleRate.load (std::memory_order_relaxed);

    // No input device, or a device that has not reported its rate: emit silence but
    // keep the chain running, which is what lets a reverb tail or a loop survive
    // while the input device is being switched.
    if (inRate <= 0.0 || outRate <= 0.0
        || ! std::isfinite (inRate) || ! std::isfinite (outRate))
    {
        /*  Silence is the right output, but the producer on the other thread is
            still producing, and leaving its samples unread turns a missing clock
            into a ring that overflows on every block. This is the consumer, so
            abandoning the backlog here is legal. */
        inputRing.flushAsConsumer();
        destination.clear();
        return;
    }

    // The common case, and the only one worth being fast about: same clock, so the
    // ring can write straight into the chain buffer.
    if (std::abs (inRate - outRate) < 0.5)
    {
        inputRing.read (destinationChannels, chainChannels, numSamples);
        return;
    }

    // Sample rate bridge. The ring holds input-rate samples, so a block of output
    // needs ratio * numSamples of them; the fractional part is carried to the next
    // block so the two clocks cannot drift apart over time.
    const auto ratio = inRate / outRate;
    resampleCarry += static_cast<double> (numSamples) * ratio;

    auto needed = static_cast<int> (resampleCarry);
    resampleCarry -= needed;

    if (needed <= 0)
    {
        destination.clear();
        return;
    }

    needed = std::min (needed, resampleScratch.getNumSamples());

    inputRing.read (resampleScratch.getArrayOfWritePointers(), chainChannels, needed);

    for (int c = 0; c < chainChannels; ++c)
        resamplers[static_cast<std::size_t> (c)].process (
            ratio,
            resampleScratch.getReadPointer (c),
            destinationChannels[c],
            numSamples);
}

void AudioEngine::updateDryCompensation (const juce::AudioBuffer<float>& input,
                                         juce::AudioBuffer<float>& destination,
                                         int totalLatency,
                                         int numSamples) noexcept
{
    dryDelayTarget = static_cast<float> (totalLatency);

    // Ramping rather than jumping is the whole point: an instantaneous change of the
    // dry delay turns the dry/wet pair into a comb filter that sweeps, which is far
    // more audible than the few milliseconds of smear a ramp costs.
    if (dryDelayStep <= 0.0f)
        dryDelayStep = 1.0f / static_cast<float> (std::max (1, static_cast<int> (0.05 * sampleRateOfChain())));

    // Spelled out rather than deduced: JUCE returns Type* const* / const Type*
    // const*, and an auto** alias silently drops the inner const, which MSVC
    // rejects.
    const float* const* src = input.getArrayOfReadPointers();
    float* const* dst = destination.getArrayOfWritePointers();

    for (int i = 0; i < numSamples; ++i)
    {
        for (int c = 0; c < chainChannels; ++c)
        {
            auto* storage = dryDelayStorage.data() + static_cast<std::size_t> (c) * maxLatencyCompensation;
            storage[static_cast<std::size_t> (dryDelayWrite)] = src[c][i];

            const auto delay = dryDelayCurrent;
            const auto whole = static_cast<int> (delay);
            const auto fraction = delay - static_cast<float> (whole);

            const auto index0 = (dryDelayWrite - whole) & dryDelayMask;
            const auto index1 = (dryDelayWrite - whole - 1) & dryDelayMask;

            const auto* base = storage;
            dst[c][i] = base[index0] + (base[index1] - base[index0]) * fraction;
        }

        if (++dryDelayWrite >= maxLatencyCompensation)
            dryDelayWrite = 0;

        if (dryDelayCurrent < dryDelayTarget)
            dryDelayCurrent = std::min (dryDelayTarget, dryDelayCurrent + dryDelayStep);
        else if (dryDelayCurrent > dryDelayTarget)
            dryDelayCurrent = std::max (dryDelayTarget, dryDelayCurrent - dryDelayStep);
    }
}

double AudioEngine::sampleRateOfChain() const noexcept
{
    return outputSampleRate.load (std::memory_order_relaxed);
}

void AudioEngine::runChain (juce::AudioBuffer<float>& audio, const ChainSnapshot& snapshot) noexcept
{
    for (int i = 0; i < snapshot.numSlots; ++i)
    {
        auto* effect = snapshot.slots[static_cast<std::size_t> (i)];

        if (effect == nullptr)
            continue;

        // Bypass is this branch. The chain buffer already contains the signal as it
        // arrived here, so skipping process() is a true hardware-style bypass:
        // nothing is disconnected and nothing is rebuilt.
        if (effect->isBypassed())
            continue;

        effect->process (audio);
    }
}

void AudioEngine::pushToVirtualRing (const juce::AudioBuffer<float>& out) noexcept
{
    /*  With the cable stream stopped there is no consumer, so writing only fills
        the ring until it overflows: the status bar then reports dropped blocks for
        a path the user deliberately switched off, and the backlog is played out on
        re-enable. The same is true when the switch is on but the cable is not
        installed and the stream never opened. The producer stops instead, and
        abandons whatever boundary is left when the path comes back - here, because
        this thread owns the write end. */
    if (! router->isEnabled() || ! router->isOpen())
        return;

    if (virtualFlushRequested.exchange (false, std::memory_order_acq_rel))
        virtualRing.flushAsProducer();

    virtualRing.write (out.getArrayOfReadPointers(), chainChannels, out.getNumSamples());
}

void AudioEngine::setVirtualOutputEnabled (bool shouldStream)
{
    if (shouldStream)
        virtualFlushRequested.store (true, std::memory_order_release);

    router->setEnabled (shouldStream);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const*, int,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    if (numSamples <= 0)
        return;

    /*  Flush-to-zero for the whole callback. Every effect here is recursive -
        biquads, gain followers, reverb tails, the compressor's RMS power - and
        those states decay exponentially into the denormal range in a silent
        passage, where each multiply costs two orders of magnitude more. No effect
        has to remember to do this itself. */
    const juce::ScopedNoDenormals noDenormals;

    if (numSamples > maxBlockSize)
    {
        // A device that hands out more than the buffers were sized for must not be
        // written into. Silence is the honest failure, and the status bar already
        // reports the buffer size the engine prepared.
        for (int c = 0; c < numOutputChannels; ++c)
            if (auto* destination = outputChannelData[c])
                std::fill_n (destination, numSamples, 0.0f);

        return;
    }

    /*  The buffers are allocated for maxBlockSize, but the effects read
        audio.getNumSamples(), so they have to describe this callback rather than
        their capacity. Left at the full 2048, an effect at a 480-sample device
        block processed four blocks of stale audio per callback (delay lines and
        LFOs then ran 4.27 times faster than wall time), and pushToVirtualRing()
        pushed 2048 samples into a ring the cable drains 480 at a time - which is
        where the thousands of virtual dropped blocks came from. avoidReallocating
        means this is a header update, not a heap call. */
    chainBuffer.setSize (chainChannels, numSamples, false, false, true);
    dryBuffer.setSize   (chainChannels, numSamples, false, false, true);
    mixedBuffer.setSize (chainChannels, numSamples, false, false, true);

    const auto snapshot = chainSnapshot.acquire();

    //--------------------------------------------------------------------------
    // 1. Guitar in, trimmed, and copied before any effect touches it: that copy is
    //    the dry side of the global Mix.
    //--------------------------------------------------------------------------
    fillChainBuffer (chainBuffer, numSamples);

    const auto trim = dbToGain (inputTrimDb.load (std::memory_order_relaxed));

    float blockPeak = 0.0f;

    for (int c = 0; c < chainChannels; ++c)
    {
        auto* data = chainBuffer.getWritePointer (c);
        auto* copy = dryBuffer.getWritePointer (c);

        for (int i = 0; i < numSamples; ++i)
        {
            auto sample = data[i] * trim;

            if (! std::isfinite (sample))
                sample = 0.0f;

            data[i] = sample;
            copy[i] = sample;
            blockPeak = juce::jmax (blockPeak, std::abs (sample));
        }
    }

    /*  Independent of the monitor switch: the card and the status bar need to be
        able to answer "is the guitar arriving?" even while monitoring is off.
        A slow decay makes it a peak hold rather than a per-block flash. */
    const auto heldPeak = juce::jmax (blockPeak,
                                      inputPeak.load (std::memory_order_relaxed) * 0.995f);
    inputPeak.store (heldPeak, std::memory_order_relaxed);

    //--------------------------------------------------------------------------
    // 2. Group delay of the wet path, summed over the slots that are actually on,
    //    then the same delay applied to the dry copy.
    //--------------------------------------------------------------------------
    int totalLatency = 0;

    for (int i = 0; i < snapshot.numSlots; ++i)
    {
        auto* effect = snapshot.slots[static_cast<std::size_t> (i)];

        if (effect != nullptr && ! effect->isBypassed())
            totalLatency += effect->getLatencySamples();
    }

    /*  -2 rather than the buffer size itself. The interpolator reads the taps at
        write-whole and write-whole-1, so a target of exactly maxLatencyCompensation
        wraps both indices back onto the write position: the dry path would deliver
        an *un-delayed* signal - the opposite of partial compensation - while the
        status line still claimed the chain was at its limit. Measured on this
        machine: a 16-effect chain reaches 341.3 ms, which is exactly the cap. */
    constexpr int longestCompensatable = maxLatencyCompensation - 2;

    const auto compensated = std::min (totalLatency, longestCompensatable);
    latencyCompensationSaturated.store (totalLatency > longestCompensatable, std::memory_order_relaxed);

    const auto rate = outputSampleRate.load (std::memory_order_relaxed);
    chainLatencyMs.store (rate > 0.0 ? 1000.0 * static_cast<double> (compensated) / rate : 0.0,
                          std::memory_order_relaxed);

    updateDryCompensation (dryBuffer, mixedBuffer, compensated, numSamples);

    //--------------------------------------------------------------------------
    // 3. The chain itself.
    //--------------------------------------------------------------------------
    runChain (chainBuffer, snapshot);

    //--------------------------------------------------------------------------
    // 4. Global dry/wet, on the compensated pair.
    //--------------------------------------------------------------------------
    const auto wet = clamp01 (globalWet.load (std::memory_order_relaxed));
    const auto dry = 1.0f - wet;

    for (int c = 0; c < chainChannels; ++c)
    {
        auto* bus  = chainBuffer.getWritePointer (c);
        const auto* dryIn = mixedBuffer.getReadPointer (c);

        for (int i = 0; i < numSamples; ++i)
            bus[i] = dryIn[i] * dry + bus[i] * wet;
    }

    //--------------------------------------------------------------------------
    // 5. Path B: the processed signal is copied to the virtual device queue. This
    //    happens whether or not monitoring is on - Monitor is a speaker switch, not
    //    a chain switch - which is what lets a recorder capture while the room is
    //    silent.
    //--------------------------------------------------------------------------
    pushToVirtualRing (chainBuffer);

    //--------------------------------------------------------------------------
    // 6. Path A: the physical monitor, scaled by the Monitor switch only.
    //--------------------------------------------------------------------------
    const auto monitor = monitorEnabled.load (std::memory_order_relaxed);

    for (int c = 0; c < numOutputChannels; ++c)
    {
        if (auto* destination = outputChannelData[c])
        {
            const auto* source = chainBuffer.getReadPointer (c % chainChannels);

            for (int i = 0; i < numSamples; ++i)
                destination[i] = source[i] * monitor;
        }
    }

    //--------------------------------------------------------------------------
    // 7. Meters: the physical one reads what the speakers actually got, the virtual
    //    one reads what the cable carries. They are allowed to disagree, because the
    //    Monitor switch makes them disagree.
    //--------------------------------------------------------------------------
    float peak = 0.0f;

    for (int c = 0; c < chainChannels; ++c)
    {
        const auto* data = chainBuffer.getReadPointer (c);

        for (int i = 0; i < numSamples; ++i)
            peak = std::max (peak, std::abs (data[i]));
    }

    /*  The cable meter must read what the cable carries, not what the chain would
        have carried: with TO CABLE off (or no cable installed) the virtual path is
        stopped, so the meter has to read zero even though the chain is still
        running. The physical meter is the one that reflects the Monitor switch. */
    const auto cableRunning = router->isEnabled() && router->isOpen();
    virtualPeak.store (cableRunning ? peak : 0.0f, std::memory_order_relaxed);
    physicalPeak.store (peak * monitor, std::memory_order_relaxed);
}

//==============================================================================
//  Persistence. Plain key=value text, written next to the executable rather than
//  in %APPDATA%, because every byte of this project has to stay on one drive.
//==============================================================================
juce::File AudioEngine::getSettingsDirectory()
{
    auto directory = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                        .getParentDirectory()
                        .getChildFile ("PedalBoardData");

    directory.createDirectory();
    return directory;
}

juce::File AudioEngine::getSettingsFile()
{
    return getSettingsDirectory().getChildFile ("PedalBoard.settings");
}

juce::String AudioEngine::getSettingsSummary() const
{
    // One template, not four fragments joined with "/": the device type and name
    // are the system's own strings and stay as they are, but the words around them
    // are ours and move with the language.
    return trf ("Input %1 / %2   Output %3 / %4",
                inputTypeName, inputDeviceName, outputTypeName, outputDeviceName);
}

bool AudioEngine::loadFromFile (Settings& result)
{
    const auto text = AudioEngine::getSettingsFile().loadFileAsString();

    // JUCE 8's String has no isEmptyOrWhitespace(): trim then test.
    if (text.trim().isEmpty())
        return false;

    for (auto& line : juce::StringArray::fromTokens (text, "\n", ""))
    {
        const auto trimmed = line.trim();

        if (trimmed.isEmpty() || ! trimmed.contains ("="))
            continue;

        const auto key   = trimmed.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto value = trimmed.fromFirstOccurrenceOf ("=", false, false).trim();

        if (key == "inputType")        result.inputType = value;
        else if (key == "inputDevice") result.inputDevice = value;
        else if (key == "outputType")  result.outputType = value;
        else if (key == "outputDevice") result.outputDevice = value;
        else if (key == "sampleRate")  result.sampleRate = value.getDoubleValue();
        else if (key == "bufferSize")  result.bufferSize = value.getIntValue();
        else if (key == "monitor")     result.monitor = value.getIntValue() != 0;
        else if (key == "globalWet")   result.globalWet = value.getFloatValue();
        else if (key == "inputTrimDb") result.inputTrimDb = value.getFloatValue();
        else if (key == "midiDevice")  result.midiDevice = value;
        else if (key == "virtualOn")   result.virtualEnabled = value.getIntValue() != 0;
        else if (key == "language")    result.language = value.getIntValue();
        else if (key == "chosen")      result.devicesChosen = value.getIntValue() != 0;
        else if (key == "version")     result.version = value.getIntValue();
        else if (key == "cabinetIR")   result.cabinetIR = value;
        else if (key == "cabinetBuiltin") result.cabinetIR = "builtin:" + value;
        else if (key == "pluginPath")  result.pluginPath = value;
        else if (key == "chain")
        {
            result.hasChain = true;

            for (auto& token : juce::StringArray::fromTokens (value, ",", {}))
                if (token.getIntValue() >= 0 && token.getIntValue() < numEffectTypes)
                    result.chain.addIfNotAlreadyThere (static_cast<EffectTypeID> (token.getIntValue()));
        }
        else if (key.startsWith ("params."))
        {
            const auto type = key.fromFirstOccurrenceOf (".", false, false).getIntValue();
            if (type >= 0 && type < numEffectTypes)
                result.parameterStrings[type] = value;
        }
    }

    return true;
}

void AudioEngine::saveSettings()
{
    juce::StringArray lines;

    // Version 2: the pitch shifter's middle knob became Formant (the spectral
    // window is fixed), and the defaults of the pedals that measured wrong were
    // recalibrated. applySettings() upgrades an older file's untouched pedals.
    lines.add ("version=2");
    lines.add ("inputType=" + inputTypeName);
    lines.add ("inputDevice=" + inputDeviceName);
    lines.add ("outputType=" + outputTypeName);
    lines.add ("outputDevice=" + outputDeviceName);
    const auto rate  = isRunning() || requestedSampleRate <= 0.0 ? getSampleRate() : requestedSampleRate;
    const auto block = isRunning() || requestedBufferSize <= 0 ? getBufferSizeSamples() : requestedBufferSize;
    lines.add ("sampleRate=" + juce::String (rate));
    lines.add ("bufferSize=" + juce::String (block));
    lines.add ("monitor=" + juce::String (isMonitorEnabled() ? 1 : 0));
    lines.add ("globalWet=" + juce::String (getGlobalWet(), 4));
    lines.add ("inputTrimDb=" + juce::String (getInputTrimDb(), 2));
    lines.add ("virtualOn=" + juce::String (router->isEnabled() ? 1 : 0));
    lines.add ("language=" + juce::String (storedLanguage));
    lines.add ("chosen="   + juce::String (devicesChosen ? 1 : 0));
    lines.add ("midiDevice=" + getExpressionMidiInputName());

    juce::String chainIds;

    for (std::size_t i = 0; i < activeChain.size(); ++i)
        chainIds << (i > 0 ? "," : "") << activeChain[i]->getTypeID();

    lines.add ("chain=" + chainIds);

    for (int type = 0; type < numEffectTypes; ++type)
    {
        auto* effect = factory->get (static_cast<EffectTypeID> (type));
        if (effect == nullptr)
            continue;

        juce::String values;

        for (int p = 0; p < effect->getNumParameters(); ++p)
            values << (p > 0 ? "|" : "") << juce::String (effect->getParameter (p), 5);

        values << "|b" << (effect->isBypassed() ? 1 : 0);

        lines.add ("params." + juce::String (type) + "=" + values);
    }

    if (auto* cabinet = dynamic_cast<const CabinetEffect*> (factory->get (pedal::cabinet)))
    {
        if (auto path = cabinet->getExternalIRPath(); path.isNotEmpty())
            lines.add ("cabinetIR=" + path);
        else if (const auto builtin = cabinet->getBuiltinSelection(); builtin >= 0)
            lines.add ("cabinetBuiltin=" + juce::String (builtin));
    }

    /*  A hosted plugin's own state is opaque and can be megabytes, so it lives in
        a binary file next to the settings rather than in a key=value line. Asking
        the plugin here, on the message thread, is also what captures changes the
        user made in its native editor: the editor writes into the plugin, and the
        state file is the only place those values are persisted. */
    if (auto* host = dynamic_cast<PluginHostEffect*> (factory->get (pedal::pluginHost)))
    {
        if (auto path = host->getPluginPath(); path.isNotEmpty())
        {
            host->saveStateTo (host->getStateFile());
            lines.add ("pluginPath=" + path);
        }
    }

    getSettingsFile().replaceWithText (lines.joinIntoString ("\n"));
}

juce::Array<EffectTypeID> AudioEngine::getStoredChain() const
{
    return storedChain;
}

juce::String AudioEngine::applyAndStart (const Settings& settings)
{
    startFromStoredChain = settings.hasChain;

    auto problems = initialise (settings.inputType, settings.inputDevice,
                                settings.outputType, settings.outputDevice,
                                settings.sampleRate, settings.bufferSize);

    applySettings (settings);

    // The cabinet's response choice is not a numeric parameter, so a user change
    // has to save the file itself. Wired after applySettings() so restoring a
    // path does not immediately rewrite the settings file mid-load. The plugin
    // host is the same: loading a plugin, clearing it or closing its editor
    // writes both the opaque plugin state and the settings.
    if (auto* cabinet = dynamic_cast<CabinetEffect*> (factory->get (pedal::cabinet)))
        cabinet->onSettingsChanged = [this] { saveSettings(); };

    if (auto* host = dynamic_cast<PluginHostEffect*> (factory->get (pedal::pluginHost)))
        host->onSettingsChanged = [this] { saveSettings(); };

    setVirtualOutputEnabled (settings.virtualEnabled);

    const auto cableStatus = router->getStatus();

    // A cable that failed to open is reported; a cable that is simply absent is
    // not, because the status bar already says so on every refresh. Folding
    // getStatusLine() in here also froze one English sentence into problemText at
    // start-up, which is the one string in the app that a later language change
    // cannot reach.
    if (! cableStatus.error.isEmpty())
        problems << trf ("Virtual device: %1", cableStatus.error);

    return problems;
}

namespace
{
/*  The parameter values written by the previous build. Version 2 of the settings
    file upgrades a pedal only when every one of its values still matches these:
    a knob the user moved is preserved, an untouched pedal picks up the corrected
    default. This matters most for the Pitch Shifter, whose old middle knob
    (Window) no longer exists, and for the Delay's tape modulation and Mix. */
const std::vector<float>* legacyDefaultsFor (int type)
{
    static const std::vector<float> legacyOverdrive    { 55.0f, 55.0f, 45.0f, 1.0f, 0.0f };
    static const std::vector<float> legacyDistortion   { 62.0f, 130.0f, 42.0f, 65.0f, 1.0f, 0.0f };
    static const std::vector<float> legacyChorus       { 0.6f, 45.0f, 8.0f, 0.5f, 70.0f };
    static const std::vector<float> legacyFlanger      { 0.25f, 60.0f, 2.5f, 0.45f, 0.5f, 60.0f };
    static const std::vector<float> legacyTremolo      { 4.5f, 70.0f, 0.0f, 0.0f };
    static const std::vector<float> legacyVibrato      { 4.0f, 40.0f, 5.0f, 1.0f, 0.0f };
    static const std::vector<float> legacyOctaver      { 65.0f, 0.0f, 0.8f };
    static const std::vector<float> legacyPitchShifter { 0.0f, 40.0f, 1.0f };
    static const std::vector<float> legacyDelay        { 380.0f, 0.35f, 60.0f, 0.35f, 0.0f, 15.0f };
    static const std::vector<float> legacyCabinet      { -2.0f, 1.0f, 1.0f };

    switch (type)
    {
        case overdrive:    return &legacyOverdrive;
        case distortion:   return &legacyDistortion;
        case chorus:       return &legacyChorus;
        case flanger:      return &legacyFlanger;
        case tremolo:      return &legacyTremolo;
        case vibrato:      return &legacyVibrato;
        case octaver:      return &legacyOctaver;
        case pitchShifter: return &legacyPitchShifter;
        case delay:        return &legacyDelay;
        case cabinet:      return &legacyCabinet;
        default:           return nullptr;
    }
}

bool matchesLegacyDefaults (int type, const juce::StringArray& tokens)
{
    const auto* expected = legacyDefaultsFor (type);

    if (expected == nullptr || tokens.size() < static_cast<int> (expected->size()))
        return false;

    for (std::size_t i = 0; i < expected->size(); ++i)
        if (std::abs (tokens[static_cast<int> (i)].getFloatValue() - (*expected)[i]) > 1.0e-4f)
            return false;

    return true;
}
} // namespace

void AudioEngine::applySettings (const Settings& settings)
{
    setMonitorEnabled (settings.monitor);
    setGlobalWet (settings.globalWet);
    setInputTrimDb (settings.inputTrimDb);

    storedChain = settings.chain;

    if (settings.hasChain)
    {
        // A saved chain replaces the default one, otherwise restoring would be
        // additive and the order would come out wrong. This includes the empty
        // chain, which is a real choice the user can make.
        activeChain.clear();
        publishChain();

        for (auto type : settings.chain)
            addEffect (type);
    }

    // Knob values are restored after the slots exist, because an effect that is not
    // in the chain has no card and the user must still get its settings back the
    // next time they add it.
    for (int type = 0; type < numEffectTypes; ++type)
    {
        auto* effect = factory->get (static_cast<EffectTypeID> (type));

        if (effect == nullptr || settings.parameterStrings[type].isEmpty())
            continue;

        const auto tokens = juce::StringArray::fromTokens (settings.parameterStrings[type], "|", {});

        /*  The saved line is "v0|v1|...|b0" (or b1). New parameters appended
            after a settings file was written have no token, so the loop must
            stop at the bypass marker rather than let getFloatValue() turn "b0"
            into a zero and overwrite the new knob's default. */
        /*  The pitch shifter changed layout, not merely defaults: old params were
            Semitones|Window|Mix, the new ones are Semitones|Formant|Mix. A user
            who had moved the old Semitones knob must keep it, but the old Window
            value cannot be read as Formant (40 would mean +40 semitones), so the
            middle value is discarded and Formant starts at 0. This runs for every
            pre-version-2 file, regardless of whether the values matched defaults. */
        if (settings.version < 2 && type == pitchShifter && tokens.size() >= 3)
        {
            effect->setParameter (PitchShifterEffect::semitones, tokens[0].getFloatValue());
            effect->setParameter (PitchShifterEffect::formant, 0.0f);
            effect->setParameter (PitchShifterEffect::mix, tokens[2].getFloatValue());

            for (auto& token : tokens)
                if (token.startsWith ("b"))
                    effect->setBypassed (token.substring (1).getIntValue() != 0);

            continue;
        }

        const auto useCorrectedDefaults = settings.version < 2
                                       && matchesLegacyDefaults (type, tokens);

        if (! useCorrectedDefaults)
        {
            for (int p = 0; p < effect->getNumParameters() && p < tokens.size(); ++p)
            {
                if (tokens[p].startsWith ("b"))
                    break;

                effect->setParameter (p, tokens[p].getFloatValue());
            }
        }

        for (auto& token : tokens)
            if (token.startsWith ("b"))
                effect->setBypassed (token.substring (1).getIntValue() != 0);
    }

    if (settings.cabinetIR.isNotEmpty())
    {
        if (auto* cabinet = dynamic_cast<CabinetEffect*> (factory->get (pedal::cabinet)))
        {
            if (settings.cabinetIR.startsWith ("builtin:"))
                cabinet->selectBuiltin (settings.cabinetIR.fromFirstOccurrenceOf (":", false, false).getIntValue());
            else
                cabinet->restoreExternalIR (juce::File (settings.cabinetIR));
        }
    }

    /*  The plugin is loaded after the devices are up, so loadPluginInternal sees
        the real sample rate and block size. A file that has moved since last run
        leaves the reason on the card instead of silently dropping the plugin. */
    if (settings.pluginPath.isNotEmpty())
    {
        if (auto* host = dynamic_cast<PluginHostEffect*> (factory->get (pedal::pluginHost)))
            host->restorePlugin (juce::File (settings.pluginPath), host->getStateFile());
    }

    if (settings.midiDevice.isNotEmpty())
        openExpressionMidiInput (settings.midiDevice);
}

} // namespace pedal
