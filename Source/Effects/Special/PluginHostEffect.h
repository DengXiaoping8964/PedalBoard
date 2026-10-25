#pragma once

#include "Effects/PedalEffect.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

/*==============================================================================
  Plugin Host

  Hosts one VST3 effect inside the chain. This is the fully open version of the
  Custom Chain: instead of a fixed set of built-in stages, the user chooses the
  effect file.

  How hosting works here, and why it is shaped this way:

    * VST3PluginFormat::findAllTypesForFile() reads a .vst3's class descriptions;
      AudioPluginFormatManager::createPluginInstance() then builds the instance.
      Both are message-thread operations and can block for a moment, so they
      never run on the audio thread.

    * The audio thread only reads an atomic pointer to a prepared Hosted object.
      Loading another plugin prepares the new instance first, then swaps the
      pointer. Retired instances are kept alive until the effect dies, because a
      block may still be inside the old plugin when the swap happens; the cost is
      a few megabytes per load and no lock or deallocation on the audio thread.

    * The eight knobs are macros over the first eight automatable parameters, in
      the plugin's own order. The native editor is where every parameter lives.
      Moving a macro calls setValueNotifyingHost() from the message thread; the
      audio thread copies the plugin's values back into the atomics so the card
      follows changes made in the editor.

    * The plugin's state is written next to the settings file when the settings
      are saved, and restored after the instance is prepared. The path and state
      are deliberately outside the numeric parameter array.

  The VST3 SDK is compiled into JUCE; this app does not install, copy or bundle
  any plugin. A missing or non-effect file leaves the previous plugin running and
  says why on the card.
==============================================================================*/
namespace pedal
{

class PluginHostEffect : public PedalEffect
{
public:
    enum Params
    {
        macro1, macro2, macro3, macro4, macro5, macro6, macro7, macro8, numParams
    };

    enum Actions { actionLoadPlugin = 1, actionOpenEditor = 2, actionClearPlugin = 3 };

    static constexpr int numMacros = 8;

    PluginHostEffect()
        : PedalEffect (pluginHost, "Plugin Host",
            {
                { "Param 1", 0.0f, 100.0f, 50.0f, "%" },
                { "Param 2", 0.0f, 100.0f, 50.0f, "%" },
                { "Param 3", 0.0f, 100.0f, 50.0f, "%" },
                { "Param 4", 0.0f, 100.0f, 50.0f, "%" },
                { "Param 5", 0.0f, 100.0f, 50.0f, "%" },
                { "Param 6", 0.0f, 100.0f, 50.0f, "%" },
                { "Param 7", 0.0f, 100.0f, 50.0f, "%" },
                { "Param 8", 0.0f, 100.0f, 50.0f, "%" },
            })
    {
    }

    ~PluginHostEffect() override
    {
        if (chooserGuard != nullptr)
            chooserGuard->alive.store (false, std::memory_order_release);

        active.store (nullptr, std::memory_order_release);

        for (auto& host : hosts)
        {
            closeEditor (*host);
            host->instance.reset();
        }
    }

    //==============================================================================
    void doPrepare (double, int) override
    {
        if (auto* host = active.load (std::memory_order_acquire))
        {
            host->instance->prepareToPlay (sampleRate, blockSize);
            latencySamples.store (host->instance->getLatencySamples(), std::memory_order_relaxed);
        }
        else
        {
            latencySamples.store (0, std::memory_order_relaxed);
        }
    }

    void resetState() noexcept override
    {
        // PedalEffect::prepare() calls this before doPrepare(), when no instance
        // is guaranteed to exist. The plugin is reset by prepareToPlay().
    }

    int getLatencySamples() const noexcept override
    {
        return latencySamples.load (std::memory_order_relaxed);
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        auto* host = active.load (std::memory_order_acquire);

        if (host == nullptr || host->instance == nullptr)
            return;

        const auto numSamples = audio.getNumSamples();

        if (numSamples <= 0)
            return;

        auto& buffer = host->buffer;

        for (int c = 0; c < chainChannels; ++c)
            buffer.copyFrom (c, 0, audio, c, 0, numSamples);

        host->midi.clear();

        juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), chainChannels, numSamples);
        host->instance->processBlock (view, host->midi);

        // A mono plugin only writes channel 0; duplicate it so the monitor and the
        // virtual output both carry the processed signal.
        if (host->instance->getTotalNumOutputChannels() < chainChannels)
            for (int i = 0; i < numSamples; ++i)
                view.setSample (1, i, view.getSample (0, i));

        for (int c = 0; c < chainChannels; ++c)
            audio.copyFrom (c, 0, view, c, 0, numSamples);

        latencySamples.store (host->instance->getLatencySamples(), std::memory_order_relaxed);

        // Follow the plugin's own parameter values, so moving a control in the
        // native editor is reflected by the card's macros. getValue() is the
        // realtime-side read the parameter API is designed for.
        for (int i = 0; i < numMacros; ++i)
            if (auto* parameter = host->mapped[static_cast<std::size_t> (i)])
                getRawParameter (i).store (parameter->getValue() * 100.0f,
                                           std::memory_order_relaxed);
    }

    void parameterWasSet (int index, float value) override
    {
        if (index < 0 || index >= numMacros || suppressMacroWriteback)
            return;

        auto* host = active.load (std::memory_order_acquire);

        if (host == nullptr)
            return;

        if (auto* parameter = host->mapped[static_cast<std::size_t> (index)])
            parameter->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, value * 0.01f));
    }

    //==============================================================================
    std::vector<EffectAction> actions() const override
    {
        return { { actionLoadPlugin, "Load VST3..." },
                 { actionOpenEditor, "Open Editor" },
                 { actionClearPlugin, "Clear" } };
    }

    void handleAction (int id) override
    {
        if (id == actionLoadPlugin)
            choosePluginFile();
        else if (id == actionOpenEditor)
            openPluginEditor();
        else if (id == actionClearPlugin)
            clearPlugin();
    }

    juce::String getStatusText() const override
    {
        if (loadError.isNotEmpty())
            return trf ("Plugin error: %1", tr (loadError));

        if (auto* host = active.load (std::memory_order_acquire))
            return trf ("Plugin: %1 (%2 of %3 params on knobs)",
                        host->name, host->mappedCount, host->parameterCount);

        return tr ("No plugin loaded - use Load VST3");
    }

    juce::String getWarningText() const override
    {
        if (loadError.isNotEmpty())
            return tr ("The previous plugin, if any, is still active. Only .vst3 effect files are supported.");

        if (active.load (std::memory_order_acquire) == nullptr)
            return tr ("Hosts a VST3 effect. The file stays where it is; this app does not install or copy plugins.");

        return tr ("The eight knobs follow the plugin's first eight automatable parameters, skipping bypass; open the editor for all controls.");
    }

    //==============================================================================
    //  Message thread, called by AudioEngine for persistence.
    //==============================================================================
    juce::String getPluginPath() const { return pluginPath; }

    juce::File getStateFile() const
    {
        return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                   .getParentDirectory()
                   .getChildFile ("PedalBoardData")
                   .getChildFile ("customPlugin.state");
    }

    bool saveStateTo (const juce::File& file)
    {
        auto* host = active.load (std::memory_order_acquire);

        if (host == nullptr || host->instance == nullptr)
        {
            file.deleteFile();
            return false;
        }

        juce::MemoryBlock state;
        host->instance->getStateInformation (state);

        if (state.getSize() == 0)
            return false;

        /*  The blob itself is opaque, so it carries no clue about which plugin it
            belongs to. Prefixing the path is what stops a state file left by one
            plugin from being fed to the next one - a mismatch that can silently
            misconfigure an unrelated effect. */
        const auto path = pluginPath.toRawUTF8();
        const auto pathBytes = static_cast<juce::uint32> (std::strlen (path));

        juce::MemoryBlock wrapped;
        wrapped.append (&pathBytes, sizeof (pathBytes));
        wrapped.append (path, pathBytes);
        wrapped.append (state.getData(), state.getSize());

        file.getParentDirectory().createDirectory();
        return file.replaceWithData (wrapped.getData(), wrapped.getSize());
    }

    bool restorePlugin (const juce::File& file, const juce::File& stateFile)
    {
        return loadPluginInternal (file, &stateFile, false);
    }

    /** Fired when the plugin or its state changes, so the engine can write the
        path and state without the effect knowing about the settings file. */
    std::function<void()> onSettingsChanged;

    //==============================================================================
private:
    class EditorWindow : public juce::DocumentWindow
    {
    public:
        explicit EditorWindow (const juce::String& name)
            : juce::DocumentWindow (name, juce::Colours::black,
                                    juce::DocumentWindow::closeButton)
        {
        }

        std::function<void()> onClose;

        void closeButtonPressed() override
        {
            setVisible (false);

            if (onClose != nullptr)
                onClose();
        }
    };

    struct Hosted
    {
        std::unique_ptr<juce::AudioPluginInstance> instance;
        juce::AudioBuffer<float> buffer;
        juce::MidiBuffer midi;
        std::array<juce::AudioProcessorParameter*, numMacros> mapped {};
        std::unique_ptr<EditorWindow> window;
        juce::String name;
        int mappedCount = 0;
        int parameterCount = 0;

        // The editor must die before the instance it was created from.
        ~Hosted()
        {
            window.reset();
            instance.reset();
        }
    };

    static juce::AudioPluginFormatManager& pluginFormats()
    {
        // Function-local static: the format manager and its VST3 code live until
        // process exit, after every Hosted instance has been destroyed.
        static juce::AudioPluginFormatManager manager;
        static const bool initialised = [] { juce::addDefaultFormatsToManager (manager); return true; }();

        static_cast<void> (initialised);
        return manager;
    }

    juce::AudioPluginFormat* findVst3Format (const juce::File& file)
    {
        auto& manager = pluginFormats();

        for (int i = 0; i < manager.getNumFormats(); ++i)
        {
            auto* format = manager.getFormat (i);

            if (format != nullptr
                && format->getName().equalsIgnoreCase ("VST3")
                && format->fileMightContainThisPluginType (file.getFullPathName()))
                return format;
        }

        return nullptr;
    }

    bool loadPluginInternal (const juce::File& file, const juce::File* stateFile, bool notify)
    {
        loadError.clear();

        if (! file.exists())
        {
            loadError = "plugin file not found";
            return false;
        }

        auto* format = findVst3Format (file);

        if (format == nullptr)
        {
            loadError = "no VST3 format could open this file";
            return false;
        }

        juce::OwnedArray<juce::PluginDescription> descriptions;

        try
        {
            format->findAllTypesForFile (descriptions, file.getFullPathName());
        }
        catch (...)
        {
            loadError = "the plugin crashed or refused to be scanned";
            return false;
        }

        if (descriptions.isEmpty())
        {
            loadError = "no plugin class found in this file";
            return false;
        }

        auto* description = descriptions[0];

        for (auto* candidate : descriptions)
            if (candidate != nullptr && ! candidate->isInstrument)
            {
                description = candidate;
                break;
            }

        if (description == nullptr || description->isInstrument)
        {
            loadError = "this file is an instrument, not an effect";
            return false;
        }

        const auto rate = sampleRate > 0.0 ? sampleRate : 44100.0;
        const auto block = blockSize > 0 ? blockSize : 512;

        juce::String creationError;
        auto instance = pluginFormats().createPluginInstance (*description, rate, block, creationError);

        if (instance == nullptr)
        {
            loadError = creationError.isNotEmpty() ? creationError
                                                    : "the plugin could not be instantiated";
            return false;
        }

        if (instance->getTotalNumInputChannels() <= 0 || instance->getTotalNumOutputChannels() <= 0)
        {
            loadError = "the plugin has no audio input or output";
            return false;
        }

        /*  Keep the chain stereo. This has to be checked before
            setPlayConfigDetails(), which clamps the cached counts and would make
            the check below pass for a surround plugin that still expects its own
            bus arrangement. */
        if (instance->getTotalNumInputChannels() > chainChannels
            || instance->getTotalNumOutputChannels() > chainChannels)
        {
            loadError = "more than stereo input/output is not supported";
            return false;
        }

        instance->setPlayConfigDetails (instance->getTotalNumInputChannels(),
                                        instance->getTotalNumOutputChannels(),
                                        rate, static_cast<int> (block));
        instance->prepareToPlay (rate, static_cast<int> (block));

        if (stateFile != nullptr && stateFile->existsAsFile())
        {
            juce::MemoryBlock fileData;

            if (stateFile->loadFileAsData (fileData)
                && fileData.getSize() > sizeof (juce::uint32))
            {
                juce::uint32 pathBytes = 0;
                std::memcpy (&pathBytes, fileData.getData(), sizeof (pathBytes));

                const auto headerBytes = static_cast<juce::uint32> (sizeof (pathBytes));

                if (pathBytes > 0 && headerBytes + pathBytes < fileData.getSize())
                {
                    const auto* bytes = static_cast<const char*> (fileData.getData());
                    const auto storedPath = juce::String::fromUTF8 (bytes + headerBytes,
                                                                    static_cast<int> (pathBytes));

                    // The state is only for the plugin that wrote it. Windows
                    // paths are case-insensitive, so a differing case is the same
                    // file, not a different one.
                    if (storedPath.equalsIgnoreCase (file.getFullPathName()))
                        instance->setStateInformation (
                            bytes + headerBytes + pathBytes,
                            static_cast<int> (fileData.getSize() - headerBytes - pathBytes));
                }
            }
        }

        auto hosted = std::make_unique<Hosted>();
        hosted->instance = std::move (instance);
        hosted->buffer.setSize (chainChannels, maxBlockSize, false, true, true);
        hosted->name = description->descriptiveName.isNotEmpty() ? description->descriptiveName
                                                                 : description->name;

        auto& pluginParameters = hosted->instance->getParameters();
        hosted->parameterCount = pluginParameters.size();

        /*  The macros are the first eight automatable parameters, minus the
            plugin's own bypass. VST3 exposes bypass through
            getBypassParameter(), and many plugins (the one this was measured
            with included) declare it as parameter 0 - mapping a knob to it makes
            the first knob a mute switch and hides a real control. Some plugins
            also publish a "Bypass" parameter without implementing the virtual,
            so the name is checked as well. A stepped bypass parameter is not a
            useful candidate either, but a deliberately stepped control (a mode
            selector) is, so only the bypass itself is skipped. */
        auto* bypassParameter = hosted->instance->getBypassParameter();

        for (auto* parameter : pluginParameters)
        {
            if (parameter == nullptr || parameter == bypassParameter
                || ! parameter->isAutomatable()
                || parameter->getName (64).containsIgnoreCase ("bypass")
                || hosted->mappedCount >= numMacros)
                continue;

            hosted->mapped[static_cast<std::size_t> (hosted->mappedCount)] = parameter;
            ++hosted->mappedCount;
        }

        suppressMacroWriteback = true;
        for (int i = 0; i < numMacros; ++i)
            if (auto* parameter = hosted->mapped[static_cast<std::size_t> (i)])
                getRawParameter (i).store (parameter->getValue() * 100.0f,
                                           std::memory_order_relaxed);
        suppressMacroWriteback = false;

        // Old editor first, then retire the old instance while the audio thread
        // may still be using it for one more block.
        if (auto* old = active.load (std::memory_order_acquire))
            closeEditor (*old);

        hosts.push_back (std::move (hosted));
        active.store (hosts.back().get(), std::memory_order_release);
        latencySamples.store (hosts.back()->instance->getLatencySamples(), std::memory_order_relaxed);

        pluginPath = file.getFullPathName();
        loadError.clear();

        if (notify && onSettingsChanged != nullptr)
            onSettingsChanged();

        return true;
    }

    void clearPlugin()
    {
        if (auto* old = active.load (std::memory_order_acquire))
            closeEditor (*old);

        active.store (nullptr, std::memory_order_release);
        latencySamples.store (0, std::memory_order_relaxed);
        pluginPath.clear();
        loadError.clear();

        // With no plugin the eight macros control nothing, so leaving the last
        // plugin's values on screen would claim state that is no longer there.
        for (int i = 0; i < numMacros; ++i)
            getRawParameter (i).store (getParameterDescription (i).defaultValue,
                                       std::memory_order_relaxed);

        if (onSettingsChanged != nullptr)
            onSettingsChanged();
    }

    void openPluginEditor()
    {
        auto* host = active.load (std::memory_order_acquire);

        if (host == nullptr || host->instance == nullptr)
            return;

        if (host->window != nullptr)
        {
            host->window->setVisible (true);
            host->window->toFront (true);
            return;
        }

        if (! host->instance->hasEditor())
        {
            loadError = "this plugin has no editor";
            return;
        }

        auto* editor = host->instance->createEditorAndMakeActive();

        if (editor == nullptr)
        {
            loadError = "the plugin editor could not be created";
            return;
        }

        auto window = std::make_unique<EditorWindow> (host->name);
        window->setUsingNativeTitleBar (true);
        window->setContentOwned (editor, true);
        window->setResizable (editor->isResizable(), false);
        window->centreWithSize (juce::jmax (360, editor->getWidth()),
                                juce::jmax (240, editor->getHeight()) + 30);
        window->setVisible (true);
        window->onClose = [this]
        {
            if (onSettingsChanged != nullptr)
                onSettingsChanged();
        };

        host->window = std::move (window);
    }

    static void closeEditor (Hosted& host)
    {
        if (host.window != nullptr)
        {
            host.window->setVisible (false);
            host.window.reset();       // deletes the editor before the instance
        }
    }

    void choosePluginFile()
    {
        if (chooser == nullptr)
        {
            auto startDirectory = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                       .getParentDirectory().getChildFile ("Plugins");

            if (! startDirectory.isDirectory())
                startDirectory = juce::File();

            chooser = std::make_unique<juce::FileChooser> (tr ("Load VST3 plugin"),
                                                           startDirectory,
                                                           "*.vst3");
        }

        chooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [this, guard = chooserGuard] (const juce::FileChooser& chosen)
                              {
                                  if (guard == nullptr
                                      || ! guard->alive.load (std::memory_order_acquire))
                                      return;

                                  const auto file = chosen.getResult();

                                  /*  A .vst3 on Windows is either a single DLL
                                      file or a bundle directory; both are valid
                                      and the format object accepts either. Only
                                      "nothing was selected" is rejected here. */
                                  if (! file.exists())
                                      return;

                                  /*  No state file on an explicit load: a new
                                      choice starts from the plugin's own
                                      defaults. The restore path is only for the
                                      plugin path recorded in the settings file,
                                      whose state header also has to match. */
                                  loadPluginInternal (file, nullptr, true);
                              });
    }

    struct ChooserGuard
    {
        std::atomic<bool> alive { true };
    };

    std::shared_ptr<ChooserGuard> chooserGuard = std::make_shared<ChooserGuard>();
    std::unique_ptr<juce::FileChooser> chooser;

    std::atomic<Hosted*> active { nullptr };
    std::vector<std::unique_ptr<Hosted>> hosts;      // message thread; instances live until destruction
    std::atomic<int> latencySamples { 0 };

    juce::String pluginPath;                         // message thread
    juce::String loadError;                          // message thread
    bool suppressMacroWriteback = false;             // message thread
};

} // namespace pedal
