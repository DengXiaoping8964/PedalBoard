#include "UI/DarkLookAndFeel.h"

#include "Common/PedalLanguage.h"

namespace pedal
{

//==============================================================================
//  Palette. Tuned for a guitar rack: near-black canvas, one amber accent for the
//  engaged/bypassed state, and a green-to-red meter ramp.
const juce::Colour DarkLookAndFeel::windowBackground  ( 0xff121214u );
const juce::Colour DarkLookAndFeel::panelBackground   ( 0xff1c1e22u );
const juce::Colour DarkLookAndFeel::panelHighlighted  ( 0xff2a2e36u );
const juce::Colour DarkLookAndFeel::accent            ( 0xfff2a13cu );
const juce::Colour DarkLookAndFeel::accentDim         ( 0xff8a5c22u );
const juce::Colour DarkLookAndFeel::textPrimary       ( 0xffe8e8eau );
const juce::Colour DarkLookAndFeel::textDim           ( 0xff8f9299u );
const juce::Colour DarkLookAndFeel::warning           ( 0xffe0b341u );
const juce::Colour DarkLookAndFeel::good              ( 0xff57c26bu );
const juce::Colour DarkLookAndFeel::bad               ( 0xffe05c4au );
const juce::Colour DarkLookAndFeel::meterTop          ( 0xffe05c4au );
const juce::Colour DarkLookAndFeel::meterBottom       ( 0xff57c26bu );

//==============================================================================
DarkLookAndFeel::DarkLookAndFeel()
{
    // Seed the base-class scheme first: initialiseColours() derives the stock widget
    // colours from these nine entries, so setting it before the per-widget overrides
    // below means the overrides are not silently re-clobbered.
    setColourScheme (ColourScheme { windowBackground,   // windowBackground
                                    panelBackground,    // widgetBackground
                                    panelBackground,    // menuBackground
                                    textDim,            // outline
                                    textPrimary,        // defaultText
                                    accent,             // defaultFill
                                    windowBackground,   // highlightedText
                                    accent,             // highlightedFill
                                    textPrimary });     // menuText

    // Force JUCE-drawn alert boxes so they inherit the dark theme instead of the
    // light native dialog.
    setUsingNativeAlertWindows (false);

    //==============================================================================
    //  ComboBox - the device and parameter pickers live on near-black.
    setColour (juce::ComboBox::backgroundColourId,    panelBackground);
    setColour (juce::ComboBox::textColourId,          textPrimary);
    setColour (juce::ComboBox::outlineColourId,       textDim.darker (0.3f));
    setColour (juce::ComboBox::buttonColourId,        panelHighlighted);
    setColour (juce::ComboBox::arrowColourId,         textPrimary);
    setColour (juce::ComboBox::focusedOutlineColourId, accent);

    //==============================================================================
    //  Label - covers slider text boxes and any Label the cards add.
    setColour (juce::Label::backgroundColourId,            juce::Colour (0x00000000));
    setColour (juce::Label::textColourId,                  textPrimary);
    setColour (juce::Label::outlineColourId,               juce::Colour (0x00000000));
    setColour (juce::Label::backgroundWhenEditingColourId, panelHighlighted);
    setColour (juce::Label::textWhenEditingColourId,       textPrimary);
    setColour (juce::Label::outlineWhenEditingColourId,    accent);

    //==============================================================================
    //  ScrollBar - the card list and the preset browsers.
    setColour (juce::ScrollBar::backgroundColourId, windowBackground);
    setColour (juce::ScrollBar::thumbColourId,      textDim);
    setColour (juce::ScrollBar::trackColourId,      panelBackground);

    //==============================================================================
    //  AlertWindow - error dialogs from device open failures must be readable.
    setColour (juce::AlertWindow::backgroundColourId, panelBackground);
    setColour (juce::AlertWindow::textColourId,       textPrimary);
    setColour (juce::AlertWindow::outlineColourId,    textDim.darker (0.3f));

    //==============================================================================
    //  Slider - the global Dry/Wet and trim controls are plain JUCE sliders.
    setColour (juce::Slider::backgroundColourId,          panelBackground);
    setColour (juce::Slider::thumbColourId,               accent);
    setColour (juce::Slider::trackColourId,               textDim.darker (0.2f));
    setColour (juce::Slider::rotarySliderFillColourId,    accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, panelHighlighted);
    setColour (juce::Slider::textBoxTextColourId,         textPrimary);
    setColour (juce::Slider::textBoxBackgroundColourId,   panelBackground);
    setColour (juce::Slider::textBoxHighlightColourId,    accent.withAlpha (0.35f));
    setColour (juce::Slider::textBoxOutlineColourId,      textDim.darker (0.3f));

    //==============================================================================
    //  TextButton / SmallTextButton.
    setColour (juce::TextButton::buttonColourId,   panelBackground);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId,  textPrimary);
    setColour (juce::TextButton::textColourOnId,   windowBackground);

    //==============================================================================
    //  TextEditor - the looper label field and any inline editing.
    setColour (juce::TextEditor::backgroundColourId,      panelBackground);
    setColour (juce::TextEditor::textColourId,            textPrimary);
    setColour (juce::TextEditor::highlightColourId,       accent.withAlpha (0.4f));
    setColour (juce::TextEditor::highlightedTextColourId, textPrimary);
    setColour (juce::TextEditor::outlineColourId,         textDim.darker (0.3f));

    //==============================================================================
    //  Windows and tooltips.
    setColour (juce::ResizableWindow::backgroundColourId, windowBackground);
    setColour (juce::DocumentWindow::textColourId,        textPrimary);
    setColour (juce::TooltipWindow::backgroundColourId,   panelHighlighted);
    setColour (juce::TooltipWindow::textColourId,         textPrimary);
    setColour (juce::TooltipWindow::outlineColourId,      textDim);
}

DarkLookAndFeel::~DarkLookAndFeel() = default;

//==============================================================================
DarkLookAndFeel& DarkLookAndFeel::instance()
{
    // Function-local static: constructed exactly once on first call, and lives for
    // the process, which is what lets widgets call it safely from resized/paint.
    static DarkLookAndFeel laf;
    return laf;
}

juce::Font DarkLookAndFeel::makeFont (float height)
{
    // FontOptions rather than the (name, height, style) constructor, which JUCE 8
    // deprecates; Font::setHeight is gone too, so this is the only supported way to
    // build a font at a pixel height.
    //
    // The family comes from the language module. Windows substitutes a CJK-capable
    // font when the requested one has no glyph - the first-run prompt proves it,
    // drawing Chinese correctly on the default face - but the substitution is the
    // system's choice, not this app's, and it is inconsistent across the widget
    // set. Naming a face keeps 中文 in one typeface everywhere.
    const auto& uiFamily = uiFontFamily();

    return uiFamily.isEmpty()
        ? juce::Font (juce::FontOptions (height))
        : juce::Font (juce::FontOptions (uiFamily, height, juce::Font::plain));
}

//==============================================================================
juce::Font DarkLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return makeFont (16.0f);
}

juce::Font DarkLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    const auto h = static_cast<float> (buttonHeight);
    return makeFont (juce::jlimit (11.0f, 22.0f, h * 0.6f));
}

juce::Font DarkLookAndFeel::getLabelFont (juce::Label&)
{
    return makeFont (15.0f);
}

juce::Font DarkLookAndFeel::getAlertWindowMessageFont()
{
    return makeFont (16.0f);
}

} // namespace pedal
