#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Audio/AudioEngine.h"
#include "UI/PedalWidgets.h"

/*==============================================================================
  EffectCard - one pedal in the board.

  What a card is allowed to do, and why it is shaped this way:

    * It displays one PedalEffect for as long as it lives. The effect comes from
      the engine's fixed pool of 22 instances, so the reference never changes and
      never dangles; the slot index does change when the board is reordered, which
      is why it is a separate piece of state with its own setter.
    * It writes parameters straight into the effect's std::atomic<float> array and
      nowhere else. There is no SliderAttachment and no value tree, because there
      is no AudioProcessor here - the atomic *is* the transport between this card
      and the audio thread, and adding a layer on top of it would only add a place
      for a update-and-echo loop to start.
    * It never deletes anything. Removal, reordering and their rebuilds belong to
      MainComponent; the callbacks below only announce intent. A card that deleted
      itself would be deleting the button whose click callback is still on the
      stack.
    * The header strip doubles as the drag grip. Child components swallow their own
      clicks, so a press that reaches the card is by construction a press on empty
      header - no separate hit-test flag is needed to keep the buttons from
      starting a drag.
    * Its height is fixed at construction from the parameter count, the presence of
      actions, and two reserved footer lines. The footer is always two lines even
      when empty: the cabinet's "no IR loaded" warning and the looper's transport
      state appear and disappear at run time, and a card that grew when a warning
      appeared would move the card under the user's finger.

  Verified against this JUCE 8.0.15 checkout: juce_Component.h (addAndMakeVisible,
  removeChildComponent, getLocalBounds), juce_GraphicsContext.h (drawText with a
  Rectangle<int>, drawFittedText), and the widget constructors in PedalWidgets.h.
==============================================================================*/
namespace pedal
{

class EffectCard : public juce::Component
{
public:
    /** @param slotIndex this card's position in the chain when it was built. */
    EffectCard (AudioEngine& engineToUse, PedalEffect& effectToDisplay, int slotIndex);
    ~EffectCard() override;

    JUCE_DECLARE_NON_COPYABLE (EffectCard)

    //==============================================================================
    int getSlotIndex() const noexcept { return slot; }

    /** Repaints the index badge and re-evaluates which move buttons are enabled. */
    void setSlotIndex (int newIndex);

    PedalEffect& getEffect() noexcept { return effect; }
    const PedalEffect& getEffect() const noexcept { return effect; }
    EffectTypeID getTypeID() const noexcept { return static_cast<EffectTypeID> (effect.getTypeID()); }

    /** Pulls knob positions, the bypass lamp and both footer lines from the
        effect. Safe to call on any cadence; it only reads atomics and
        message-thread strings. */
    void refresh();

    /** Highlight while the board's drop indicator is on this card. */
    void setDropTarget (bool isDropTarget);

    /** Dim the card while it is the one being dragged. */
    void setBeingDragged (bool isBeingDragged);

    //==============================================================================
    //  Installed by MainComponent. None of these may delete a card.
    std::function<void (EffectCard*)> onRequestRemove;
    std::function<void (EffectCard*, int direction)> onRequestMove;      // -1 up, +1 down
    std::function<void (EffectCard*, const juce::MouseEvent&)> onGripDrag;

    //==============================================================================
    /** The height MainComponent should give this card. It depends only on things
        that cannot change while the card exists: the parameter count and whether
        the effect publishes actions. Not noexcept, because asking an effect for its
        actions builds a small vector. */
    static int getPreferredHeight (const PedalEffect&);

    /** Layout metrics, public because the board's drop indicator lines up with them. */
    static constexpr int headerHeight = 26;
    static constexpr int knobRowHeight = 74;
    static constexpr int actionsHeight = 26;
    static constexpr int footerHeight = 34;    // two reserved lines
    static constexpr int maxKnobsPerRow = 4;
    static constexpr int edgeMargin = 8;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    //==============================================================================
    void buildControls();
    void buildActionButtons();
    void updateMoveButtons();
    juce::Rectangle<int> headerArea() const noexcept;

    AudioEngine& engine;
    PedalEffect& effect;
    int slot = 0;

    // Widgets, owned here. Knob and selector pointers are also in controls, which
    // keeps the ordering for layout and the typed access for refresh() in one list.
    std::vector<std::unique_ptr<juce::Component>> controls;
    std::vector<Knob*> knobs;
    std::vector<DiscreteSelector*> selectors;
    std::vector<std::unique_ptr<SmallTextButton>> actionButtons;

    std::unique_ptr<FlagButton> bypassFlag;
    std::unique_ptr<SmallTextButton> moveUp, moveDown, removeButton;

    // Drawn strings, rebuilt only in refresh() so paint() allocates nothing.
    juce::String badgeText, statusText, warningText;

    /** Where the two footer lines live, computed in resized() and read in paint().
        Kept as state because the actions row above it is optional, so the footer's
        position depends on what the effect publishes. */
    juce::Rectangle<int> footerBounds;

    bool dropTarget = false;
    bool beingDragged = false;
    bool draggingGrip = false;

    int knobRows = 1;
    bool hasActions = false;
};

} // namespace pedal
