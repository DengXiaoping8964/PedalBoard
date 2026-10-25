#pragma once

#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Audio/AudioEngine.h"
#include "Common/PedalLanguage.h"
#include "UI/DeviceChooserPanel.h"
#include "UI/EffectCard.h"
#include "UI/PedalWidgets.h"

/*==============================================================================
  MainComponent - everything inside the one application window.

  Layout, top to bottom:

    * Top bar, two rows. Row 1 is device selection: an input type + input device
      pair and an output type + output device pair that are completely independent
      of one another, then buffer size and sample rate, then the Devices button.
      Row 2 is the performance surface: the Monitor switch, the virtual-device
      switch, the three global knobs, the two meters and the add-effect selector.
    * The chain: a Viewport whose content is a ChainList, i.e. the vertical column
      of effect cards. Vertical because a pedalboard reads top to bottom, and
      scrollable because sixteen cards will never fit on one screen.
    * Status bar: the VB-CABLE line, the four latency figures, CPU and the drop
      counters. Everything the user needs to judge whether the recording path is
      honest about its delay.

  Threading: this file is message-thread only. It reads and writes the engine's
  atomics and its message-thread methods; it never touches an AudioDeviceManager
  or an AudioIODevice directly. The meters are driven from a 40 ms timer that
  samples the block peaks the audio thread publishes, which is the only direction
  audio state travels into the UI.

  Reordering: a card's header strip is the drag grip. The card forwards its mouse
  events here; nothing moves while the pointer is down, only a drop gap indicator
  is drawn, and the engine's moveSlot() is called exactly once on release. That
  avoids rebuilding (and therefore deleting) components in the middle of the mouse
  gesture that started it - the classic way to crash a drag implementation.
==============================================================================*/
namespace pedal
{

/**
    The scrollable column that holds the effect cards.

    It owns nothing: MainComponent keeps the cards alive and hands this class the
    order to lay out. It sizes itself to the sum of its card heights, which is what
    the Viewport uses to compute its scroll range (verified in
    juce_Viewport.cpp: updateVisibleArea() derives the range from the content
    component's bounds and only ever moves it, never resizes it).
*/
class ChainList : public juce::Component
{
public:
    /** Breathing room between two cards, also the height of the drop indicator. */
    static constexpr int cardGap = 8;

    void setCards (std::vector<EffectCard*> newCards)
    {
        cards = std::move (newCards);
        layoutCards();
    }

    /** Places every card from the top down and resizes this component to fit. */
    void layoutCards();

    /** Which insertion gap a pointer y falls into: 0 is above the first card,
        getNumCards() is below the last. */
    int gapForY (int yInThisComponent) const noexcept;

    /** -1 hides the indicator. */
    void setDropGap (int newGap) noexcept { dropGap = newGap; repaint(); }

    int getDropGap() const noexcept { return dropGap; }

    /** The card whose body the pointer is over, clamped so a gap of getNumCards()
        still resolves to the last card. Used to light exactly one card. */
    EffectCard* cardForGap (int gap) const noexcept;

    int getNumCards() const noexcept { return static_cast<int> (cards.size()); }

    void paint (juce::Graphics&) override;

private:
    std::vector<EffectCard*> cards;
    int dropGap = -1;
};

//==============================================================================
/**
    Note there is no MouseListener base: juce::Component already derives from
    MouseListener publicly, so overriding mouseUp here and handing 'this' to
    Desktop::addGlobalMouseListener() uses that single base. Inheriting
    MouseListener a second time would make the upcast ambiguous.
*/
class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    /** The engine outlives this component: the window tears the content down
        before the engine, so every reference held here stays valid. */
    explicit MainComponent (AudioEngine& engineToUse);
    ~MainComponent() override;

    JUCE_DECLARE_NON_COPYABLE (MainComponent)

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Writes the current engine state to the settings file. Called on any
        structural change and once more when the window closes. */
    void persistSettings();

    //==============================================================================
    //  Meter refresh interval and the slower cadence for card text.
    static constexpr int uiRefreshMs = 40;
    static constexpr int statusRefreshDivisor = 12;   // ~480 ms

private:
    //==============================================================================
    //  Construction
    //==============================================================================
    void buildGlobalControls();
    void buildChainArea();

    //==============================================================================
    //  Device selection
    //==============================================================================
    /** Refills the two type boxes and the two name boxes from the engine, keeping
        the current selection. Runs with the change callbacks suppressed. */
    void populateDeviceBoxes();
    void populateNameBox (juce::ComboBox& box, const juce::StringArray& names,
                          const juce::String& selectedName);
    void populateRateBox();
    void populateBufferBox();
    void populateAddBox();

    void inputTypeChanged();
    void inputDeviceChanged();
    void outputTypeChanged();
    void outputDeviceChanged();
    void bufferChanged();
    void sampleRateChanged();
    void addEffectChanged();

    /** Pushes engine state back into the switches and knobs, e.g. after the
        settings file has been applied at start-up. */
    void refreshGlobalControls();

    //==============================================================================
    //  Chain
    //==============================================================================
    void rebuildCards();
    void startCardDrag (EffectCard& card, const juce::MouseEvent& e);
    void updateCardDrag (EffectCard& card, const juce::MouseEvent& e);
    void finishCardDrag();
    void removeCard (EffectCard* card);
    void moveCard (EffectCard* card, int direction);

    /** Only the release is interesting: the card itself never tells us, because
        deleting a component inside its own mouse gesture is unsafe. */
    void mouseUp (const juce::MouseEvent&) override;

    //==============================================================================
    //  Status
    //==============================================================================
    void timerCallback() override;
    void updateStatusText();

    void showDeviceChooser();
    void hideDeviceChooser();

    //==============================================================================
    //  Language
    //==============================================================================
    /** Switches the interface language and re-applies every string that is held
        by a widget rather than drawn from a literal. Never restarts the audio:
        the engine is not involved, only its names are. */
    void applyLanguage (Language newLanguage);

    /** The first-run question. It is asked before the device chooser so the
        chooser itself appears in the language that was just picked. */
    void showLanguagePrompt();

    /** Re-applies the text of every widget that keeps a copy of its own, which is
        everything except what paint() draws straight from a literal. Called after
        a language change; it does not touch the engine. */
    void refreshAllText();

    //==============================================================================
    //  Top bar geometry, in one place so the rows can be re-tuned together.
    //==============================================================================
    static constexpr int topBarHeight   = 112;
    static constexpr int statusLineHeight = 15;
    static constexpr int statusBarHeight  = statusLineHeight * 4 + 6;   // cable, latency, load, problem
    static constexpr int rowOneY        = 12;    // caption strip starts here
    static constexpr int comboHeight    = 22;
    static constexpr int rowTwoY        = 66;
    static constexpr int rowTwoHeight   = 40;
    static constexpr int edgeMargin     = 12;
    static constexpr int boxGap         = 10;

    /*  Fixed widths, measured against the strings they have to hold: the longest
        device type this machine offers is "Windows Audio (Exclusive Mode)" but the
        one that gets selected is "Windows Audio", which needs ~100 px at the combo
        font; the buffer box shows "480 (10.0 ms)", which needs ~96. The row adds up
        to 1066 logical pixels including the Devices button, and that total is what
        sets minWindowWidth in PedalConfig.h - the first builds clipped both labels
        because the row was wider than the window was allowed to be. */
    static constexpr int inputTypeWidth   = 110;
    static constexpr int deviceNameWidth  = 210;
    static constexpr int bufferBoxWidth   = 104;
    static constexpr int rateBoxWidth     = 100;
    static constexpr int devicesWidth     = 92;
    static constexpr int addBoxWidth      = 200;
    static constexpr int meterWidth       = 150;
    static constexpr int meterHeight      = 26;
    static constexpr int knobWidth        = 68;
    static constexpr int flagWidth        = 112;
    static constexpr int flagHeight       = 30;

    /** Row 1's x positions, computed in one place: paint() draws the captions and
        resized() places the boxes, and two copies of that arithmetic would drift
        apart the moment a width changes. */
    struct RowOnePositions
    {
        int inputType, inputDevice, outputType, outputDevice, buffer, rate, devices;
    };

    static RowOnePositions rowOnePositions() noexcept;

    //==============================================================================
    AudioEngine& engine;

    std::unique_ptr<juce::Viewport> viewport;
    ChainList* chainList = nullptr;                 // owned by the viewport
    std::vector<std::unique_ptr<EffectCard>> cards;

    std::unique_ptr<juce::ComboBox> inputTypeBox, inputDeviceBox;
    std::unique_ptr<juce::ComboBox> outputTypeBox, outputDeviceBox;
    std::unique_ptr<juce::ComboBox> bufferBox, sampleRateBox, addBox;

    std::unique_ptr<Knob> dryWetKnob, trimKnob, expressionKnob;
    std::unique_ptr<FlagButton> monitorFlag, virtualFlag;
    std::unique_ptr<LevelMeter> physicalMeter, virtualMeter;
    std::unique_ptr<SmallTextButton> devicesButton;

    /** True when start-up decided the device chooser should be up but the
        language question has not been answered yet, so the chooser can be asked
        for after the answer instead of underneath it. */
    bool wantsDeviceChooser = false;

    /** Held by this component and parented to it, so it must be a pointer: a
        Component that is a child gets deleted by the parent's destructor, which
        would destroy a value member a second time. */
    std::unique_ptr<DeviceChooserPanel> chooser;

    //==============================================================================
    //  Drawn text, rebuilt only when it actually changes.
    //==============================================================================
    juce::StringArray statusLines;
    juce::String problemText;

    /** Copies of the device's own lists, so a box change can map an item id back
        to a real rate or buffer size without asking the driver again. */
    juce::Array<double> availableRates;
    juce::Array<int> availableBuffers;

    EffectCard* draggingCard = nullptr;
    int dragFromIndex = -1;
    int statusTickCount = 0;

    /** Structural changes are applied on the next timer tick, never inside a
        callback that belongs to a component about to be deleted. */
    bool pendingRebuild = false;

    /** Set when the rebuild was caused by adding an effect, so the new card at
        the end of the chain is scrolled into view instead of appearing to do
        nothing. */
    bool revealLastCard = false;

    /** Guards the ComboBox change callbacks while populate*() is writing them. */
    bool suppressBoxCallbacks = false;
};

} // namespace pedal
