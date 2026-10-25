#include "VirtualDevice/VBCableRouter.h"

#include <algorithm>
#include <cmath>

#include <atomic>

#include "Common/PedalLanguage.h"

namespace pedal
{

namespace
{
    bool looksLikeCable (const juce::String& name)
    {
        return name.containsIgnoreCase (cableOutputDeviceHint);
    }

    /*  DriverKind and Availability are nested enums of VBCableRouter, so at this
        scope they have to be qualified - unqualified use does not compile. */
    using Kind = VBCableRouter::DriverKind;

    Kind driverKindFor (const juce::String& typeName)
    {
        if (typeName.containsIgnoreCase ("ASIO"))         return Kind::asio;
        if (typeName.containsIgnoreCase ("MME"))          return Kind::mme;
        if (typeName.containsIgnoreCase ("DirectSound"))  return Kind::directSound;
        if (typeName.containsIgnoreCase ("WASAPI")
            || typeName.containsIgnoreCase ("Windows Audio")) return Kind::wasapi;

        return Kind::other;
    }

    /** VB-Audio's own documentation is explicit that the latency of the cable depends
        on which driver model opened it: the ASIO bridge is roughly one extra buffer,
        plain MME is far looser. Those separate figures are shown rather than one
        optimistic number, because promising low latency here would be both untrue and
        the wrong thing to say about recording, which is buffered by definition.

        One engine block is unavoidable regardless of driver: the ring is written and
        read a block at a time. */
    double estimateExtraLatencyMs (Kind kind,
                                   double cableRate,
                                   int cableBuffer,
                                   double engineRate,
                                   int engineBuffer) noexcept
    {
        const auto cableBlockMs  = cableRate > 0.0 && cableBuffer > 0
                                 ? 1000.0 * static_cast<double> (cableBuffer) / cableRate : 11.6;

        const auto engineBlockMs = engineRate > 0.0 && engineBuffer > 0
                                 ? 1000.0 * static_cast<double> (engineBuffer) / engineRate : 11.6;

        switch (kind)
        {
            case Kind::asio:         return engineBlockMs + cableBlockMs + 3.0;
            case Kind::mme:          return engineBlockMs + std::max (cableBlockMs * 2.0, 150.0);
            case Kind::directSound:  return engineBlockMs + cableBlockMs * 2.0 + 20.0;
            case Kind::wasapi:
            default:                 return engineBlockMs + cableBlockMs * 2.0 + 10.0;
        }
    }
}

//==============================================================================
VBCableRouter::VBCableRouter (AudioRingBuffer& sourceRing,
                              const std::atomic<double>& rate,
                              const std::atomic<int>& blockSize) noexcept
    : source (sourceRing),
      engineSampleRate (rate),
      engineBlockSize (blockSize)
{
    scratch.setSize (chainChannels, maxBlockSize * 5, false, true, true);
}

VBCableRouter::~VBCableRouter()
{
    stop();
}

//==============================================================================
bool VBCableRouter::scanForCable()
{
    // A private type list, purely for searching: no device is opened, so a scan can
    // never interrupt what the other two managers are already playing.
    juce::OwnedArray<juce::AudioIODeviceType> types;
    cableManager.createAudioDeviceTypes (types);

    for (auto* type : types)
    {
        type->scanForDevices();

        for (auto& name : type->getDeviceNames (false))
        {
            if (! looksLikeCable (name))
                continue;

            deviceName = name;
            deviceType = type->getTypeName();
            localDriverKind = driverKindFor (deviceType);
            lastError.clear();

            availability.store (static_cast<int> (Availability::installed), std::memory_order_relaxed);
            driverKind.store (static_cast<int> (localDriverKind), std::memory_order_relaxed);
            return true;
        }
    }

    deviceName.clear();
    deviceType.clear();
    availability.store (static_cast<int> (Availability::notInstalled), std::memory_order_relaxed);
    return false;
}

juce::String VBCableRouter::start()
{
    if (! enabled.load (std::memory_order_relaxed))
        return tr ("Virtual recording is switched off.");

    if (deviceName.isEmpty() && ! scanForCable())
        return tr ("VB-CABLE is not installed.");

    stop();

    /*  A manager that has never been initialised has no device types at all, and
        both setCurrentAudioDeviceType() and setAudioDeviceSetup() then do nothing
        and return an empty error string. That is exactly what happened here: the
        app said "Active, adds about 43.2 ms" while no stream existed, and the drop
        counter climbed once per engine block because nothing was draining the
        ring. initialise() creates the types; the preferred-device wildcard makes
        it choose the cable rather than the system speakers, so no unrelated device
        is opened on the way. */
    if (auto error = cableManager.initialise (0, chainChannels, nullptr, false,
                                              deviceName, nullptr); error.isNotEmpty())
    {
        lastError = error;
        availability.store (static_cast<int> (Availability::installed), std::memory_order_relaxed);
        return error;
    }

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = deviceName;
    setup.inputDeviceName  = {};                    // playback direction only
    setup.sampleRate       = engineSampleRate.load (std::memory_order_relaxed);
    setup.bufferSize       = 0;                     // the device default
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;

    if (auto error = cableManager.setAudioDeviceSetup (setup, true); error.isNotEmpty())
    {
        lastError = error;
        cableManager.closeAudioDevice();
        availability.store (static_cast<int> (Availability::installed), std::memory_order_relaxed);
        return error;
    }

    /*  The device object, not the error string, is the proof that the stream
        exists. A manager that accepted the setup but opened nothing would leave
        the UI claiming a running cable and the ring without a consumer. */
    if (auto* opened = cableManager.getCurrentAudioDevice())
    {
        cableRate.store (opened->getCurrentSampleRate(), std::memory_order_relaxed);
        cableBuffer.store (opened->getCurrentBufferSizeSamples(), std::memory_order_relaxed);
        deviceType = opened->getTypeName();
    }
    else
    {
        lastError = tr ("The cable device reported no open stream.");
        cableManager.closeAudioDevice();
        availability.store (static_cast<int> (Availability::installed), std::memory_order_relaxed);
        return lastError;
    }

    open.store (true, std::memory_order_relaxed);
    resyncRequested.store (true, std::memory_order_relaxed);
    lastError.clear();
    refreshLatencyEstimate();

    // Registering the callback is what starts streaming. Everything above ran while
    // no cable thread existed, which is why the strings are safe to touch here.
    source.resetCounters();   // a new stream is judged on its own record
    cableManager.addAudioCallback (this);
    availability.store (static_cast<int> (Availability::running), std::memory_order_relaxed);

    return {};
}

void VBCableRouter::stop()
{
    if (! open.load (std::memory_order_relaxed))
        return;

    cableManager.removeAudioCallback (this);   // joins the cable thread
    cableManager.closeAudioDevice();

    open.store (false, std::memory_order_relaxed);
    availability.store (static_cast<int> (Availability::installed), std::memory_order_relaxed);
}

void VBCableRouter::setEnabled (bool shouldStream)
{
    enabled.store (shouldStream, std::memory_order_relaxed);

    if (! shouldStream)
    {
        stop();
        return;
    }

    // Already streaming: reopening the device would drop the cable for the time it
    // takes to start again and would zero the drop counters that describe it. This
    // runs on every Apply from the device panel, not just on a real state change.
    if (open.load (std::memory_order_relaxed))
        return;

    start();
}

void VBCableRouter::deviceWillRestart() noexcept
{
    // The engine's clock moved. The cable stream is left running: the interpolator
    // bridges the two rates, which is exactly why there is a ring here instead of a
    // shared buffer. Only a flag may be published from this thread.
    resyncRequested.store (true, std::memory_order_relaxed);
    refreshLatencyEstimate();
}

void VBCableRouter::refreshLatencyEstimate() noexcept
{
    const auto rate = cableRate.load (std::memory_order_relaxed);
    const auto buffer = cableBuffer.load (std::memory_order_relaxed);
    const auto kind = static_cast<DriverKind> (driverKind.load (std::memory_order_relaxed));

    extraLatency.store (estimateExtraLatencyMs (kind, rate, buffer,
                                                engineSampleRate.load (std::memory_order_relaxed),
                                                engineBlockSize.load (std::memory_order_relaxed)),
                        std::memory_order_relaxed);
}

VBCableRouter::Status VBCableRouter::getStatus() const
{
    Status result;
    result.availability = static_cast<Availability> (availability.load (std::memory_order_relaxed));
    result.enabled      = enabled.load (std::memory_order_relaxed);
    result.open         = open.load (std::memory_order_relaxed);
    result.deviceName   = deviceName;
    result.deviceType   = deviceType;
    result.error        = lastError;
    result.extraLatencyMs = extraLatency.load (std::memory_order_relaxed);
    result.deviceSampleRate = cableRate.load (std::memory_order_relaxed);
    result.deviceBufferSize = cableBuffer.load (std::memory_order_relaxed);
    result.drops        = getDropCount();
    return result;
}

juce::String VBCableRouter::getStatusLine() const
{
    const auto state = static_cast<Availability> (availability.load (std::memory_order_relaxed));

    if (state == Availability::notInstalled)
        return tr ("VB-CABLE not detected - virtual recording unavailable");

    if (! enabled.load (std::memory_order_relaxed))
        return tr ("VB-CABLE found - virtual recording switched off");

    if (state == Availability::running)
        return trf ("Virtual Device: %1 - Active, adds about %2 ms to the recording path",
                    deviceName, juce::String (extraLatency.load (std::memory_order_relaxed), 0));

    if (! lastError.isEmpty())
        return trf ("VB-CABLE could not be opened: %1", lastError);

    return tr ("VB-CABLE found - stream not started");
}

juce::String VBCableRouter::getInstallationGuidance()
{
    /*  Four paragraphs rather than one string with \n\n in it: the translation
        table is read one line at a time, so a key containing a newline could not
        be stored. Joined here, so the layout is the same in every language. */
    return tr ("Recording through a virtual soundcard needs VB-CABLE, which this application "
               "neither bundles nor installs.")
         + "\n\n"
         + tr ("VB-CABLE is Donationware by Vincent Burel. If you keep using it the author "
               "asks for a donation; the exact terms are on the download page.")
         + "\n\n"
         + tr ("Install it from https://vb-audio.com/Cable/ and then restart PedalBoard: "
               "Windows only publishes newly installed audio devices to an application at "
               "process start, so there is no way for this app to see it sooner.")
         + "\n\n"
         + tr ("Nothing else is blocked in the meantime. The input device, the effect chain, "
               "the physical monitoring path and both meters work without it.");
}

//==============================================================================
void VBCableRouter::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr)
    {
        cableRate.store (device->getCurrentSampleRate(), std::memory_order_relaxed);
        cableBuffer.store (device->getCurrentBufferSizeSamples(), std::memory_order_relaxed);
    }

    localEngineRate = 0.0;              // forces a resync on the first block
    resyncRequested.store (true, std::memory_order_relaxed);

    /*  This thread is the read end of the ring, so dropping the backlog left by a
        stopped cable is legal here and nowhere else. Without it, re-enabling the
        virtual output after it has been off replays up to a full ring of old
        audio into the recorder before the clocks line up again. */
    source.flushAsConsumer();

    refreshLatencyEstimate();
}

void VBCableRouter::audioDeviceStopped()
{
    availability.store (static_cast<int> (Availability::installed), std::memory_order_relaxed);
}

void VBCableRouter::audioDeviceIOCallbackWithContext (const float* const*, int,
                                                      float* const* outputChannelData,
                                                      int numOutputChannels,
                                                      int numSamples,
                                                      const juce::AudioIODeviceCallbackContext&)
{
    if (numOutputChannels == 0 || numSamples <= 0)
        return;

    if (numSamples > scratch.getNumSamples())
    {
        for (int c = 0; c < numOutputChannels; ++c)
            std::fill_n (outputChannelData[c], numSamples, 0.0f);

        return;
    }

    if (resyncRequested.exchange (false, std::memory_order_acq_rel))
    {
        for (auto& resampler : resamplers)
            resampler.reset();

        resampleCarry = 0.0;
    }

    const auto engineRate = engineSampleRate.load (std::memory_order_relaxed);
    const auto cableRateNow = cableRate.load (std::memory_order_relaxed);

    if (engineRate != localEngineRate)
    {
        // The engine changed clock mid stream. Resetting here, on the thread that
        // owns the interpolators, is what keeps the history coherent.
        localEngineRate = engineRate;
        resyncRequested.store (true, std::memory_order_relaxed);

        for (auto& resampler : resamplers)
            resampler.reset();

        resampleCarry = 0.0;
        refreshLatencyEstimate();
    }

    // Silence is the only option without two known rates. The ring is not drained,
    // so the samples that arrived meanwhile are simply dropped on the floor of a
    // queue that is, by design, allowed to overflow.
    if (cableRateNow <= 0.0 || engineRate <= 0.0
        || ! std::isfinite (cableRateNow) || ! std::isfinite (engineRate))
    {
        for (int c = 0; c < numOutputChannels; ++c)
            std::fill_n (outputChannelData[c], numSamples, 0.0f);

        return;
    }

    if (std::abs (cableRateNow - engineRate) < 0.5)
    {
        // Same clock: drain the ring straight into the device's own buffers. A cable
        // is mono in practice, so the stereo chain lands on as many channels as the
        // device actually has.
        float* channels[chainChannels];
        const auto usable = std::min (numOutputChannels, chainChannels);

        for (int c = 0; c < usable; ++c)
            channels[c] = outputChannelData[c];

        source.read (channels, usable, numSamples);

        for (int c = usable; c < numOutputChannels; ++c)
            std::fill_n (outputChannelData[c], numSamples, 0.0f);

        return;
    }

    // Different clocks: read the engine-rate samples this cable block needs, then
    // interpolate them onto the cable's own clock.
    const auto ratio = engineRate / cableRateNow;
    resampleCarry += static_cast<double> (numSamples) * ratio;

    auto needed = static_cast<int> (resampleCarry);
    resampleCarry -= needed;

    if (needed <= 0)
    {
        for (int c = 0; c < numOutputChannels; ++c)
            std::fill_n (outputChannelData[c], numSamples, 0.0f);

        return;
    }

    needed = std::min (needed, scratch.getNumSamples());

    float* scratchChannels[chainChannels];
    const auto usableScratch = std::min (numOutputChannels, chainChannels);

    for (int c = 0; c < usableScratch; ++c)
        scratchChannels[c] = scratch.getWritePointer (c);

    source.read (scratchChannels, usableScratch, needed);

    for (int c = 0; c < usableScratch; ++c)
    {
        resamplers[static_cast<std::size_t> (c)].process (
            ratio,
            scratch.getReadPointer (c),
            outputChannelData[c],
            numSamples);
    }

    for (int c = usableScratch; c < numOutputChannels; ++c)
        std::fill_n (outputChannelData[c], numSamples, 0.0f);
}

} // namespace pedal
