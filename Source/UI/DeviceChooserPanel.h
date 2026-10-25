#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Audio/AudioEngine.h"
#include "Common/PedalLanguage.h"
#include "UI/PedalWidgets.h"

/*==============================================================================
  DeviceChooserPanel - the first-run device setup, and the Devices dialog after it.

  Why this is a panel and not a stack of alerts: the user has to see the input and
  the output side by side, because the whole design depends on them being two
  different devices, and because the honest answer about the virtual path (is
  VB-CABLE there or not, and what does it cost in latency) belongs in the same
  place where they choose it.

  It applies its own selections through the engine's public setters, in the order
  that keeps the audio path alive: input device, output device, then the clock,
  then the buffer, then MIDI, then the cable. Applying a sample rate before the
  devices would have the two managers on different clocks for a moment, and a
  moment is enough for the resampler to see a nonsense ratio.

  Nothing here enumerates or opens a device directly. It is all AudioEngine and
  VBCableRouter public calls, on the message thread.

  Verified against this checkout: juce_ComboBox.h (addItem, addSectionHeading is not
  used here, clear(NotificationType), setSelectedId(int, NotificationType),
  getSelectedId, getText, the deprecated bool overloads are avoided),
  juce_GraphicsContext.h (drawMultiLineText, drawText with Rectangle<int>),
  juce_Label.h only through the look-and-feel.
==============================================================================*/
namespace pedal
{

class DeviceChooserPanel : public juce::Component
{
public:
    explicit DeviceChooserPanel (AudioEngine& engineToUse);
    ~DeviceChooserPanel() override;

    JUCE_DECLARE_NON_COPYABLE (DeviceChooserPanel)

    /** Plain data, so MainComponent can fold it into AudioEngine::Settings without
        the panel having to own a settings object. */
    struct Selection
    {
        juce::String inputType, inputDevice, outputType, outputDevice, midiDevice;
        double sampleRate = 0.0;   ///< 0 means "leave the clock alone"
        int bufferSize = 0;        ///< 0 means "leave the buffer alone"
    };

    /** Enumerates everything, keeping whatever is currently selected. Call this
        before showing the panel and again after a rescan. */
    void populate();

    Selection getSelection() const;

    /** Applies devices, then clock, then buffer, then MIDI, then the cable.
        Returns every failure it saw, empty when all of them took. */
    juce::String applySelections();

    /** Re-reads the router's status line and shows the donation-ware notice when
        there is no cable. Cheap: it reads atomics and message-thread strings. */
    void refreshCableStatus();

    /** Re-applies every caption this panel holds a copy of - the three buttons,
        the virtual-output flag, the language box, and the drawn paragraphs, which
        are cached rather than rebuilt per paint. Called after the interface
        language changes; it changes nothing but text. */
    void refreshText();

    std::function<void()> onAccepted;
    std::function<void()> onCancelled;

    /** Fired the moment a different language is picked, not when Apply is: the
        interface language is not a device setting and should not wait behind a
        button labelled Apply. */
    std::function<void (Language)> onLanguageChosen;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    //==============================================================================
    void fillTypeBox (juce::ComboBox& box, bool wantInputs);
    void fillDeviceBox (juce::ComboBox& box, const juce::String& typeName, bool wantInputs);
    void fillRateBox();
    void fillBufferBox();
    void fillMidiBox();

    /** The labelled rows of the card. Computed by one function and read by both
        resized(), which places the boxes, and paint(), which draws the caption
        above each one - two copies of that arithmetic would drift. */
    struct Rows
    {
        int captionX = 0, boxX = 0, narrowWidth = 0, wideX = 0, wideWidth = 0;
        int inputY = 0, outputY = 0, clockY = 0, midiY = 0;
        int cableY = 0;
    };

    static Rows layoutRows (const juce::Rectangle<int>& card) noexcept;

    /** The card rect the panel draws inside whatever bounds it was given. */
    juce::Rectangle<int> cardBounds() const noexcept;

    /** Where the language box sits, in the button row. One function because
        paint() has to draw its caption against the same edge resized() uses. */
    static juce::Rectangle<int> languageBoxBounds (const juce::Rectangle<int>& card) noexcept;

    /** Item ids used by languageBox, which lists the languages in a fixed order. */
    enum LanguageItems : int { englishItem = 1, chineseItem = 2 };

    AudioEngine& engine;

    std::unique_ptr<juce::ComboBox> inputTypeBox, inputDeviceBox;
    std::unique_ptr<juce::ComboBox> outputTypeBox, outputDeviceBox;
    std::unique_ptr<juce::ComboBox> bufferBox, sampleRateBox, midiBox;

    std::unique_ptr<FlagButton> virtualFlag;
    std::unique_ptr<SmallTextButton> rescanButton, acceptButton, cancelButton;
    std::unique_ptr<juce::ComboBox> languageBox;

    juce::Array<double> availableRates;
    juce::Array<int> availableBuffers;

    // Drawn paragraphs, rebuilt only by populate()/refreshCableStatus().
    juce::String cableLine, cableGuidance, summaryLine, problemLine;

    bool suppressChanges = false;

    /** The router's own availability, kept as a bool so the warning colour of the
        status line does not depend on reading English out of a translated string. */
    bool cableMissing = false;
};

} // namespace pedal
