#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/*==============================================================================
  PedalBoard - the single dark look-and-feel shared by every widget.

  Why this exists as a LookAndFeel_V4 subclass rather than a plain theme struct:
    JUCE 8 removed the old LookAndFeel_V2 style setters (setDefaultComboBoxProperties,
    setDefaultScrollbarColour, setAlertWindowTheme, setSliderStyleInformation - none of
    these exist in 8.0.15, verified against lookandfeel/juce_LookAndFeel_V2.h and
    juce_LookAndFeel_V4.h). The supported path is now:
      * setColourScheme() with a LookAndFeel_V4::ColourScheme, which is what
        initialiseColours() uses to seed stock widget colours; and
      * per-widget setColour() calls against each widget's ColourIds enum
        (verified: ComboBox, Label, ScrollBar, AlertWindow, Slider, TextButton,
        TextEditor, ResizableWindow, DocumentWindow, TooltipWindow all still expose
        them in this checkout).
    setUsingNativeAlertWindows (false) is required so AlertWindow uses our
    juce::AlertWindow::ColourIds rather than the OS dialog, otherwise a dark app pops a
    light system box.

  The widgets in PedalWidgets.h read the static colours below directly, so they stay
  consistent with the stock widgets even where they paint themselves.

  makeFont builds every font in the app through juce::FontOptions, because JUCE 8
  deprecates the (height, style) and (name, height, style) Font constructors. An
  earlier note here claimed Font::setHeight does not exist; it does (juce_Font.h:274),
  it is just not what the deprecated constructors were replaced with.

  The font family is not chosen here: makeFont asks the language module for one, so
  Chinese gets a face with CJK coverage and English keeps the system default.
==============================================================================*/
namespace pedal
{

class DarkLookAndFeel : public juce::LookAndFeel_V4
{
public:
    //==============================================================================
    //  Static palette. Defined in the .cpp so all widgets and the scheme agree.
    static const juce::Colour windowBackground;   // very dark, near black
    static const juce::Colour panelBackground;     // effect card body
    static const juce::Colour panelHighlighted;    // card being dragged / hovered
    static const juce::Colour accent;              // one strong accent for on-states
    static const juce::Colour accentDim;
    static const juce::Colour textPrimary;
    static const juce::Colour textDim;
    static const juce::Colour warning;
    static const juce::Colour good;
    static const juce::Colour bad;
    static const juce::Colour meterTop;
    static const juce::Colour meterBottom;

    //==============================================================================
    DarkLookAndFeel();
    ~DarkLookAndFeel() override;

    /** One shared instance, created on first use. MainComponent installs this with
        juce::LookAndFeel::setDefaultLookAndFeel, and every custom widget falls back
        to it for colours. */
    static DarkLookAndFeel& instance();

    /** Builds a Font from a pixel height using the JUCE 8 API. */
    static juce::Font makeFont (float height);

    //==============================================================================
    //  Font overrides so the stock widgets use our metrics on a near-black field.
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getAlertWindowMessageFont() override;

    JUCE_DECLARE_NON_COPYABLE (DarkLookAndFeel)
};

} // namespace pedal
