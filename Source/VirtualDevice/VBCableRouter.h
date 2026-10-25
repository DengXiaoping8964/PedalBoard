#pragma once

#include <array>
#include <atomic>

#include <juce_audio_devices/juce_audio_devices.h>

#include "Common/PedalConfig.h"
#include "Common/PedalLockFree.h"

namespace pedal
{

/*==============================================================================
  VB-CABLE router

  What this is not: it does not ship a driver, it does not install anything, and it
  does not silently launch an installer. VB-CABLE is Donationware written by Vincent
  Burel; the app only ever opens a device that is already in the system, and if it is
  not there the UI says so, names https://vb-audio.com/Cable/, states the licence, and
  every other function - the input device, the whole effect chain, physical
  monitoring, both meters - keeps working exactly as before.

  What it is: a third AudioDeviceManager, on its own thread, whose callback drains
  the engine's virtual ring and writes to the stream a recording application reads
  as "CABLE Output". Separate manager and separate thread mean the physical monitor
  cannot be disturbed by it; the lock free ring means it can never block the
  monitoring callback either. When the cable falls behind, the cable loses audio and
  a counter increments - not the speakers.

  Thread discipline, because this file has three of them:
    * message thread  - scanForCable(), start(), stop(), setEnabled(), getStatus().
                        Owns every juce::String below.
    * engine thread   - deviceWillRestart(), which may only touch atomics.
    * cable thread    - the callbacks, which own the numeric device state and the
                        resampler history.
  A string is therefore only ever written while the cable callback is not registered.
==============================================================================*/
class VBCableRouter : private juce::AudioIODeviceCallback
{
public:
    /** The engine's real block size is part of the latency figure: quoting the
        worst-case buffer instead of the one actually running would add 35 ms of
        phantom delay at a 512-sample block, and the whole point of this number is
        that the user can trust it. */
    VBCableRouter (AudioRingBuffer& source,
                   const std::atomic<double>& engineSampleRate,
                   const std::atomic<int>& engineBlockSize) noexcept;
    ~VBCableRouter() override;

    JUCE_DECLARE_NON_COPYABLE (VBCableRouter)

    enum class Availability : int { unknown = 0, notInstalled, installed, running };

    /** Which driver model opened the cable, because that is what decides the latency
        figure shown to the user. */
    enum class DriverKind : int { other = 0, asio, mme, directSound, wasapi };

    struct Status
    {
        Availability availability = Availability::unknown;
        bool enabled = true;
        bool open = false;
        juce::String deviceName;         ///< e.g. "CABLE Input (VB-Audio Virtual Cable)"
        juce::String deviceType;         ///< e.g. "Windows Audio", "MME", "ASIO"
        juce::String error;              ///< last failure text, empty when healthy
        double extraLatencyMs = 0.0;     ///< estimate, see the note above
        double deviceSampleRate = 0.0;
        int deviceBufferSize = 0;
        int drops = 0;
    };

    /** Finds an output device whose name contains "CABLE Input" in every device type
        the system offers. Message thread only; call at start-up and on Refresh. */
    bool scanForCable();

    /** Opens the device and starts streaming. Returns an error string, empty on
        success. Never touches the monitoring path - different manager, different
        thread. */
    juce::String start();
    void stop();

    /** Off stops the cable stream only. The engine keeps writing the ring, so
        switching back on is instant and never reconfigures the physical device. */
    void setEnabled (bool shouldStream);
    bool isEnabled() const noexcept { return enabled.load (std::memory_order_relaxed); }

    /** True only while the cable callback is registered, i.e. there is a consumer
        on the ring. enabled is the user's choice and stays true when the cable is
        not installed, so the producer must ask this one before writing. */
    bool isOpen() const noexcept { return open.load (std::memory_order_relaxed); }

    /** Called from the engine's audioDeviceAboutToStart(), i.e. from the engine
        thread, so it publishes nothing but an atomic. The cable stream is left
        running on purpose: a rate change is bridged by resampling rather than by
        interrupting playback. */
    void deviceWillRestart() noexcept;

    /** Message thread only: assembles the atomics and the strings it owns. */
    Status getStatus() const;

    double getExtraLatencyMs() const noexcept { return extraLatency.load (std::memory_order_relaxed); }
    int getDropCount() const noexcept { return source.getOverflowCount() + source.getUnderflowCount(); }

    /** One line for the status bar, worded so a user can act on it. */
    juce::String getStatusLine() const;

    /** The licence and installation notice, shown when nothing was found. */
    static juce::String getInstallationGuidance();

private:
    //==============================================================================
    //  Cable thread
    //==============================================================================
    void audioDeviceIOCallbackWithContext (const float* const*, int,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    void refreshLatencyEstimate() noexcept;

    AudioRingBuffer& source;
    const std::atomic<double>& engineSampleRate;
    const std::atomic<int>& engineBlockSize;

    juce::AudioDeviceManager cableManager;
    std::array<juce::LagrangeInterpolator, chainChannels> resamplers;
    juce::AudioBuffer<float> scratch;

    //==============================================================================
    //  Shared state: atomics only
    //==============================================================================
    std::atomic<bool>       enabled { true };
    std::atomic<bool>       open { false };
    std::atomic<int>        availability { static_cast<int> (Availability::unknown) };
    std::atomic<int>        driverKind { static_cast<int> (DriverKind::other) };
    std::atomic<double>     extraLatency { 0.0 };
    std::atomic<double>     cableRate { 0.0 };
    std::atomic<int>        cableBuffer { 0 };

    /** Set by the engine thread when its clock changed; consumed by the cable
        thread, which is the only thread that owns the interpolator history. */
    std::atomic<bool>       resyncRequested { true };

    //==============================================================================
    //  Message-thread-only state
    //==============================================================================
    juce::String deviceName, deviceType, lastError;

    //==============================================================================
    //  Cable-thread-only state
    //==============================================================================
    double localEngineRate = 0.0;    // last rate seen on this thread, for change detection
    double resampleCarry = 0.0;
    DriverKind localDriverKind = DriverKind::other;
};

} // namespace pedal
