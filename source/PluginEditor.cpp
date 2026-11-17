// HOT-STEP LINK -- PluginEditor.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Layout and painting follow mock-up B (three plates: setup, generate, queue/preview).
#include "PluginEditor.h"
#include "WavTempoTag.h"
#include "TakeStretch.h"
#include "MidiExport.h"
#include "ChordExport.h"
#include "TempoMeasure.h"

namespace
{
    const juce::Colour kBg (0xff0b0e16), kPanel (0xff111521), kAccent (0xfff472b6), kText (0xffd6dbe8);
    const juce::Colour kBright (0xfff3f5fa), kDim (0xff7c859d), kField (0xff0a0d16), kEdge (0x38a0aac8);
    const juce::Colour kAmberA (0xfffbbf24), kAmberB (0xfff59e0b), kOk (0xff86efac), kBad (0xfffca5a5);
    const juce::Colour kLav (0xffc4b5fd);
    // Text alpha of a Backend menu entry the server does not list. The entry stays an
    // active choice, so it has to stay readable: WCAG contrast 5.40:1 on the menu
    // background (0.4 gave 3.09:1; a listed entry has 13.15:1). On the highlight bar
    // the same 0.6 gives only 4.20:1, so the highlighted row uses 0.7: 5.13:1, against
    // 11.05:1 for a listed entry there. The role cards keep their own dimming.
    const float kUnlistedItemAlpha = 0.6f;
    const float kUnlistedItemAlphaOnBar = 0.7f;
}

//==============================================================================
HslinkLook::HslinkLook()
{
    setColour (juce::ResizableWindow::backgroundColourId, kBg);
    setColour (juce::Label::textColourId, kText);
    setColour (juce::TextEditor::backgroundColourId, kField);
    setColour (juce::TextEditor::textColourId, kBright);
    setColour (juce::TextEditor::highlightColourId, kAccent.withAlpha (0.35f));
    setColour (juce::TextEditor::outlineColourId, kEdge);
    setColour (juce::TextEditor::focusedOutlineColourId, kAccent.withAlpha (0.7f));
    setColour (juce::CaretComponent::caretColourId, kAccent);
    setColour (juce::ComboBox::backgroundColourId, kField);
    setColour (juce::ComboBox::textColourId, kBright);
    setColour (juce::ComboBox::outlineColourId, kEdge);
    setColour (juce::ComboBox::arrowColourId, kAccent);
    setColour (juce::PopupMenu::backgroundColourId, kPanel);
    setColour (juce::PopupMenu::textColourId, kText);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, kAccent.withAlpha (0.25f));
    setColour (juce::PopupMenu::highlightedTextColourId, kBright);
    setColour (juce::TextButton::buttonColourId, kField);
    setColour (juce::TextButton::buttonOnColourId, kAccent.withAlpha (0.18f));
    setColour (juce::TextButton::textColourOffId, kText);
    setColour (juce::TextButton::textColourOnId, kAccent);
    setColour (juce::ToggleButton::textColourId, kText);
    setColour (juce::ToggleButton::tickColourId, kAccent);
    setColour (juce::ToggleButton::tickDisabledColourId, kDim);
    setColour (juce::Slider::backgroundColourId, kField);
    setColour (juce::Slider::trackColourId, kAccent);
    setColour (juce::Slider::thumbColourId, kAccent);
    setColour (juce::Slider::textBoxTextColourId, kBright);
    setColour (juce::Slider::textBoxBackgroundColourId, kField);
    setColour (juce::Slider::textBoxOutlineColourId, kEdge);
    setColour (juce::ScrollBar::thumbColourId, kAccent.withAlpha (0.35f));
    setColour (juce::TooltipWindow::backgroundColourId, kPanel);
    setColour (juce::TooltipWindow::textColourId, kText);
    setColour (juce::TooltipWindow::outlineColourId, kEdge);
}

void HslinkLook::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();
    if (b.getComponentID() == "generate")
    {
        const float a = b.isEnabled() ? 1.0f : 0.35f;
        g.setGradientFill (juce::ColourGradient (kAmberA.withAlpha (a), r.getX(), r.getY(),
                                                 kAmberB.withAlpha (a), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 6.0f);
        if (over && b.isEnabled())
        {
            g.setColour (juce::Colours::white.withAlpha (down ? 0.18f : 0.10f));
            g.fillRoundedRectangle (r, 6.0f);
        }
        return;
    }
    auto fill = on ? kAccent.withAlpha (0.14f) : kField;
    if (over) fill = fill.brighter (0.15f);
    if (down) fill = fill.brighter (0.25f);
    if (b.getComponentID() == "zoomglyph")
    {
        // R40: the body carries the state, the glyph never dims. Enabled: lifted fill and a
        // brighter border; disabled: flat fill and a faint border.
        // C5: one body for both glyphs whatever the state (no lit pill on +).
        g.setColour (b.isEnabled() ? fill : kField);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (kEdge.withMultipliedAlpha (0.4f));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
        return;
    }
    // D1a: a pink action marked "solid" (Send to takes with a framed region) is a filled accent pill.
    if (b.getComponentID() == "pink" && b.isEnabled() && (bool) b.getProperties().getWithDefault ("solid", false))
    {
        g.setColour (over ? kAccent.brighter (down ? 0.25f : 0.12f) : kAccent);
        g.fillRoundedRectangle (r, 5.0f);
        return;
    }
    g.setColour (fill);
    g.fillRoundedRectangle (r, 5.0f);
    const bool pink = on || b.getComponentID() == "pink";
    g.setColour (pink ? kAccent.withAlpha (b.isEnabled() ? 0.7f : 0.25f) : kEdge);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
}

void HslinkLook::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    const bool gen = b.getComponentID() == "generate";
    juce::Colour c = gen ? juce::Colour (0xff1a1205)
                         : (b.getToggleState() || b.getComponentID() == "pink" ? kAccent : kText);
    if (b.getComponentID() == "pink" && b.isEnabled() && (bool) b.getProperties().getWithDefault ("solid", false))
        c = kBg; // D1a: dark text on the solid accent pill
    if (! b.isEnabled() && b.getComponentID() != "zoomglyph") // R40: the zoom - and + glyphs stay kText at full alpha
        c = gen ? c.withAlpha (0.5f) : kDim.withAlpha (0.6f);
    g.setColour (c);
    g.setFont (juce::FontOptions (gen ? 17.0f : 12.0f, juce::Font::bold));
    auto t = b.getButtonText();
    if (gen)
    {
        juce::String spaced;
        for (auto ch : t.toUpperCase())
            spaced << juce::String::charToString (ch) << " ";
        t = spaced.trimEnd();
    }
    if (b.getComponentID() == "arm")
    {
        // Red record dot left of the label (Mockup A); brighter while armed.
        const auto lb = b.getLocalBounds().toFloat();
        const float d = 8.0f, cx = lb.getCentreX() - 22.0f, cy = lb.getCentreY();
        g.setColour (juce::Colour (0xffef4444).withAlpha (b.getToggleState() ? 1.0f : 0.75f));
        g.fillEllipse (cx - d * 0.5f, cy - d * 0.5f, d, d);
        g.setColour (c);
        g.drawText (t, b.getLocalBounds().withTrimmedLeft (14).reduced (4, 0), juce::Justification::centred);
        return;
    }
    // Fitted, not ellipsed: a long label in a narrow button shrinks instead of clipping.
    // Numbox step buttons are 16 wide (inventory "Cards"): a 4 px pad left 8 px and ellipsed "+".
    const int padX = b.getWidth() < 24 ? 0 : 4;
    g.drawFittedText (t, b.getLocalBounds().reduced (padX, 0), juce::Justification::centred, 1, 0.75f);
}

float HotStepLinkEditor::moreLikeLabelWidth (const juce::String& text)
{
    // Same font the look draws button text with (12 pt bold).
    juce::GlyphArrangement ga;
    ga.addLineOfText (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)), text, 0.0f, 0.0f);
    return ga.getBoundingBox (0, -1, true).getWidth();
}

juce::String HotStepLinkEditor::moreLikeTextFor (const juce::String& takeLabel, float availWidth)
{
#if defined (HSLINK_TEST_HOOKS)
    const auto teeth = juce::SystemStats::getEnvironmentVariable ("HSLINK_TEETH", {});
    // Teeth only (HSLINK_TEETH=OLD_MORE_LIKE): the old long label that clipped in the button.
    if (teeth == "OLD_MORE_LIKE")
        return takeLabel.isEmpty() ? juce::String ("+4 MORE LIKE TAKE") : "+4 MORE LIKE " + takeLabel.toUpperCase();
#endif
    if (takeLabel.isEmpty())
        return moreLikeLabelWidth ("+4 MORE LIKE") <= availWidth ? juce::String ("+4 MORE LIKE") : juce::String ("+4 LIKE");
    auto t = takeLabel.toUpperCase();
    if (t.startsWith ("TAKE"))
        t = "T" + t.substring (4).trimStart();
    const juce::String withId = "+4 LIKE " + t;
#if defined (HSLINK_TEST_HOOKS)
    // Teeth only (HSLINK_TEETH=MORELIKE_NO_FIT): always keep the id, whatever the width.
    if (teeth == "MORELIKE_NO_FIT")
        return withId;
#endif
    if (moreLikeLabelWidth (withId) <= availWidth)
        return withId;
    // The id does not fit at the button's real width: drop it rather than clip.
    return moreLikeLabelWidth ("+4 MORE LIKE") <= availWidth ? juce::String ("+4 MORE LIKE") : juce::String ("+4 LIKE");
}

void HslinkLook::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
    g.setColour (kField);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (box.hasKeyboardFocus (true) ? kAccent.withAlpha (0.7f) : kEdge);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
    juce::Path p;
    const float cx = (float) w - 12.0f, cy = (float) h * 0.5f;
    p.addTriangle (cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
    g.setColour (kAccent.withAlpha (box.isEnabled() ? 1.0f : 0.3f));
    g.fillPath (p);
}

void HslinkLook::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    // C2: the text runs up to the arrow (the default margin cut short values to '...').
    label.setBounds (1, 1, juce::jmax (0, box.getWidth() - 17), box.getHeight() - 2); // C3d: arrow spans w-16..w-8
    label.setBorderSize (juce::BorderSize<int> (0, 4, 0, 0));
    label.setFont (getComboBoxFont (box));
    label.setMinimumHorizontalScale (1.0f);
}

void HslinkLook::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (kField);
    g.fillRoundedRectangle (0.0f, 0.0f, (float) w, (float) h, 4.0f);
}

void HslinkLook::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    if (e.isColourSpecified (juce::TextEditor::outlineColourId))
    {
        const auto c = e.findColour (juce::TextEditor::outlineColourId);
        if (c != kEdge)
        {
            g.setColour (c);
            g.drawRoundedRectangle (1.0f, 1.0f, (float) w - 2.0f, (float) h - 2.0f, 4.0f, 2.0f);
            return;
        }
    }
    g.setColour (e.hasKeyboardFocus (true) ? kAccent.withAlpha (0.7f) : kEdge);
    g.drawRoundedRectangle (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f, 4.0f, 1.0f);
}

void HslinkLook::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                   juce::Slider::SliderStyle style, juce::Slider&)
{
    if (style == juce::Slider::LinearBar || style == juce::Slider::LinearBarVertical)
        return; // C2: bar sliders are LCD value boxes, the text alone shows the value
    const float cy = (float) y + (float) h * 0.5f;
    auto track = juce::Rectangle<float> ((float) x, cy - 2.0f, (float) w, 4.0f);
    g.setColour (kField.brighter (0.25f));
    g.fillRoundedRectangle (track, 2.0f);
    g.setColour (kAccent);
    g.fillRoundedRectangle (track.withRight (pos), 2.0f);
    g.setColour (kBright);
    g.fillEllipse (pos - 5.0f, cy - 5.0f, 10.0f, 10.0f);
    g.setColour (kAccent);
    g.drawEllipse (pos - 5.0f, cy - 5.0f, 10.0f, 10.0f, 1.5f);
}

// Mockup A knob: dark body, dim 270 deg track, accent value arc, bright pointer.
void HslinkLook::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                   float a0, float a1, juce::Slider&)
{
    const float d = (float) juce::jmin (w, h) - 6.0f;
    if (d < 8.0f)
        return;
    const auto c = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).getCentre();
    const float rad = d * 0.5f, ang = a0 + pos * (a1 - a0);
    juce::Path track, value;
    track.addCentredArc (c.x, c.y, rad, rad, 0.0f, a0, a1, true);
    g.setColour (kField.brighter (0.35f));
    g.strokePath (track, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (pos > 0.0f)
    {
        value.addCentredArc (c.x, c.y, rad, rad, 0.0f, a0, ang, true);
        g.setColour (kAccent);
        g.strokePath (value, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    const float br = rad - 7.0f;
    g.setColour (juce::Colour (0xff1a1f2e));
    g.fillEllipse (c.x - br, c.y - br, br * 2.0f, br * 2.0f);
    g.setColour (kEdge);
    g.drawEllipse (c.x - br, c.y - br, br * 2.0f, br * 2.0f, 1.0f);
    const auto tip = c.getPointOnCircumference (br - 3.0f, ang);
    const auto base = c.getPointOnCircumference (br * 0.35f, ang);
    g.setColour (kBright);
    g.drawLine ({ base, tip }, 2.0f);
}

void HslinkLook::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                    bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                    const juce::String& shortcut, const juce::Drawable* icon, const juce::Colour* textColour)
{
    juce::Colour onBar;
    if (textColour != nullptr && isHighlighted && isActive && ! isSeparator)
    {
        // Same highlight bar as the base class, but the item's own colour stays. A
        // marked Backend entry gets its highlight-bar alpha (the bar is brighter than
        // the menu background, so the same alpha would read below 4.5:1 there).
        g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
        g.fillRect (area.reduced (1));
        isHighlighted = false;
        bool lift = *textColour == unlistedItemColour (false);
#if defined (HSLINK_TEST_HOOKS)
        lift = lift && ! testNoHighlightLift; // teeth: the code before
#endif
        if (lift)
        {
            onBar = unlistedItemColour (true);
            textColour = &onBar;
        }
    }
    juce::LookAndFeel_V4::drawPopupMenuItem (g, area, isSeparator, isActive, isHighlighted, isTicked, hasSubMenu,
                                             text, shortcut, icon, textColour);
}

juce::Colour HslinkLook::unlistedItemColour (bool onHighlightBar)
{
    return kText.withAlpha (onHighlightBar ? kUnlistedItemAlphaOnBar : kUnlistedItemAlpha);
}

juce::Font HslinkLook::getLabelFont (juce::Label& l)
{
    return l.getFont();
}

HotStepLinkEditor::HotStepLinkEditor (HotStepLinkProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      standalone (juce::JUCEApplicationBase::isStandaloneApp()),
      vblank (this, [this] { tick ((++vblankTick % 10) == 0); })
{
    clearMidiDragTempRoot (midiDragTempRoot()); // drag-out MIDI files left by earlier sessions
    setLookAndFeel (&look);
    titleLabel.setText ("HOT-STEP LINK", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (21.0f, juce::Font::bold));
    titleLabel.setColour (juce::Label::textColourId, kAccent);
    addChildComponent (titleLabel); // header B: the wordmark is painted (Beat Grid)
    addAndMakeVisible (headerBars); // animated beat-grid bars, repainted alone by the tick

    versionLabel.setText (HSLINK_VERSION, juce::dontSendNotification);
    versionLabel.setJustificationType (juce::Justification::centredRight);
    versionLabel.setColour (juce::Label::textColourId, kDim);
    versionLabel.setFont (juce::FontOptions (11.0f));
    versionLabel.setText ("HOT-STEP LINK " + juce::String (HSLINK_VERSION) + (standalone ? " standalone" : " plugin"),
                          juce::dontSendNotification);
    addAndMakeVisible (versionLabel);

    syncLabel.setColour (juce::Label::textColourId, kText);
    syncLabel.setFont (juce::FontOptions (11.0f));
    syncLabel.setColour (juce::Label::textColourId, kDim.brighter (0.2f));
    addAndMakeVisible (syncLabel);
    exposureLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    exposureLabel.setColour (juce::Label::textColourId, kAmberA);
    exposureLabel.setJustificationType (juce::Justification::centredRight);
    addChildComponent (exposureLabel);

    auto caption = [this] (juce::Label& l, const char* t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, kDim);
        l.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        addAndMakeVisible (l);
    };
    caption (urlCaption, "Server URL");
    caption (keyCaption, "DAW key");
    caption (backendCaption, "Backend");
    caption (aceModelCaption, "ACE model");
    caption (durationCaption, "Duration (s)");
    caption (takesCaption, "Takes");
    caption (variationCaption, "Variation");
    caption (guidanceCaption, "Guidance");
    caption (stepsCaption, "Steps");
    caption (bpmCaption, "BPM");
    caption (modeCaption, "Mode / strength");
    caption (captionCaption, "Caption");

    captionEditor.setTextToShowWhenEmpty ("describe the music", kText.withAlpha (0.4f));
    captionEditor.onTextChange = [this] { proc.setCaption (captionEditor.getText()); };
    addAndMakeVisible (captionEditor);
    captionEditor.setText (proc.getCaption(), false); // after parenting: the text takes the theme colour, not the default look's

    caption (lyricsCaption, "Lyrics");
    lyricsEditor.setMultiLine (true, true);
    lyricsEditor.setReturnKeyStartsNewLine (true);
    lyricsEditor.setScrollbarsShown (true);
    lyricsEditor.setTextToShowWhenEmpty ("optional, e.g. [verse] ... (empty = instrumental)", kText.withAlpha (0.4f));
    lyricsEditor.onTextChange = [this] { proc.setLyrics (lyricsEditor.getText()); };
    addAndMakeVisible (lyricsEditor);
    lyricsEditor.setText (proc.getLyrics(), false); // after parenting (theme text colour)
    instrumentalButton.setToggleState (proc.getInstrumental(), juce::dontSendNotification);
    lyricsEditor.setEnabled (! proc.getInstrumental());
    instrumentalButton.onClick = [this]
    {
        const bool on = instrumentalButton.getToggleState();
        proc.setInstrumental (on);
        lyricsEditor.setEnabled (! on);
    };
    addAndMakeVisible (instrumentalButton);
    generateButton.setComponentID ("generate");
    cancelButton.setComponentID ("pink");
    exportWavButton.setComponentID ("pink");
    generateButton.onClick = [this] { startJob(); };
    cancelButton.onClick = [this] { if (currentJob != 0) proc.daw.cancelJob (currentJob); };
    queueList.onRemove = [this] (int id) { proc.daw.removeQueued (id); refreshJob(); };
    queueList.onSelect = [this] (int id)
    {
        selectedJob = (selectedJob == id) ? 0 : id;
        refreshQueue();
        refreshTakes();
    };
    addAndMakeVisible (queueList);
    cancelButton.setEnabled (false);
    addAndMakeVisible (generateButton);
    addAndMakeVisible (cancelButton);
    jobLabel.setColour (juce::Label::textColourId, kText);
    jobLabel.setFont (juce::FontOptions (11.0f));
    jobLabel.setJustificationType (juce::Justification::topLeft);
    jobLabel.setText ("Idle", juce::dontSendNotification);
    addAndMakeVisible (jobLabel);

    modeBox.addItem ("text2music", 1);
    modeBox.addItem ("repaint", 2);
    modeBox.addItem ("lego", 3);
    modeBox.setSelectedId (1, juce::dontSendNotification);
    addAndMakeVisible (modeBox);
    strengthEditor.setInputRestrictions (4, "0123456789.");
    strengthEditor.setJustification (juce::Justification::centred);
    strengthEditor.setText (juce::String (proc.getCoverStrength(), 2), false);
    strengthEditor.setTooltip ("Reference strength (cover_strength), 0.0 to 1.0. ACE-Step only.");
    {
        auto commit = [this]
        {
            const double v = juce::jlimit (0.0, 1.0, strengthEditor.getText().getDoubleValue());
            proc.setCoverStrength (v);
            strengthEditor.setText (juce::String (v, 2), false);
        };
        strengthEditor.onReturnKey = commit;
        strengthEditor.onFocusLost = commit;
    }
    addAndMakeVisible (strengthEditor);
    refLockNote.setText ("pick a backend to use reference audio", juce::dontSendNotification);
    refLockNote.setFont (juce::FontOptions (10.0f));
    refLockNote.setJustificationType (juce::Justification::centredRight);
    refLockNote.setMinimumHorizontalScale (0.6f);
    // kDim measured 4.19:1 from pixels at this size on the popover plate; 15 % toward kText reads 4.5:1 or better.
    refLockNote.setColour (juce::Label::textColourId, kDim.interpolatedWith (kText, 0.15f));
    addChildComponent (refLockNote);
    // Same pattern as the Mode-row note: right-aligned in the "DAW key" caption row, warning colour.
    keySaveNote.setFont (juce::FontOptions (10.0f));
    keySaveNote.setJustificationType (juce::Justification::centredRight);
    keySaveNote.setMinimumHorizontalScale (1.0f); // never squeezed: the text is chosen to fit (keySaveNoteTextFor)
    keySaveNote.setBorderSize (juce::BorderSize<int> (1, 0, 1, 5));
    keySaveNote.setColour (juce::Label::textColourId, kAmberA);
    keySaveNote.setInterceptsMouseClicks (false, false);
    addChildComponent (keySaveNote);

    serverLabel.setColour (juce::Label::textColourId, kBright);
    serverLabel.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    serverLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (serverLabel);

    overlay.retryButton.onClick = [this] { proc.daw.retryNow(); };
    addChildComponent (overlay);

    // Take editor: the panel holds no processor, these are its only ways out.
    takeEditor.onSetEdit = [this] (const juce::File& temp, const TakeEdit::EditData& e) { return proc.setTakeEdit (temp, e); };
    takeEditor.onSetGrid = [this] (const juce::File& temp, const TakeEdit::GridEdit& g) { return proc.setTakeGrid (temp, g); };
    takeEditor.onSetWarp = [this] (const juce::File& temp, const std::vector<WarpMarkers::Marker>& m, bool ed, double tag) { return proc.setTakeWarp (temp, m, ed, tag); };
    takeEditor.onApply = [this] (const juce::File& temp, const TakeEdit::EditData& e, const TakeEdit::EditOptions& o, juce::String& whyNot)
    {
        return proc.requestTakeEdit (temp, e, o, &whyNot);
    };
    takeEditor.onReport = [this] (juce::uint64 ticket, HotStepLinkProcessor::TakeEditReport& r) { return proc.getTakeEditReport (ticket, r); };
    takeEditor.onJobState = [this] (juce::uint64 ticket) { return proc.getTakeEditState (ticket); };
    takeEditor.onClosed = [this]
    {
        takeEditor.setVisible (false);
        proc.editorAudition.dropCache(); // the decoded take of the audition is not kept past the panel
    };
    // Fit to bars: the tempo field starts from the host tempo (the manual one without a host).
    takeEditor.onHostBpm = [this] { return fitHostBpm (standalone, proc.hostSync, proc.getManualBpm()); };
    // Warp to host: the real host tempo only (B4). The manual tempo is not a host tempo.
    takeEditor.onWarpHostBpm = [this]
    {
        if (TakeEditorMath::teMutant (TakeEditorMath::mutWarpResetsNoHost))
            return fitHostBpm (standalone, proc.hostSync, proc.getManualBpm()); // teeth: the B3 bug
        return (standalone || ! proc.hostSync.hostSyncActive.load (std::memory_order_relaxed)) ? 0.0 : proc.hostSync.tempoForCascade();
    };
    // Stretch to fit: the take the Apply made, looked up by its name now (a copy of the list, no disk IO).
    takeEditor.onStretch = [this] (const juce::String& name, double fromBpm, double toBpm)
    {
        for (const auto& e : proc.getTakes())
            if (e.name == name)
                return proc.requestTakeStretch (e.temp, fromBpm, toBpm);
        return false;
    };
    // Audition of the take editor: its own engine, independent of the host transport.
    takeEditor.onAuditionPlay = [this] (const juce::File& temp, juce::int64 start, juce::int64 end, juce::int64 length, bool loop)
    {
        proc.editorAudition.play (temp, start, end, length, loop);
    };
    takeEditor.onAuditionWarp = [this] (const std::vector<WarpMarkers::Marker>& m) { proc.editorAudition.setWarp (m); };
    takeEditor.onAuditionStop = [this] { proc.editorAudition.stop(); };
    takeEditor.onAuditionLevel = [this] (juce::int64 s, juce::int64 e, juce::int64 n, const TakeEdit::EditData& d)
    {
        proc.editorAudition.setLevel (s, e, n, d.fadeInMs, d.fadeOutMs, d.loopXfadeMs, d.gainDb, d.reverse);
    };
    takeEditor.onAuditionLoop = [this] (bool on) { proc.editorAudition.setLoop (on); };
    takeEditor.onAuditionClick = [this] (bool on, double bpm, double downbeat) { proc.editorAudition.setClick (on, bpm, downbeat); };
    takeEditor.onAuditionRegion = [this] (juce::int64 s, juce::int64 e, juce::int64 n) { proc.editorAudition.setRegion (s, e, n); };
    takeEditor.onAuditionState = [this] { return (int) proc.editorAudition.getState(); };
    takeEditor.onAuditionPosition = [this] { return proc.editorAudition.getPlayFraction(); };
    addChildComponent (takeEditor);
    editTakeButton.setTooltip ("Open the take editor for the selected take: waveform and crop");
    editTakeButton.onClick = [this] { openTakeEditor(); };
    editTakeButton.setEnabled (false);
    addAndMakeVisible (editTakeButton);

    // Check and save on Return or focus loss only; a bad value keeps the saved URL.
    urlEditor.onReturnKey = [this] { commitServerUrl(); };
    urlEditor.onFocusLost = [this] { commitServerUrl(); };
    if (HotStepLinkProcessor::serverUrlOverride().isNotEmpty())
    {
        // HSLINK_SERVER_URL is set: show the override read-only so typing cannot change state.
        urlEditor.setReadOnly (true);
        urlEditor.setEnabled (false);
        urlEditor.setTooltip ("Server URL set by HSLINK_SERVER_URL (launch override, not saved)");
    }
    addAndMakeVisible (urlEditor);
    // Text is set after parenting: a TextEditor colours text when it is inserted, and before
    // it has a parent it only sees the default look and feel (white), not the theme's kBright.
    urlEditor.setText (proc.getServerUrl(), false);

    keyEditor.setPasswordCharacter ((juce::juce_wchar) '*');
    keyEditor.onTextChange = [this]
    {
        bool saveTried = false;
        const bool stored = proc.setDawKey (keyEditor.getText().trim(), &saveTried);
        noteKeySave (saveTried, stored);
        updateExposureWarning();
    };
#if defined (HSLINK_TEST_HOOKS)
    if (testKeyRevealMutant == 4) // teeth 4: the old order, text before the field has a parent
        keyEditor.setText (proc.getDawKey(), false);
#endif
    addAndMakeVisible (keyEditor);
    keyEditor.setText (proc.getDawKey(), false); // after parenting (theme text colour), no notification, no save
    keyRevealButton.onClick = [this] { setKeyRevealed (! keyRevealed); };
    addAndMakeVisible (keyRevealButton);
    setKeyRevealed (false);
    updateExposureWarning();

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("backend")))
        backendBox.addItemList (choice->choices, 1);
    addAndMakeVisible (backendBox);
    backendAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "backend", backendBox);
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("aceModel")))
        aceModelBox.addItemList (choice->choices, 1);
    addAndMakeVisible (aceModelBox);
    aceModelAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "aceModel", aceModelBox);

    for (auto* s : { &durationSlider, &takesSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 54, 22);
        addAndMakeVisible (*s);
    }
    durationAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "duration", durationSlider);
    takesAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "takes", takesSlider);
    // Every backend change (combo, role tile, preset, automation) re-applies the Takes limit.
    proc.apvts.addParameterListener ("backend", this);
    applyTakesLimit (false);
    applyIgnoredSettings();
    applyBackendRefLock();
    // Knob row (Mockup A): GUIDANCE, STEPS, VARIATION as rotaries, value readout below.
    for (auto* s : { &variationKnob, &guidanceKnob, &stepsKnob })
    {
        s->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s->setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
        s->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        s->setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        s->setColour (juce::Slider::textBoxTextColourId, kBright);
    }
    // VARIATION: 0 = every generate sends the same seed.
    variationKnob.setTooltip ("Seed spread between repeat generates and \"+4 more like\" (0 = the same seed every time)");
    addAndMakeVisible (variationKnob);
    variationAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "variation", variationKnob);
    // GUIDANCE / STEPS: leftmost = Auto (the server picks; no field sent).
    for (auto* s : { &guidanceKnob, &stepsKnob })
        addAndMakeVisible (*s);
    guidanceKnob.setTooltip ("Guidance scale 0..20 (far left = Auto, the server default)");
    stepsKnob.setTooltip ("Diffusion steps 1..300 (far left = Auto, the server default)");
    guidanceAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "guidance", guidanceKnob);
    stepsAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "steps", stepsKnob);

    // Standalone: host sync is off, BPM is manual (app-owned, not automated).
    manualBpm.setRange (40.0, 240.0, 0.1);
    manualBpm.setValue (120.0, juce::dontSendNotification);
    manualBpm.setSliderStyle (juce::Slider::LinearHorizontal);
    manualBpm.setTextBoxStyle (juce::Slider::TextBoxRight, false, 78, 22);
    manualBpm.setTextValueSuffix (" BPM");
    addChildComponent (manualBpm);
    manualBpm.setVisible (standalone);
    bpmCaption.setVisible (standalone);

    // Seed (blank or Random = random) and key (Auto = omit).
    caption (seedCaption, "Seed");
    caption (songKeyCaption, "Key");
    seedEditor.setInputRestrictions (SeedInput::kMaxDigits, "0123456789");
    seedEditor.setTextToShowWhenEmpty ("random", kText.withAlpha (0.4f));
    seedEditor.onReturnKey = [this] { commitSeed(); };
    seedEditor.onFocusLost = [this] { commitSeed(); };
    // The error outline goes as soon as the text is a valid seed or empty; the seed itself
    // is committed on Return, focus-lost or Generate as before.
    seedEditor.onTextChange = [this]
    {
        if (seedRefused && SeedInput::parse (seedEditor.getText().trim()).ok)
            setSeedRefused (false);
    };
    addAndMakeVisible (seedEditor);
    seedRandomButton.setTooltip ("Random seed each generate (the seed is not sent)");
    seedRandomButton.onClick = [this]
    {
        if (seedRandomButton.getToggleState())
            proc.setSeed (-1);
        else if (seedEditor.getText().trim().isEmpty())
            proc.setSeed ((juce::int64) (diceRng.nextInt64() & 0x7fffffff));
        else
        {
            commitSeed(); // refreshes the field itself, or keeps a refused text
            return;
        }
        refreshSeedUi();
    };
    addAndMakeVisible (seedRandomButton);
    diceButton.setTooltip ("Roll a new fixed seed");
    diceButton.onClick = [this]
    {
        proc.setSeed ((juce::int64) (diceRng.nextInt64() & 0x7fffffff));
        refreshSeedUi();
    };
    addAndMakeVisible (diceButton);
    songKeyBox.addItem ("Auto", 1);
    for (int i = 1; i <= HotStepLinkProcessor::kNumKeys; ++i)
        songKeyBox.addItem (HotStepLinkProcessor::keyName (i), i + 1);
    songKeyBox.setSelectedId (proc.getKeyIndex() + 1, juce::dontSendNotification);
    songKeyBox.onChange = [this] { proc.setKeyIndex (songKeyBox.getSelectedId() - 1); };
    addAndMakeVisible (songKeyBox);
    caption (peakModeCaption, "Level");
    peakModeBox.addItem ("Off", PeakGuard::kOff + 1);
    peakModeBox.addItem ("Peak guard", PeakGuard::kGuard + 1);
    peakModeBox.addItem ("Normalise -1 dBTP", PeakGuard::kNormalise + 1);
    peakModeBox.setTooltip ("New takes on import and Export WAV: Off = untouched; Peak guard = cut to -1 dBTP only; Normalise = gain to -1 dBTP (may boost)");
    peakModeBox.setSelectedId (proc.getPeakMode() + 1, juce::dontSendNotification);
    peakModeBox.onChange = [this] { proc.setPeakMode (peakModeBox.getSelectedId() - 1); };
    addAndMakeVisible (peakModeBox);
    refreshSeedUi();

    // R5 take list + audition
    // Cells name their take by key; the file is looked up from the entry at the action.
    takeList.onSelect = [this] (const juce::String& key) { selectTake (key); };
    takeList.dragFileFor = [this] (const juce::String& key) { return dragFileFor (key); };
    takeList.onAdd = [this] (const juce::String& key)
    {
        const auto model = buildTakeModel();
        const auto* c = model.find (key);
        const auto r = proc.addTakeToCollection (c != nullptr ? c->stable : juce::File()); // {} = no lasting file yet
        collectionPanel.showNote (r.ok ? "Added to Collection: " + r.item.name : "Not added: " + r.error, ! r.ok);
    };
    takeList.onStar = [this] (const juce::String& key, bool on)
    {
        const auto model = buildTakeModel();
        if (const auto* c = model.find (key))
            proc.setStarred (c->file, on); // stars are stored by take name
    };
    takeList.onDelete = [this] (const juce::String& tempPath) { deleteTake (tempPath); };
    takeList.onLoadSlot = [this] (const juce::String& key, int slot) { loadTakeIntoSlot (key, slot); };
    moreLikeButton.setTooltip ("Four new takes near the selected take (seeds just past its seed)");
    moreLikeButton.onClick = [this] { if (moreLikeKey.isNotEmpty()) startMoreLike (moreLikeKey); };
    moreLikeButton.setEnabled (false);
    addAndMakeVisible (moreLikeButton);
    clearUnstarredButton.setTooltip ("Move every unstarred take to the Deleted folder (one Undo brings them all back)");
    clearUnstarredButton.onClick = [this]
    {
        if (proc.clearUnstarredTakes() == 0)
            collectionPanel.showNote ("Nothing to clear: every take is starred", false);
        refreshTakes();
    };
    addAndMakeVisible (clearUnstarredButton);
    deleteNotice.setFont (juce::FontOptions (10.5f));
    deleteNotice.setJustificationType (juce::Justification::centredLeft);
    deleteNotice.setMinimumHorizontalScale (1.0f);
    deleteNotice.setColour (juce::Label::textColourId, kText);
    addChildComponent (deleteNotice);
    undoDeleteButton.setTooltip ("Put the deleted take back in the list");
    undoDeleteButton.onClick = [this] { undoDelete(); };
    addChildComponent (undoDeleteButton);
    saveNotice.setFont (juce::FontOptions (10.5f));
    saveNotice.setJustificationType (juce::Justification::centredLeft);
    saveNotice.setMinimumHorizontalScale (1.0f);
    addChildComponent (saveNotice);
    retrySaveButton.setTooltip ("Try again now to save the takes that could not be saved");
    retrySaveButton.onClick = [this] { proc.requestSaveRetry(); }; // runs on the saver thread
    addChildComponent (retrySaveButton);
    exportWavButton.setTooltip ("Save a copy of the selected take as WAV");
    exportWavButton.onClick = [this] { exportWav(); };
    addAndMakeVisible (exportWavButton);
    exportMidiButton.setEnabled (false);
    exportMidiButton.setTooltip ("Write the parts picked in the MIDI picker (v) beside the selected take: drums (one GM drum note per "
                                 "detected hit), melody (tonal takes), chords (with the key), a tempo map (bar markers), "
                                 "or all of them as one multitrack file");
    exportMidiMenuButton.setTooltip ("Pick what Export MIDI and the MIDI drag chip write");
    exportMidiMenuButton.setClickingTogglesState (false);
    exportMidiMenuButton.onClick = [this] { setMidiPickerOpen (! midiPicker.isVisible()); };
    addAndMakeVisible (exportMidiMenuButton);
    midiPicker.closeButton.onClick = [this] { setMidiPickerOpen (false); };
    addChildComponent (midiPicker);
    exportMidiButton.onClick = [this] { exportMidi(); };
    quantizeBox.addItem ("Q Off", 1);
    quantizeBox.addItem ("Q 1/16", 2);
    quantizeBox.addItem ("Q 1/8", 3);
    quantizeBox.setSelectedId (1, juce::dontSendNotification);
    quantizeBox.setTooltip ("Quantize the exported hits to the take's grid");
    addAndMakeVisible (quantizeBox);
    quantStrengthSlider.setRange (0.0, 100.0, 1.0);
    quantStrengthSlider.setValue (100.0, juce::dontSendNotification);
    quantStrengthSlider.setTextValueSuffix (" %");
    quantStrengthSlider.setTooltip ("Quantize strength: how far each hit moves toward the grid");
    addAndMakeVisible (quantStrengthSlider);
    exportPathLabel.setMinimumHorizontalScale (0.6f);
    exportPathLabel.setJustificationType (juce::Justification::centredLeft);
    addChildComponent (exportPathLabel);
    showExportButton.setTooltip ("Open the folder the last export was written to");
    showExportButton.onClick = [this] { if (lastExportFile.exists()) lastExportFile.revealToUser(); };
    addChildComponent (showExportButton);
    addAndMakeVisible (exportMidiButton);
    midiDrag.setTooltip ("Drag the selected take's MIDI files (the parts picked in the MIDI picker) into the DAW. "
                         "Built on the fly when Export MIDI has not been run.");
    midiDrag.filesFor = [this] { return midiFilesForSelection(); };
    midiDrag.requestBuild = [this] { return buildMidiForDrag(); };
    addAndMakeVisible (midiDrag);
    addAndMakeVisible (takeList);
    levelMatchButton.setTooltip ("Match each take to the captured source RMS (or -18 dBFS RMS)");
    addAndMakeVisible (levelMatchButton);
    levelMatchAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "levelMatch", levelMatchButton);
    gainLabel.setColour (juce::Label::textColourId, kDim);
    gainLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (gainLabel);
    playButton.setClickingTogglesState (true);
    playButton.setToggleState (proc.previewPlay.load(), juce::dontSendNotification);
    playButton.setButtonText (playButton.getToggleState() ? "Stop" : "Play");
    playButton.onClick = [this]
    {
        const bool on = playButton.getToggleState();
        if (on)
            proc.previewRestart.store (true);
        proc.previewPlay.store (on);
        playButton.setButtonText (on ? "Stop" : "Play");
    };
    addChildComponent (playButton);
    playButton.setVisible (true);
    followHostButton.setTooltip ("Preview follows the host transport (silent while the host is stopped)");
    addAndMakeVisible (followHostButton);
    followHostAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "followHost", followHostButton);

    // Source clip for repaint / lego.
    caption (sourceCaption, "Source");
    caption (regionCaption, "Region (s)");
    sourceLabel.setColour (juce::Label::textColourId, kText);
    sourceLabel.setFont (juce::FontOptions (11.5f));
    addAndMakeVisible (sourceLabel);
    captureButton.setClickingTogglesState (true);
    captureButton.setTooltip ("Arm: capture starts when the host plays inside the loop (or at once with no loop)"
                              " and stops at the loop end or transport stop");
    captureButton.onClick = [this]
    {
        if (! proc.armCapture (captureButton.getToggleState()))
            captureButton.setToggleState (false, juce::dontSendNotification);
        refreshSource();
    };
    captureButton.setButtonText ("ARM");
    captureButton.setComponentID ("arm");
    addChildComponent (captureButton);
    captureButton.setVisible (! standalone);
    captureNowButton.setComponentID ("pink");
    captureNowButton.onClick = [this]
    {
        if (proc.isRecordingSource())
            proc.armCapture (false); // stop now, keep what was recorded
        refreshSource();
    };
    addAndMakeVisible (captureNowButton);
    sourceCaption.setVisible (false); // the SOURCE panel header is painted
    refHintSeen = HotStepLinkProcessor::loadRefHintSeen();
    sourceWave.setRefHint (! refHintSeen, standalone);
    sourceWave.onRefHintDismiss = [this] { dismissRefHint(); };
    sourceWave.setTooltip (refHintSeen ? "Captured source clip (repaint / lego)"
                                       : "Captured source clip (repaint / lego). Click the hint to hide it");
    addAndMakeVisible (sourceWave);
    dropButton.setTooltip ("Pick a WAV as the repaint / lego source, or drop one on the window");
    dropButton.onClick = [this]
    {
        sourceChooser = std::make_unique<juce::FileChooser> ("Source WAV", juce::File(), "*.wav");
        sourceChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                    [this] (const juce::FileChooser& fc) { useSourceFile (fc.getResult()); });
    };
    addChildComponent (dropButton);
    dropButton.setVisible (standalone);
    addTakeButton.setTooltip ("List a WAV as a take (edit, audition, export, drag out; no server needed). "
                              "Or drop a WAV on the TAKES list");
    addTakeButton.onClick = [this]
    {
        // One chooser at a time: a click while it is open is ignored (it opened twice before).
        if (addTakeChooser != nullptr)
            return;
        // Owned by this window and brought to the front, so it never opens behind the app.
        if (auto* top = getTopLevelComponent())
            top->toFront (true);
        addTakeChooser = std::make_unique<juce::FileChooser> ("Add WAV as take", juce::File(), "*.wav;*.aif;*.aiff;*.flac",
                                                              true, false, this);
        juce::Component::SafePointer<HotStepLinkEditor> safeThis (this);
        addTakeChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                     [this, safeThis] (const juce::FileChooser& fc)
                                     {
                                         const auto picked = fc.getResult();
                                         // Freed after the callback returns, never inside it.
                                         juce::MessageManager::callAsync ([safeThis]
                                                                          { if (safeThis != nullptr) safeThis->addTakeChooser.reset(); });
                                         addTakeFromFile (picked);
                                     });
    };
    addChildComponent (addTakeButton);
    addTakeButton.setVisible (addTakeAllowed()); // all formats (VST3, CLAP, Standalone)
    for (auto* e : { &regionStartEditor, &regionEndEditor })
    {
        e->setInputRestrictions (10, "0123456789.");
        e->onTextChange = [this] { refreshJob(); };
        addChildComponent (*e);
        e->setVisible (standalone);
    }
    regionStartEditor.setTextToShowWhenEmpty ("start", kText.withAlpha (0.4f));
    regionEndEditor.setTextToShowWhenEmpty ("end", kText.withAlpha (0.4f));
    regionInfo.setColour (juce::Label::textColourId, kDim);
    regionInfo.setFont (juce::FontOptions (10.5f));
    regionInfo.setJustificationType (juce::Justification::topLeft);
    regionInfo.setText (standalone ? "repaint region, seconds into the source"
                                   : "repaint region = host loop, relative to the capture start",
                        juce::dontSendNotification);
    addAndMakeVisible (regionInfo);
    modeBox.onChange = [this] { proc.setModeId (modeBox.getSelectedId()); if (proc.getSelectedBackendId() == "ace") proc.setAceModeId (modeBox.getSelectedId()); refreshJob(); };
    manualBpm.onValueChange = [this] { proc.setManualBpm (manualBpm.getValue()); };

    // Presets + A/B
    presetBox.setEditableText (true);
    presetBox.setTextWhenNothingSelected ("Preset name");
    presetBox.setTooltip ("Pick a preset to load it into the active slot; type a name and press Save to store the current settings");
    presetBox.onChange = [this]
    {
        if (presetBox.getSelectedId() > 0)
        {
            commitSeed();
            proc.loadPreset (presetBox.getText());
        }
    };
    presetSaveButton.setTooltip ("Save the current settings as a preset");
    presetSaveButton.onClick = [this]
    {
        commitSeed();
        const auto name = HotStepLinkProcessor::sanitizePresetName (presetBox.getText());
        if (name.isNotEmpty() && proc.savePreset (name))
            refreshPresetList (name);
    };
    reanalyzeButton.setTooltip ("Run the understand job on the current source (capture or dropped WAV). "
                                "Fills KEY, TEMPO EST. and tags only; Generate does not use them");
    reanalyzeButton.onClick = [this]
    {
        const auto f = proc.getSourceFile();
        if (f.existsAsFile())
            proc.daw.submitUnderstand (f);
    };
    addAndMakeVisible (reanalyzeButton);
    presetDeleteButton.setTooltip ("Delete the preset named in the box");
    presetDeleteButton.onClick = [this]
    {
        if (proc.deletePreset (presetBox.getText()))
            refreshPresetList ({});
    };
    slotAButton.setTooltip ("Settings slot A");
    slotBButton.setTooltip ("Settings slot B");
    copyAbButton.setTooltip ("Copy the active slot into the other slot");
    slotAButton.setClickingTogglesState (false);
    slotBButton.setClickingTogglesState (false);
    slotAButton.onClick = [this] { switchSlot (0); };
    slotBButton.onClick = [this] { switchSlot (1); };
    copyAbButton.onClick = [this] { commitSeed(); proc.copyActiveToOther(); };
    for (auto* c : std::initializer_list<juce::Component*> { &presetBox, &presetSaveButton, &presetDeleteButton,
                                                             &slotAButton, &slotBButton, &copyAbButton })
        addAndMakeVisible (*c);
    refreshPresetList ({});
    refreshSlotButtons();

    roleCards.onPick = [this] (int card)
    {
        proc.applyRoleCard (card);
        roleCards.setActive (proc.getActiveRoleCard());
    };
    roleCards.setActive (proc.getActiveRoleCard());
    addAndMakeVisible (roleCards);

    // Collection panel. The first scan runs on the processor's background thread;
    // the VBlank tick picks the list up when the generation counter moves.
    collectionPanel.onStar = [this] (const CollectionItem& it, bool on)
    {
        if (! proc.setCollectionStarred (it, on))
            collectionPanel.showNote ("Could not change the star of " + it.name, true);
    };
    collectionPanel.onRemove = [this] (const CollectionItem& it) { confirmRemoveFromCollection (it); };
    collectionPanel.onSelect = [this] (const CollectionItem& it)
    {
        // Same audition path as a take cell; clicking the selected row again turns it off.
        const bool off = it.wav == proc.audition.getSelected();
        proc.audition.select (off ? juce::File() : it.wav, proc.getSourceFile());
        refreshTakes();
        collectionPanel.setSelectedFile (proc.audition.getSelected());
    };
    addAndMakeVisible (collectionPanel);
#if defined (HSLINK_TEST_HOOKS)
    if (standalone)
    {
        // Test hook (standalone): HSLINK_COLLECTION_FILTER = sa3 / yue2 / ace / starred / text:<words>.
        const auto hook = juce::SystemStats::getEnvironmentVariable ("HSLINK_COLLECTION_FILTER", {});
        CollectionChips c;
        if (hook == "sa3")  c.backend = CollectionChips::sa3;
        if (hook == "yue2") c.backend = CollectionChips::yue2;
        if (hook == "ace")  c.backend = CollectionChips::ace;
        if (hook == "minimax-m3") c.backend = CollectionChips::minimax;
        if (hook == "heartmula")  c.backend = CollectionChips::heartmula;
        c.starredOnly = hook == "starred";
        if (hook.startsWith ("text:"))
            c.search = hook.substring (5);
        if (hook.isNotEmpty())
            collectionPanel.setChips (c);
    }
#endif
    if (proc.getCollectionGeneration() != 0)
        refreshCollectionPanel(); // reopened editor: show the cached list at once
    proc.requestCollectionRefresh();

    refreshSyncReadout();
    refreshServerStatus();
    refreshSource();
    refreshTakes();
    setResizable (true, true);
    setResizeLimits (kMinWindowW, kMinWindowH, 1920, 1300);
    // Test hook (standalone): HSLINK_PREVIEW_WAV preselects a local WAV for the preview.
#if defined (HSLINK_TEST_HOOKS)
    if (standalone)
    {
        const auto hook = juce::SystemStats::getEnvironmentVariable ("HSLINK_PREVIEW_WAV", {});
        if (hook.isNotEmpty() && juce::File (hook).existsAsFile())
            proc.audition.select (juce::File (hook), {});
        // Debug hook (standalone, off by default): HSLINK_DEBUG_SEED_TAKES=1 lists three
        // generated click-track takes so the TAKES rows can be checked without a server.
        if (juce::SystemStats::getEnvironmentVariable ("HSLINK_DEBUG_SEED_TAKES", {}) == "1")
            seedDebugTakes();
    }
#endif
    // SETTINGS popover: move the SETUP form into it. addChildComponent keeps each
    // control's own visibility (standalone-only fields stay hidden in the plugin).
    for (juce::Component* c : { (juce::Component*) &urlCaption, (juce::Component*) &urlEditor,
                                (juce::Component*) &keyCaption, (juce::Component*) &keyEditor,
                                (juce::Component*) &keyRevealButton,
                                (juce::Component*) &keySaveNote,
                                (juce::Component*) &modeCaption, (juce::Component*) &modeBox,
                                (juce::Component*) &strengthEditor, (juce::Component*) &refLockNote,
                                (juce::Component*) &backendCaption, (juce::Component*) &backendBox,
                                (juce::Component*) &aceModelCaption, (juce::Component*) &aceModelBox,
                                (juce::Component*) &regionCaption, (juce::Component*) &regionStartEditor,
                                (juce::Component*) &regionEndEditor, (juce::Component*) &regionInfo })
        settingsPopover.addChildComponent (c);
    settingsPopover.closeButton.onClick = [this] { setSettingsOpen (false); };
    addChildComponent (settingsPopover);
    settingsButton.setTooltip ("Server, key, mode, backend and region settings");
    settingsButton.setClickingTogglesState (false);
    settingsButton.onClick = [this] { setSettingsOpen (! settingsPopover.isVisible()); };
    addAndMakeVisible (settingsButton);
    refreshUnderstand();

    if (standalone)
        parseShotArgs (juce::JUCEApplicationBase::getCommandLineParameters());
    setSize (kMinWindowW, kMinWindowH);
}

void SettingsPopover::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff141826));
    g.fillRoundedRectangle (b, 6.0f);
    g.setColour (juce::Colour (0xffe0559a).withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);
    g.setColour (juce::Colour (0xffd8dbe6));
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("SETTINGS", getLocalBounds().reduced (12, 8).removeFromTop (20), juce::Justification::centredLeft);
}

MidiExportPopover::MidiExportPopover()
{
    struct Tip { juce::TextButton* b; bool on; const char* tip; };
    for (const auto& t : { Tip { &drumsButton, true, "Drums: one GM drum note per detected hit (_hits.mid)" },
                           Tip { &melodyButton, true, "Melody (auto): pitched notes, skipped with a reason when the take is not tonal (_melody.mid)" },
                           Tip { &chordsButton, true, "Chords (auto): chords and the key, skipped with a reason when none is found (_chords.mid)" },
                           Tip { &perBeatButton, false, "Chords per bar (off) or per beat (on); the label shows the current choice" },
                           Tip { &tempoButton, true, "Tempo map with bar markers (_tempo.mid; takes with a tempo only)" },
                           Tip { &oneFileButton, false, "A file per part (off) or one multitrack file, _multitrack.mid (on); the label shows the current choice" } })
    {
        t.b->setClickingTogglesState (true);
        t.b->setToggleState (t.on, juce::dontSendNotification);
        t.b->setTooltip (t.tip);
        addAndMakeVisible (*t.b);
    }
    // Two-way choices say their current state in words (an unlit "One file" read as nothing).
    auto words = [this]
    {
        oneFileButton.setButtonText (oneFileButton.getToggleState() ? "One file" : "Separate files");
        perBeatButton.setButtonText (perBeatButton.getToggleState() ? "Per beat" : "Per bar");
    };
    oneFileButton.onClick = words;
    perBeatButton.onClick = words;
    words();
    addAndMakeVisible (closeButton);
}

void MidiExportPopover::paint (juce::Graphics& g)
{
    // Same hardware card as the SETTINGS popover.
    auto b = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff141826));
    g.fillRoundedRectangle (b, 6.0f);
    g.setColour (juce::Colour (0xffe0559a).withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);
    g.setColour (juce::Colour (0xffd8dbe6));
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("MIDI EXPORT", getLocalBounds().reduced (12, 10).removeFromTop (24), juce::Justification::centredLeft);
}

void MidiExportPopover::resized()
{
    // 2 columns x 3 rows of 24 px lit buttons, 6 px gaps; Close in the header row.
    auto a = getLocalBounds().reduced (12, 10);
    closeButton.setBounds (a.removeFromTop (24).removeFromRight (64));
    a.removeFromTop (8);
    const int gap = 6, colW = (a.getWidth() - gap) / 2;
    juce::TextButton* rows[3][2] = { { &drumsButton, &tempoButton }, { &melodyButton, &chordsButton },
                                     { &oneFileButton, &perBeatButton } };
    for (int r = 0; r < 3; ++r)
    {
        auto row = a.removeFromTop (24);
        rows[r][0]->setBounds (row.removeFromLeft (colW));
        rows[r][1]->setBounds (row.removeFromRight (colW));
        a.removeFromTop (gap);
    }
}

void HotStepLinkEditor::setMidiPickerOpen (bool open)
{
    midiPicker.setVisible (open);
    exportMidiMenuButton.setToggleState (open, juce::dontSendNotification);
    if (open)
        midiPicker.toFront (false);
}

void HotStepLinkEditor::applyMidiPicker (MidiBuildInputs& in) const
{
    in.drums = midiPicker.drumsButton.getToggleState();
    in.melody = midiPicker.melodyButton.getToggleState();
    in.chords = midiPicker.chordsButton.getToggleState();
    in.chordMode = midiPicker.perBeatButton.getToggleState() ? 1 : 0;
    in.tempo = midiPicker.tempoButton.getToggleState();
    in.combined = midiPicker.oneFileButton.getToggleState();
}

void HotStepLinkEditor::setKeyRevealed (bool revealed)
{
    // Display only: the stored key and the field text are not touched, nothing is saved.
    keyRevealed = revealed;
#if defined (HSLINK_TEST_HOOKS)
    if (testKeyRevealMutant == 3)
        HotStepLinkProcessor::saveStoredKey (proc.getDawKey() + "-M3"); // teeth: the toggle writes the settings file
    if (testKeyRevealMutant != 1) // teeth 1: the field stays as it was
#endif
    keyEditor.setPasswordCharacter (revealed ? (juce::juce_wchar) 0 : (juce::juce_wchar) '*');
    keyRevealButton.setButtonText (revealed ? "Hide" : "Show");
    keyRevealButton.setTooltip (revealed ? "Hide the DAW key again (shown as stars)"
                                         : "Show the DAW key as plain text. This does not change or save it");
    keyRevealButton.setTitle (revealed ? "Hide DAW key" : "Show DAW key");
}

void HotStepLinkEditor::JobLabel::setStatus (const juce::String& shown, const juce::String& full)
{
    inSetStatus = true;
    setText (shown, juce::dontSendNotification);
    inSetStatus = false;
    fullText = full;
    updateTip();
}

void HotStepLinkEditor::JobLabel::textWasChanged()
{
    if (inSetStatus)
        return; // setStatus sets the tooltip once the full text is stored
    fullText.clear(); // a plain setText: the line is all the text there is
    updateTip();
}

void HotStepLinkEditor::JobLabel::resized()
{
    juce::Label::resized();
    updateTip(); // whether the line fits depends on the box
}

bool HotStepLinkEditor::JobLabel::lineIsCut() const
{
    const auto box = getBorderSize().subtractedFrom (getLocalBounds());
    if (box.getWidth() <= 2 || box.getHeight() <= 0 || getText().isEmpty())
        return false;
    const auto font = getFont();
    juce::AttributedString as;
    as.append (getText(), font);
    juce::TextLayout tl;
    tl.createLayout (as, (float) box.getWidth() - 2.0f);
    const int maxLines = juce::jmax (1, (int) ((float) box.getHeight() / font.getHeight()));
    return tl.getNumLines() > maxLines || tl.getWidth() > (float) box.getWidth();
}

void HotStepLinkEditor::JobLabel::updateTip()
{
    const auto shown = getText();
    const bool fuller = fullText.isNotEmpty() && fullText != shown;
#if defined (HSLINK_TEST_HOOKS)
    if (testKeyRevealMutant == 8) // teeth 8: no tooltip
        return;
    if (testKeyRevealMutant == 9) // teeth 9: the full text is ignored
    {
        setTooltip (lineIsCut() ? shown : juce::String());
        return;
    }
    if (testKeyRevealMutant == 10) // teeth 10: always a tooltip, the line's own text
    {
        setTooltip (fuller ? fullText : shown);
        return;
    }
#endif
    // Only when there is more to read than the line shows; a short line has no tooltip.
    setTooltip (fuller ? fullText : lineIsCut() ? shown : juce::String());
}

void HotStepLinkEditor::visibilityChanged()
{
#if defined (HSLINK_TEST_HOOKS)
    if (testKeyRevealMutant == 5) // teeth 5: a hidden editor keeps the shown key
        return;
#endif
    // A host can hide the editor without destroying it: the key must not come back shown.
    if (! isVisible() && keyRevealed)
        setKeyRevealed (false); // display only, nothing is saved
}

void HotStepLinkEditor::setSettingsOpen (bool open)
{
    settingsPopover.setVisible (open);
#if defined (HSLINK_TEST_HOOKS)
    if (testKeyRevealMutant != 2) // teeth 2: the panel keeps the shown state
#endif
    setKeyRevealed (false); // the key is masked again whenever the panel opens or closes
    settingsButton.setToggleState (open, juce::dontSendNotification);
    if (open)
        settingsPopover.toFront (false);
}

void HotStepLinkEditor::refreshUnderstand()
{
    juce::String k { "--" }, t { "--" };
    juce::StringArray tg;
    for (const auto& j : proc.daw.getJobs())
    {
        if (! j.isUnderstand || j.state != DawJob::State::done)
            continue;
        if (j.key.isNotEmpty())
            k = j.key;
        if (j.bpm > 0.0)
            t = juce::String (juce::roundToInt (j.bpm)) + " BPM";
        tg = j.tags;
    }
    if (k == "--" && songKeyBox.getSelectedId() > 1)
        k = songKeyBox.getText();
    if (k != understandKeyText || t != understandTempoText || tg != understandTags)
    {
        understandTags = tg;
        understandKeyText = k;
        understandTempoText = t;
        repaint (understandPanel);
    }
}

HotStepLinkEditor::~HotStepLinkEditor()
{
    proc.apvts.removeParameterListener ("backend", this);
    proc.previewPlay.store (false); // L1: preview stops when the editor closes
    proc.commitTakeDelete(); // no Undo button any more: the pending delete is moved to its Deleted folder (pool thread)
    if (keyHost != nullptr)
        keyHost->removeKeyListener (this);
    setLookAndFeel (nullptr);
}

#if defined (HSLINK_TEST_HOOKS)
std::atomic<bool> HotStepLinkEditor::testNoTopLevelKeys { false };
#endif

void HotStepLinkEditor::parentHierarchyChanged()
{
    juce::Component* top = getTopLevelComponent();
    if (top == keyHost.getComponent())
        return;
    if (keyHost != nullptr)
        keyHost->removeKeyListener (this);
    keyHost = top;
    if (top != nullptr)
        top->addKeyListener (this);
}

bool HotStepLinkEditor::keyPressed (const juce::KeyPress& k, juce::Component*)
{
    auto* f = juce::Component::getCurrentlyFocusedComponent();
#if defined (HSLINK_TEST_HOOKS)
    const bool noTopLevelKeys = testNoTopLevelKeys.load();
#else
    const bool noTopLevelKeys = false;
#endif
    if (noTopLevelKeys || ! takeEditor.isOpen() || ! takeEditor.isShowing())
        return false;
    for (auto* c = f; c != nullptr; c = c->getParentComponent())
        if (dynamic_cast<juce::TextEditor*> (c) != nullptr)
            return false; // a text field keeps its own Ctrl+Z
    return takeEditor.keyPressed (k);
}

void HotStepLinkEditor::refreshSyncReadout()
{
    auto& s = proc.hostSync;
    const auto counter = s.updateCounter.load (std::memory_order_acquire);
    if (drawnOnce && counter == lastCounter)
        return;
    lastCounter = counter;
    drawnOnce = true;

    {
        const juce::String tt = syncTempoCellText (standalone, s), mt = syncMeterCellText (standalone, s),
                           bt = syncBarsCellText (standalone, s);
        if (tt != syncTempoText || mt != syncMeterText || bt != syncBarsText)
        {
            syncTempoText = tt; syncMeterText = mt; syncBarsText = bt;
            repaint (syncPanel);
        }
    }
    if (takesNote.isNotEmpty())
    {
        if (juce::Time::getMillisecondCounter() < takesNoteUntilMs)
        {
            syncLabel.setText (takesNote, juce::dontSendNotification);
            return;
        }
        takesNote.clear();
    }
    if (standalone || ! s.hostSyncActive.load (std::memory_order_relaxed))
    {
        syncLabel.setText (standalone ? "Host sync: off (standalone, manual BPM/key)"
                                      : "Host sync: waiting for host",
                           juce::dontSendNotification);
        return;
    }

    syncLabel.setText (syncReadoutText (s), juce::dontSendNotification);
}

void HotStepLinkEditor::parameterChanged (const juce::String&, float)
{
    juce::Component::SafePointer<HotStepLinkEditor> safe (this);
    juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->applyTakesLimit (true); safe->applyIgnoredSettings(); safe->applyBackendRefLock(); } });
}

// Takes range and value follow the selected backend's per-request limit. Message thread.
void HotStepLinkEditor::applyTakesLimit (bool announce)
{
    const auto id = proc.getSelectedBackendId();
    const int mx = DawJobRequest::maxTakesForBackend (id);
    auto* p = proc.apvts.getParameter ("takes");
    if (p == nullptr)
        return;
    const auto range = proc.apvts.getParameterRange ("takes");
    const int cur = juce::roundToInt (range.convertFrom0to1 (p->getValue()));
    const int top = mx > 0 ? juce::jmin (mx, (int) range.end) : (int) range.end;
    if (top > (int) range.start)
        takesSlider.setRange (range.start, (double) top, 1.0);
    takesSlider.setEnabled (top > (int) range.start);
    const auto note = DawJobRequest::takesClampNote (cur, id);
    if (note.isNotEmpty())
    {
        p->setValueNotifyingHost (range.convertTo0to1 ((float) DawJobRequest::clampTakesForBackend (cur, id)));
        if (announce)
        {
            takesNote = note;
            takesNoteUntilMs = juce::Time::getMillisecondCounter() + 6000;
            syncLabel.setText (note, juce::dontSendNotification);
        }
    }
}

// Controls the selected backend ignores are greyed with a tooltip; re-enabled on switch.
void HotStepLinkEditor::applyIgnoredSettings()
{
    const auto id = proc.getSelectedBackendId();
    const auto ign = DawJobRequest::ignoredFieldsForBackend (id);
    auto apply = [&] (juce::Slider& s, juce::uint32 bit)
    {
        const bool off = (ign & bit) != 0;
        s.setEnabled (! off);
        s.setTooltip (off ? DawJobRequest::ignoredFieldsNote (bit, id) : juce::String());
    };
    apply (durationSlider, DawJobRequest::ignDuration);
    apply (guidanceKnob, DawJobRequest::ignGuidance);
    apply (stepsKnob, DawJobRequest::ignSteps);
    apply (variationKnob, DawJobRequest::ignVariation);
}

juce::String HotStepLinkEditor::syncTempoCellText (bool isStandalone, const HostSyncState& s)
{
    const bool live = ! isStandalone && s.hostSyncActive.load (std::memory_order_relaxed);
    if (live && s.bpmKnown.load (std::memory_order_relaxed))
        return juce::String (s.bpm.load (std::memory_order_relaxed), 2);
    return "--";
}

juce::String HotStepLinkEditor::syncMeterCellText (bool isStandalone, const HostSyncState& s)
{
    const bool live = ! isStandalone && s.hostSyncActive.load (std::memory_order_relaxed);
    const int num = s.timeSigNum.load (std::memory_order_relaxed), den = s.timeSigDen.load (std::memory_order_relaxed);
    if (live && num > 0 && den > 0)
        return juce::String (num) + "/" + juce::String (den);
    return "--";
}

juce::String HotStepLinkEditor::syncBarsCellText (bool isStandalone, const HostSyncState& s)
{
    const bool live = ! isStandalone && s.hostSyncActive.load (std::memory_order_relaxed);
    const int num = s.timeSigNum.load (std::memory_order_relaxed), den = s.timeSigDen.load (std::memory_order_relaxed);
    if (! (live && num > 0 && den > 0))
        return "--";
    const double barQ = 4.0 * num / den;
    auto barOf = [barQ] (double ppq) { return (int) std::floor (ppq / barQ + 1.0e-6) + 1; };
    // A NaN or inf position never reaches (int) std::floor (undefined behaviour): show "--".
    // Negative ppq (pre-roll) is kept as is: ppq -1 in 4/4 reads bar 0.
    if (s.isLooping.load (std::memory_order_relaxed))
    {
        const double a = s.loopStartPpq.load (std::memory_order_relaxed), b = s.loopEndPpq.load (std::memory_order_relaxed);
        if (! std::isfinite (a) || ! std::isfinite (b))
            return "--";
        // End bar = barOf (loopEnd) - 1 with the 1e-6 tolerance: exact for an end on a barline
        // (8..24 in 4/4 gives 3-6), but a mid-bar end drops the partial bar it lands in
        // (8..22 gives 3-5). Pinned by the tests; clamped so the end is never before the start.
        return juce::String (barOf (a)) + "-" + juce::String (juce::jmax (barOf (a), barOf (b) - 1));
    }
    const double ppq = s.ppqPosition.load (std::memory_order_relaxed);
    if (! std::isfinite (ppq))
        return "--";
    return juce::String (barOf (ppq));
}

double HotStepLinkEditor::fitHostBpm (bool isStandalone, const HostSyncState& s, double manualBpm)
{
    if (isStandalone || ! s.hostSyncActive.load (std::memory_order_relaxed))
        return manualBpm;
#if defined (HSLINK_TEETH_NO_TEMPO_GATE)
    return s.bpm.load (std::memory_order_relaxed); // teeth: before round 15, the stored number whatever bpmKnown says
#else
    return s.tempoForCascade(); // 0 while the host gives no tempo
#endif
}

juce::String HotStepLinkEditor::syncReadoutText (const HostSyncState& s)
{
    juce::String t;
    t << "Host sync: ";
#if defined (HSLINK_TEETH_NO_TEMPO_GATE)
    if (true) // teeth: before 72fa3a8, the stored number is always printed
#else
    if (s.bpmKnown.load (std::memory_order_relaxed))
#endif
        t << juce::String (s.bpm.load (std::memory_order_relaxed), 2) << " BPM  ";
    else
        t << "no host tempo  ";
    t << s.timeSigNum.load (std::memory_order_relaxed) << "/" << s.timeSigDen.load (std::memory_order_relaxed)
      << "  ppq " << juce::String (s.ppqPosition.load (std::memory_order_relaxed), 2)
      << (s.isPlaying.load (std::memory_order_relaxed) ? "  PLAY" : "  STOP");
    if (s.isLooping.load (std::memory_order_relaxed))
        t << "  loop " << juce::String (s.loopStartPpq.load (std::memory_order_relaxed), 2)
          << "-" << juce::String (s.loopEndPpq.load (std::memory_order_relaxed), 2);
    return t;
}

void HotStepLinkEditor::refreshServerStatus()
{
    const auto st = proc.daw.getSnapshot();
    const auto url = proc.daw.getBaseUrl();
    juce::String t;
    const auto switching = proc.daw.getSwitchingTo(); // a Generate asked for another backend
    if (st.online && switching.isNotEmpty())
        t << "Switching to " << switching << "...";
    else if (st.online)
        t << DawStatus::onlinePillText (st.etaBackend, st.engineModel, proc.getSelectedBackendId());
    else
        t << (st.heartbeatsOk == 0 && st.consecutiveFailures == 0 ? "Connecting..." : "Offline");
    const auto ps = pillStateFor (st.online, proc.daw.getJobs());
    if (st.online)
        t << " - " << pillStateWord (ps);
    if (ps != pillState)
    {
        pillState = ps;
        repaint (headerArea);
    }
    if (t != lastServerText)
    {
        lastServerText = t;
        serverLabel.setText (t, juce::dontSendNotification);
    }

    // Do not flash the overlay before the first heartbeat has had a chance.
    // A banner over the Generate controls only; hidden while the take editor covers them.
    const bool offline = ! st.online && (st.consecutiveFailures > 0 || st.everOnline);
    if (lastServerOnline == offline)
    {
        lastServerOnline = ! offline; // "not known offline": Connecting... does not block
        refreshJob(); // re-run the Generate gate
    }
#if defined (HSLINK_TEST_HOOKS)
    const bool overlayFullWindow = offlineMutantForTest == mutOverlayFullWindow;
#else
    const bool overlayFullWindow = false;
#endif
    const bool showOverlay = offline && (! takeEditor.isVisible() || overlayFullWindow);
    if (showOverlay)
        overlay.setMessage (url, st.lastError, st.recentAttempts);
    if (overlay.isVisible() != showOverlay)
    {
        overlay.setVisible (showOverlay);
        if (showOverlay)
            overlay.toFront (false);
    }
    applyModeGating (st);
    applyAceModelGating (st.online && st.baseAvailable);
#if defined (HSLINK_TEST_HOOKS)
    if (DawWorker::testCapsMutant.load() == DawWorker::capsMutSkipApply)
        return;
#endif
    applyBackendAvailability (st);
}

void HotStepLinkEditor::applyBackendAvailability (const DawStatus& st)
{
    // The list counts only while its server is online; offline nothing is marked.
    const bool known = st.online && st.backendsKnown;
    const int n = juce::jmin (HotStepLinkProcessor::kBackendChoices, backendBox.getNumItems());
    const int sel = backendBox.getSelectedItemIndex();
    const auto key = (known ? "k|" : "u|") + st.backendIds.joinIntoString (",") + "|" + juce::String (sel);
    if (key == lastBackendAvailKey)
        return;
    lastBackendAvailKey = key;
    // Soft marking: every item stays enabled and can be picked. An unlisted entry only
    // gets the dimmed text colour (kUnlistedItemAlpha) in the open menu. Item text and
    // ids are not touched, so the parameter mapping is unchanged.
    bool legacyDisable = false;
#if defined (HSLINK_TEST_HOOKS)
    legacyDisable = DawWorker::testCapsMutant.load() == DawWorker::capsMutItemsDisabled; // teeth: the hard gate
#endif
    if (auto* menu = backendBox.getRootMenu())
        for (juce::PopupMenu::MenuItemIterator it (*menu, true); it.next();)
        {
            auto& item = it.getItem();
            if (item.itemID < 1 || item.itemID > n)
                continue;
            const bool avail = HotStepLinkProcessor::backendAvailable (item.itemID - 1, known, st.backendIds);
            item.colour = (avail || legacyDisable) ? juce::Colour() : HslinkLook::unlistedItemColour (false);
            item.isEnabled = avail || ! legacyDisable;
        }
    unsigned dim = 0;
    for (int c = 0; c < RoleCards::kCount; ++c) // card c sets backend choice c + 1
        if (! HotStepLinkProcessor::backendAvailable (c + 1, known, st.backendIds))
            dim |= 1u << c;
    roleCards.setDimmed (dim);
    auto tip = HotStepLinkProcessor::backendPickerTooltip (known, st.backendIds);
    const auto note = HotStepLinkProcessor::backendUnlistedNote (sel, known, st.backendIds);
#if defined (HSLINK_TEST_HOOKS)
    if (note.isNotEmpty() && DawWorker::testCapsMutant.load() == DawWorker::capsMutAutoReset)
        if (auto* bp = proc.apvts.getParameter ("backend"))
            bp->setValueNotifyingHost (0.0f); // teeth: the unlisted choice is put back to Auto
#endif
    if (note.isNotEmpty())
    {
        // The selection is kept and Generate is not blocked; the status line says it once
        // and the picker text stays amber while it holds.
        tip = "Selected: " + note + "." + (tip.isEmpty() ? juce::String() : "\n" + tip);
        takesNote = note;
        takesNoteUntilMs = juce::Time::getMillisecondCounter() + 8000;
        syncLabel.setText (note, juce::dontSendNotification);
    }
    backendBox.setTooltip (tip);
    if (note.isNotEmpty())
        backendBox.setColour (juce::ComboBox::textColourId, kAmberA);
    else
        backendBox.removeColour (juce::ComboBox::textColourId); // back to the look-and-feel colour
}

bool HotStepLinkEditor::backendItemMarked (int index)
{
    if (auto* menu = backendBox.getRootMenu())
        for (juce::PopupMenu::MenuItemIterator it (*menu, true); it.next();)
            if (it.getItem().itemID == index + 1)
                return it.getItem().colour != juce::Colour();
    return false;
}

void HotStepLinkEditor::applyAceModelGating (bool baseOk)
{
    // Lego forces Base (turbo + lego is rejected); Base needs base_available.
    const auto mode = modeBox.getText() + (baseOk ? "|b" : "|n");
    if (mode == lastModelGateMode)
        return;
    lastModelGateMode = mode;
    const bool lego = modeBox.getText() == "lego";
    aceModelBox.setItemEnabled (2, ! lego);
    aceModelBox.setItemEnabled (3, baseOk);
    juce::String tip ("Sent only when Backend is ACE. Auto: the server's loaded DiT.");
    if (lego)
        tip << juce::newLine << "Turbo: unavailable in lego mode (lego always uses Base)";
    if (! baseOk)
        tip << juce::newLine << "Base: the server reports no base DiT model";
    aceModelBox.setTooltip (tip);
}

void HotStepLinkEditor::commitSeed()
{
    const auto t = seedEditor.getText().trim();
    if (t.isNotEmpty())
    {
        // A seed over the limit is refused, never clamped: the seed in effect stays, the
        // field keeps the typed text with the error outline and startJob sends nothing
        // until the field holds a valid seed or is empty. The Collection footer says why.
        if (const auto r = SeedInput::parse (t); r.ok)
            proc.setSeed (r.value);
        else
        {
            setSeedRefused (true);
            seedRandomButton.setToggleState (proc.getSeed() < 0, juce::dontSendNotification);
            collectionPanel.showNote (SeedInput::refusedText(), true, 8000);
            return;
        }
    }
    else if (! seedRandomButton.getToggleState())
        proc.setSeed (-1);
    refreshSeedUi();
}

void HotStepLinkEditor::setSeedRefused (bool refused)
{
    if (seedRefused == refused)
        return;
    seedRefused = refused;
    if (refused)
    {
        seedEditor.setColour (juce::TextEditor::outlineColourId, kBad);
        seedEditor.setColour (juce::TextEditor::focusedOutlineColourId, kBad);
    }
    else
    {
        seedEditor.removeColour (juce::TextEditor::outlineColourId);
        seedEditor.removeColour (juce::TextEditor::focusedOutlineColourId);
    }
    seedEditor.repaint();
}

void HotStepLinkEditor::refreshSeedUi()
{
    setSeedRefused (false); // the field is about to show the seed in effect
    const auto sd = proc.getSeed();
    seedRandomButton.setToggleState (sd < 0, juce::dontSendNotification);
    seedEditor.setText (sd < 0 ? juce::String() : juce::String (sd), juce::dontSendNotification);
}

void HotStepLinkEditor::refreshTextState()
{
    lastStateGen = proc.getStateGeneration();
    // No notifications: reloading must not write the values back.
    captionEditor.setText (proc.getCaption(), false);
    lyricsEditor.setText (proc.getLyrics(), false);
    const bool inst = proc.getInstrumental();
    instrumentalButton.setToggleState (inst, juce::dontSendNotification);
    lyricsEditor.setEnabled (! inst);
    songKeyBox.setSelectedId (proc.getKeyIndex() + 1, juce::dontSendNotification);
    peakModeBox.setSelectedId (proc.getPeakMode() + 1, juce::dontSendNotification);
    refreshSeedUi();
    modeBox.setSelectedId (proc.getModeId(), juce::dontSendNotification);
    manualBpm.setValue (proc.getManualBpm(), juce::dontSendNotification);
    refreshSlotButtons();
}

void HotStepLinkEditor::refreshPresetList (const juce::String& select)
{
    presetBox.clear (juce::dontSendNotification);
    const auto names = proc.listPresets();
    for (int i = 0; i < names.size(); ++i)
        presetBox.addItem (names[i], i + 1);
    const int idx = names.indexOf (select);
    if (idx >= 0)
        presetBox.setSelectedId (idx + 1, juce::dontSendNotification);
    else
        presetBox.setText ({}, juce::dontSendNotification);
}

void HotStepLinkEditor::refreshSlotButtons()
{
    const int a = proc.getActiveSlot();
    slotAButton.setToggleState (a == 0, juce::dontSendNotification);
    slotBButton.setToggleState (a == 1, juce::dontSendNotification);
    copyAbButton.setButtonText (a == 0 ? "A=B" : "B=A");
    copyAbButton.setTooltip (a == 0 ? "Copy A into B" : "Copy B into A");
}

void HotStepLinkEditor::switchSlot (int slot)
{
    commitSeed();
    proc.selectSlot (slot);
    refreshTextState(); // generation bumped; reload now so the UI follows at once
}

void HotStepLinkEditor::startJob (int takesOverride, juce::int64 seedOverride)
{
    commitSeed();
    // The seed field holds a refused text: no job goes out with a seed the user did not
    // see accepted. Every route into a generate job comes through here.
    if (seedRefused)
    {
        jobLabel.setText (SeedInput::refusedText(), juce::dontSendNotification);
        collectionPanel.showNote (SeedInput::refusedText(), true, 8000); // held; the job label is rewritten by the next refresh
        lastJobText = {};
        return;
    }
    auto r = proc.makeRequest (captionEditor.getText().trim(), standalone ? manualBpm.getValue() : 0.0);
    r.mode = modeBox.getText();
    if (takesOverride > 0)
        r.takes = takesOverride;
    if (seedOverride >= 0)
        r.seed = seedOverride;
    // VARIATION: spread a fixed seed per generate (a random seed stays random). The
    // "+4 more like" path spreads its own base before it gets here (its note shows it).
    if (seedOverride < 0)
    {
        const double variation = proc.apvts.getRawParameterValue ("variation")->load();
        const int n = generateCount;
        r.seed = SeedSpread::spread (r.seed, variation, n);
    }
    // Repaint and lego send the source clip (and repaint its region).
    juce::String err;
    if (! proc.prepareSourceJob (r, regionStartEditor.getText().getDoubleValue(),
                                 regionEndEditor.getText().getDoubleValue(), err))
    {
        jobLabel.setText ("Blocked: " + err, juce::dontSendNotification);
        collectionPanel.showNote ("Blocked: " + err, true); // held; the job label is rewritten by the next refresh
        lastJobText = {};
        return;
    }
    const int id = proc.daw.submitGenerate (r);
    if (id == 0)
    {
        jobLabel.setText ("Queue full: " + juce::String (DawWorker::kMaxQueued) + " jobs waiting", juce::dontSendNotification);
        lastJobText = {};
        return;
    }
    DawJob cur;
    if (currentJob == 0 || ! proc.daw.getJob (currentJob, cur) || cur.isFinished())
        currentJob = id;
    ++generateCount; // only a job that went out counts as a generate
    refreshJob();
}

void HotStepLinkEditor::startMoreLike (const juce::String& key)
{
    const auto model = buildTakeModel();
    const auto* c = model.find (key);
    DawJob job;
    if (c == nullptr || ! proc.daw.getJob (c->jobId, job))
    {
        collectionPanel.showNote ("More like this: the take's job is gone", true);
        return;
    }
    const juce::int64 seed = juce::isPositiveAndBelow (c->takeIndex, job.takeSeeds.size()) ? job.takeSeeds[c->takeIndex] : -1;
    if (seed < 0)
    {
        collectionPanel.showNote ("More like this: the server did not report a seed for " + c->label, true);
        return;
    }
    // The server seeds take i of a job with base + i, so base = takeSeed + 1 gives the four
    // new takes takeSeed + 1 .. takeSeed + 4: neighbours of take N, none a repeat of it.
    // At the top of the range the base steps down instead so every seed stays exact.
    const juce::int64 base0 = seed + 4 <= SeedInput::kMaxSeed ? seed + 1 : seed - 4;
    const juce::int64 base = SeedSpread::spread (base0, proc.apvts.getRawParameterValue ("variation")->load(), generateCount);
    collectionPanel.showNote ("4 more like " + c->label + " (seeds " + juce::String (base) + " to " + juce::String (base + 3) + ")", false);
    startJob (4, base);
}

void HotStepLinkEditor::loadTakeIntoSlot (const juce::String& key, int slot)
{
    const auto model = buildTakeModel();
    const auto* c = model.find (key);
    DawJob job;
    if (c == nullptr || ! proc.daw.getJob (c->jobId, job))
    {
        collectionPanel.showNote ("Load into slot: the take's job is gone", true);
        return;
    }
    slot = slot == 1 ? 1 : 0;
    // Reuse the header A/B: make the slot active (the live settings go into the old
    // slot), then write the take's request over it like a manual edit.
    // If the slot is already active, switchSlot is a no-op, so back the live settings up
    // into the other slot first: the user's edits stay one A/B click away.
    // Commit a typed seed first, like the Copy A/B button, so it lands in the backup too.
    if (proc.getActiveSlot() == slot)
    {
        commitSeed();
        proc.copyActiveToOther();
    }
    switchSlot (slot);
    auto s = proc.getSlot (slot);
    const auto& rq = job.request;
    s.caption = rq.caption;
    s.lyrics = rq.lyrics;
    if (rq.bpm > 0.0)       s.bpm = rq.bpm;
    if (rq.durationS > 0.0) s.duration = (float) rq.durationS;
    const juce::int64 seed = juce::isPositiveAndBelow (c->takeIndex, job.takeSeeds.size()) ? job.takeSeeds[c->takeIndex] : -1;
    s.seed = seed >= 0 ? seed : rq.seed;
    s.aceModel = rq.aceModel;
    proc.applySettings (s);
    refreshTextState();
    collectionPanel.showNote (c->label + " settings loaded into slot " + juce::String (slot == 0 ? "A" : "B"), false);
}

#if defined (HSLINK_TEST_HOOKS)
void HotStepLinkEditor::seedDebugTakes()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hslink_debug_takes");
    dir.createDirectory();
    const double rate = 48000.0, bpm = 128.0;
    const double lens[3] = { 7.5, 6.9, 9.0 }; // 4 bars, a little short, a bar and a fifth over
    for (int k = 0; k < 3; ++k)
    {
        const int n = (int) (lens[k] * rate);
        juce::AudioBuffer<float> b (2, n);
        b.clear();
        const int beat = (int) (rate * 60.0 / bpm);
        for (int start = 0; start < n; start += beat)
            for (int i = 0; i < 2400 && start + i < n; ++i)
            {
                const float v = 0.6f * std::exp (-(float) i / 300.0f) * (float) std::sin (0.13 * (double) i);
                b.setSample (0, start + i, v);
                b.setSample (1, start + i, v);
            }
        const auto f = dir.getChildFile ("debug_take_" + juce::String (k + 1) + ".wav");
        f.deleteFile();
        {
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
            if (os == nullptr)
                continue;
            juce::WavAudioFormat wav;
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate (rate)
                                                  .withNumChannels (2)
                                                  .withBitsPerSample (24));
            if (w == nullptr)
                continue;
            w->writeFromAudioSampleBuffer (b, 0, n);
        }
        DawJob j;
        j.request.caption = "debug take " + juce::String (k + 1);
        j.request.bpm = bpm;
        j.request.durationS = lens[k];
        j.request.seed = 1000 + k;
        j.takeFiles.add (f);
        j.takeSeeds.add (1000 + k);
        j.localId = 900000 + k;
        proc.registerTakeAndWait (j, 0);
        proc.requestTakeMeasure (f, bpm);
    }
    refreshTakes();
}
#endif

void HotStepLinkEditor::refreshSource()
{
    proc.pollCapture();
    const bool armed = proc.isArmed();
    if (captureButton.getToggleState() != armed)
        captureButton.setToggleState (armed, juce::dontSendNotification);
    if (! refHintSeen && proc.firstCaptureDone())
        dismissRefHint(); // first capture: same as a click (the flag is already saved, no second write)
    sourceWave.setRecording (proc.isRecordingSource());
    sourceWave.setSource (proc.getSourceFile(), proc.getSourceLengthS(), proc.getSourceBpm());
    sourceWave.poll();
    refreshCaptureNow();
    const auto t = proc.getSourceStateText();
    if (t != lastSourceText)
    {
        lastSourceText = t;
        sourceLabel.setText (t, juce::dontSendNotification);
        refreshJob();
    }
}

void HotStepLinkEditor::refreshCaptureNow()
{
    // No processor logic: the existing stop-and-keep call while recording, else a readout.
    juce::String t ("CAPTURE"), tip;
    const bool rec = proc.isRecordingSource();
    if (standalone)
        tip = "Capture records from the host (plugin only); drop a WAV instead";
    else if (rec)
    {
        t = "STOP + KEEP";
        tip = "Stop the capture now and keep what was recorded";
    }
    else if (proc.isArmed())
    {
        t = "WAITING...";
        tip = "Armed: recording starts when the host plays inside the loop";
    }
    else
    {
        const double bpm = proc.getSourceBpm(), len = proc.getSourceLengthS();
        const int num = juce::jmax (1, proc.hostSync.timeSigNum.load (std::memory_order_relaxed));
        if (proc.getSourceFile() != juce::File() && bpm > 0.0 && len > 0.0)
            t = "CAPTURED " + juce::String (juce::roundToInt (len * bpm / 60.0 / num)) + " BARS";
        else if (proc.getSourceFile() != juce::File())
            t = "CAPTURED";
        tip = "Press ARM, then play the host: the take stops at the loop end or transport stop";
    }
    if (captureNowButton.getButtonText() != t)
        captureNowButton.setButtonText (t);
    captureNowButton.setEnabled (rec);
    if (captureNowButton.getTooltip() != tip)
        captureNowButton.setTooltip (tip);
}

void HotStepLinkEditor::useSourceFile (const juce::File& f)
{
    if (f == juce::File())
        return;
    if (! proc.setSourceFile (f))
        jobLabel.setText ("Not a readable audio file: " + f.getFileName(), juce::dontSendNotification);
    else if (regionEndEditor.isEmpty())
    {
        regionStartEditor.setText ("0", false);
        regionEndEditor.setText (juce::String (proc.getSourceLengthS(), 3), false);
    }
    refreshSource();
}

bool HotStepLinkEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return addTakeAllowed() && files.size() == 1 && juce::File (files[0]).hasFileExtension ("wav;aif;aiff;flac");
}

void HotStepLinkEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
    if (! addTakeAllowed() || files.size() == 0)
        return;
    // Dropped on the TAKES list (right plate): a new take. Anywhere else: the Source.
    if (queuePlate.contains (x, y))
        addTakeFromFile (juce::File (files[0]));
    else
        useSourceFile (juce::File (files[0]));
}

bool HotStepLinkEditor::addTakeFromFile (const juce::File& f)
{
    if (f == juce::File())
        return false;
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr || r->sampleRate <= 0.0 || r->lengthInSamples <= 0)
        {
            jobLabel.setText ("Not a readable audio file: " + f.getFileName(), juce::dontSendNotification);
            return false;
        }
    }
    const auto added = proc.addShotTakeChecked (f);
    if (added == HotStepLinkProcessor::AddTake::duplicate || added == HotStepLinkProcessor::AddTake::refused)
    {
        jobLabel.setText ((added == HotStepLinkProcessor::AddTake::duplicate ? "Already in the take list: " : "Take not added: ")
                              + f.getFileName(),
                          juce::dontSendNotification);
        return false;
    }
    if (added == HotStepLinkProcessor::AddTake::pending)
    {
        jobLabel.setText ("Adding take (peak guard)...", juce::dontSendNotification); // the result line comes from takeImportMessage
        return true;
    }
    jobLabel.setText ("Added take: " + f.getFileName(), juce::dontSendNotification);
    refreshTakes();
    return true;
}

// One VBlank tick. Nothing here or in paint makes a file-system call: the take
// grid, the Export button and the preview card are filled from processor state.
// periodic: every 10th tick (about 6 times a second).
void HotStepLinkEditor::tick (bool periodic)
{
    if (periodic && shot.step > 0)
        runShotStep();
    // Header bars: 20 fps (every 3rd VBlank). isShowing() is false while the editor is
    // hidden or its window minimised, so the animation pauses then.
    if (headerAnimOn && (vblankTick % 3) == 0 && isShowing())
    {
        ++headerAnimFrame;
        if (headerBars.isVisible())
            headerBars.repaint();
    }
    {
        const float pk = proc.inputPeak.exchange (0.0f, std::memory_order_relaxed);
        const float db = juce::Decibels::gainToDecibels (pk, -60.0f);
        const float target = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        const float next = juce::jmax (target, inputLevelShown * 0.9f);
        if (std::abs (next - inputLevelShown) > 0.002f)
        {
            inputLevelShown = next < 0.002f ? 0.0f : next;
            repaint (inputPanel);
        }
    }
    if (proc.getStateGeneration() != lastStateGen)
        refreshTextState();
    roleCards.setActive (proc.getActiveRoleCard());
    refreshSyncReadout();
    refreshPreview();
    proc.editorAudition.poll();
    takeEditor.tick();
    if (proc.getCollectionGeneration() != lastCollectionGen)
        refreshCollectionPanel();
    if (periodic)
    {
        collectionPanel.tick();
        refreshUnderstand();
        reanalyzeButton.setEnabled (proc.getSourceFile().existsAsFile());
        collectionPanel.setSelectedFile (proc.audition.getSelected());
        refreshServerStatus();
        refreshSource();
        refreshJob();
        proc.pollTakeDelete(); // the undo window: when it has passed the file is moved to its Deleted folder (pool thread)
        const auto failed = proc.takeTakeDeleteMessage();
        if (failed.isNotEmpty())
            collectionPanel.showNote (failed, true, 8000);
        if (const auto imported = proc.takeImportMessage(); imported.isNotEmpty())
            jobLabel.setText (imported, juce::dontSendNotification); // a guarded import finished
        refreshTakes();
    }
}

void HotStepLinkEditor::deleteTake (const juce::String& tempPath)
{
    const auto r = proc.deleteTake (tempPath); // no file call here; a selected take is deselected first
    if (r == HotStepLinkProcessor::TakeDelete::deleted)
        takeEditor.forgetHistory (juce::File (tempPath)); // C3
    if (r != HotStepLinkProcessor::TakeDelete::deleted)
        collectionPanel.showNote (HotStepLinkProcessor::takeDeleteText (r), true);
    refreshTakes();
}

void HotStepLinkEditor::undoDelete()
{
    if (! proc.undoTakeDelete())
        collectionPanel.showNote ("Too late to undo. A deleted take is in the Deleted folder beside its file", true);
    refreshTakes();
}

void HotStepLinkEditor::refreshJob()
{
    refreshQueue();
    DawJob j;
    const bool have = currentJob != 0 && proc.daw.getJob (currentJob, j);
    juce::String t ("Idle");
    juce::String tFull; // the same line with the uncut error, when there is one
    double prog = 0.0;
    if (have)
    {
        t = DawJob::headline (j); // same word as the queue row, see DawJob::label
        prog = j.progress;
        t << DawJob::etaText (j); // "Loading model..." while the server loads, else ETA
        t << "  takes " << j.takeFiles.size() << "/" << j.takeCount;
        if (j.ditModel.isNotEmpty())
            t << "  DiT " << j.ditModel;
        {
            // Level mode: the largest gain applied to this job's takes on import,
            // a cut (Peak guard or Normalise) or a boost (Normalise).
            float most = 0.0f;
            for (auto g : j.takeGuardDb)
                if (std::abs (g) > std::abs (most))
                    most = g;
            if (most != 0.0f)
                t << "  " << PeakGuard::text (most);
        }
        // Empty when the server reported no seed at all; see DawJob::seedText.
        if (const auto seedPart = DawJob::seedText (j.takeSeeds); seedPart.isNotEmpty())
            t << "  " << seedPart;
        if (j.error.isNotEmpty())
        {
            if (const auto whole = DawJob::fullErrorText (j); whole != j.error)
                tFull = t + "  " + whole;
            t << "  " << j.error;
        }
    }
    const bool active = have && ! j.isFinished();
    const bool needSrc = HotStepLinkProcessor::modeNeedsSource (modeBox.getText());
    const bool srcOk = ! needSrc || proc.sourceLoaded(); // state only; the file is checked on Generate
    // while a job runs, Generate queues the next one; offline it is blocked (banner over it)
    generateButton.setEnabled (srcOk && lastServerOnline);
    generateButton.setTooltip (! lastServerOnline ? juce::String ("HOT-Step is not reachable: see the banner, then Retry")
                               : srcOk ? juce::String()
                                     : modeBox.getText() + " needs a source clip first: "
                                           + (standalone ? "drop a WAV" : "arm Capture and play the host"));
    cancelButton.setEnabled (active && ! j.cancelRequested);
    if (t != lastJobText || tFull != lastJobFull)
    {
        lastJobText = t;
        lastJobFull = tFull;
        jobLabel.setStatus (t, tFull);
    }
    if (prog != jobProgress)
    {
        jobProgress = prog;
        repaint (progressArea);
    }
}

void HotStepLinkEditor::refreshQueue()
{
    std::vector<QueueListComponent::Card> cards;
    bool selectedAlive = false;
    int inFlight = 0;
    for (const auto& j : proc.daw.getJobs())
    {
        if (j.isUnderstand)
            continue;
        QueueListComponent::Card c;
        c.id = j.localId;
        c.caption = j.request.caption.isEmpty() ? juce::String ("(no caption)") : j.request.caption;
        c.state = QueueListComponent::stateLabel (j);
        c.takes = j.takeFiles.size();
        c.takeCount = j.request.takes;
        c.removable = DawWorker::isWaiting (j) && ! j.cancelRequested;
        c.finished = j.isFinished();
        cards.push_back (c);
        selectedAlive = selectedAlive || j.localId == selectedJob;
        if (! j.isFinished() && ! DawWorker::isWaiting (j) && inFlight == 0)
            inFlight = j.localId;
    }
    if (! selectedAlive)
        selectedJob = 0;
    // The job card and Cancel follow the job in flight, else the next waiting one.
    if (inFlight != 0)
        currentJob = inFlight;
    else
    {
        DawJob cur;
        if (currentJob != 0 && proc.daw.getJob (currentJob, cur) && cur.isFinished())
            for (const auto& c : cards)
                if (! c.finished)
                {
                    currentJob = c.id;
                    break;
                }
    }
    queueList.setCards (cards, selectedJob);
    // The QUEUE header count and the job card title are painted by the editor:
    // repaint when either changes, else they stay at 0 / "No job yet".
#if defined (HSLINK_TEST_HOOKS)
    const bool noHeaderRepaint = noHeaderRepaintForTest;
#else
    const bool noHeaderRepaint = false;
#endif
    if (! noHeaderRepaint && (queueList.getNumActiveCards() != lastQueueCountShown || currentJob != lastJobCardShown))
    {
        lastQueueCountShown = queueList.getNumActiveCards();
        lastJobCardShown = currentJob;
        repaint();
    }
}

// The grid of the take editor from a take cell: the test of requestTakeEdit.
template <typename Cell>
static TakeEditorMath::Grid takeEditorGridOf (const Cell& cell)
{
    TakeEditorMath::Grid g;
    g.bpm = cell.tempo.bpm;
    g.downbeatSec = cell.tempo.downbeatSec;
    g.measured = cell.tempoState == HslinkTakeEntry::tempoMeasured && cell.tempo.valid;
    // The tempo cascade: no certain measurement, but a tempo from the file name or the
    // host. Beats from the file start; drags snap on it, the render is not asked to.
    if (cell.tempoState == HslinkTakeEntry::tempoMeasured && cell.resolved.isFallback())
    {
        g.bpm = cell.resolved.bpm;
        g.downbeatSec = cell.resolved.downbeatSec;
        g.measured = false;
        g.fallback = true;
    }
    return g;
}

// The take editor header: the cell tag, and "(est)" when the grid is the estimate
// (no file name or host tempo): "88.00 bpm (file)", "98.00 bpm (host)", "130.10 bpm  3 ms (est)".
// A user-set Fit to bars tempo outranks the host and the estimate (TempoResolve::applyFitBars).
template <typename Cell>
static Cell withFitBars (Cell cell, double userFitBpm)
{
    if (cell.tempoState == HslinkTakeEntry::tempoMeasured)
        cell.resolved = TempoResolve::applyFitBars (cell.resolved, userFitBpm, &cell.tempo);
    return cell;
}

template <typename Cell>
static juce::String editorTempoText (const Cell& cell)
{
    auto text = TakeGrid::tempoTagOf (cell.tempoState, cell.tempo, cell.resolved).text;
    if (cell.resolved.source == TempoResolve::Source::measured)
        text << " (est)";
    return text;
}

void HotStepLinkEditor::refreshTakes()
{
    proc.audition.collectGarbage();
    // Round 6: the grid is built from the processor's take entries only (TakeGrid::build),
    // with no file-system call: every listed take has a cell, keyed by its name. Saved,
    // not saved yet and save failed come from stable / copyFailed. A take with no lasting
    // copy acts on its temp file and its drag is refused, with a NOT SAVED badge once a
    // copy attempt has failed. The list outlives the job cards (a pruned card keeps its
    // takes here). getTakes copies the list, no disk IO.
    const auto notice = proc.getSaveNotice();
    takeModel = buildTakeModel();
    {
        // v0.5.19: a take that just arrived (generate result, Add as take, WAV drop) is
        // selected at once so Edit take and Export WAV are on. The first refresh only
        // records the keys, so a fresh launch does not select anything by itself.
        juce::String arrived;
        juce::StringArray now;
        for (const auto& cell : takeModel.cells)
        {
            // A take that registerTake is still handing over (listed, its bytes still
            // in <name>.wav.part) is not known yet: selecting it now would find no
            // file. It counts as arrived on the first refresh after the hand-over.
            bool inHandOver = cell.handingOver;
#if defined (HSLINK_TEST_HOOKS)
            inHandOver = inHandOver && ! testSelectInHandOver; // teeth: the code before
#endif
            if (inHandOver)
                continue;
            now.add (cell.key);
            if (takeKeysSeeded && cell.shown && ! knownTakeKeys.contains (cell.key))
                arrived = cell.key;
        }
        // D2: a loaded state replaced the list. Its takes are not arrivals (no auto-select),
        // and undo histories of takes no longer listed are forgotten.
        const auto restoreGen = proc.getTakesRestoreGeneration();
        if (restoreGen != lastTakesRestoreGen)
        {
            lastTakesRestoreGen = restoreGen;
            arrived = {};
            juce::StringArray listed;
            for (const auto& cell : takeModel.cells)
                listed.add (cell.temp.getFullPathName());
            takeEditor.forgetHistoriesExcept (listed);
        }
        knownTakeKeys = now;
        takeKeysSeeded = true;
        // An open take editor does not block it (Apply crop, stretch, Export loop add a
        // take while it is open): the new take is selected for audition, and the editor
        // stays on the take it edits.
#if defined (HSLINK_TEST_HOOKS)
        const bool noHeaderRepaint = noHeaderRepaintForTest;
#else
        const bool noHeaderRepaint = false;
#endif
#if defined (HSLINK_TEST_HOOKS)
        const bool openEditorBlocksSelect = openEditorBlocksSelectForTest;
#else
        const bool openEditorBlocksSelect = false;
#endif
#if defined (HSLINK_TEST_HOOKS)
        const bool noAutoSelect = noAutoSelectForTest;
#else
        const bool noAutoSelect = false;
#endif
        if (arrived.isNotEmpty() && ! (openEditorBlocksSelect && takeEditor.isOpen()) && ! noAutoSelect)
        {
            const auto before = proc.audition.getSelected();
            selectTake (arrived); // refreshes the whole take area itself
            if (proc.audition.getSelected() != before)
            {
                if (! noHeaderRepaint && takeList.getNumCells() != lastTakeCountShown)
                {
                    lastTakeCountShown = takeList.getNumCells();
                    repaint();
                }
                return;
            }
        }
    }
    takeList.setTakes (takeModel.shown(),
                       "Not saved: could not write to " + notice.stableFolder
                           + ". Kept in a temporary folder; you can still audition it and use Export WAV."
                             " Dragging to the DAW is off until it is saved.");
#if defined (HSLINK_TEST_HOOKS)
    const bool noHeaderRepaint = noHeaderRepaintForTest;
#else
    const bool noHeaderRepaint = false;
#endif
    if (! noHeaderRepaint && takeList.getNumCells() != lastTakeCountShown)
    {
        // The TAKES header count is painted by the editor: repaint in the same call.
        lastTakeCountShown = takeList.getNumCells();
        repaint();
    }
    {
        // Edit take: on while a take cell is selected. An open editor whose take left the list closes.
        bool anySelected = false, editedListed = false, selMissing = false;
        juce::String selKey, selLabel;
        for (const auto& cell : takeModel.cells)
        {
            anySelected = anySelected || (cell.selected && cell.shown);
            if (cell.selected && cell.shown && selKey.isEmpty())
            {
                selMissing = cell.missing;
                selKey = cell.key;
                selLabel = cell.label;
            }
            editedListed = editedListed || cell.temp == takeEditor.getTemp();
            // The open editor follows the take's tempo: a measurement that lands while it
            // is open would else leave the panel on the grid of the moment it was opened.
            if (takeEditor.isOpen() && cell.temp == takeEditor.getTemp())
            {
                const auto fc = withFitBars (cell, takeEditor.userFitBpm());
                takeEditor.setLiveGrid (takeEditorGridOf (fc), editorTempoText (fc));
                takeEditor.setLiveTempo (cell.tempoState == HslinkTakeEntry::tempoMeasured ? cell.tempo : TempoMeasure::Result {});
            }
        }
        editTakeButton.setEnabled (anySelected && ! selMissing);
        moreLikeKey = selKey;
        moreLikeLabel = selKey.isEmpty() ? juce::String() : selLabel;
        const auto moreText = moreLikeTextFor (moreLikeLabel, (float) moreLikeButton.getWidth() - 8.0f);
        if (moreLikeButton.getButtonText() != moreText)
            moreLikeButton.setButtonText (moreText);
        // Only with a known seed: a take whose job reported none (e.g. YuE2) cannot seed neighbours.
        bool seedKnown = false;
        if (selKey.isNotEmpty())
            if (const auto* c = takeModel.find (selKey))
            {
                DawJob job;
                if (proc.daw.getJob (c->jobId, job) && juce::isPositiveAndBelow (c->takeIndex, job.takeSeeds.size()))
                    seedKnown = job.takeSeeds[c->takeIndex] >= 0;
            }
        moreLikeButton.setEnabled (seedKnown);
        const juce::String moreTip = (selKey.isNotEmpty() && ! seedKnown)
                                         ? juce::String ("server did not report a seed")
                                         : juce::String ("Four new takes near the selected take (seeds just past its seed)");
        if (moreLikeButton.getTooltip() != moreTip)
            moreLikeButton.setTooltip (moreTip);
        if (takeEditor.isOpen() && ! editedListed && ! TakeEditorMath::teMutant (TakeEditorMath::mutNoSelfClose))
            closeTakeEditor (false);
    }
    const auto noticeText = notice.text();
    if (noticeText != lastNoticeText || (notice.unsaved > 0) != lastNoticeUnsaved)
    {
        lastNoticeText = noticeText;
        lastNoticeUnsaved = notice.unsaved > 0;
        saveNotice.setText (noticeText, juce::dontSendNotification);
        saveNotice.setTooltip (noticeText);
        saveNotice.setColour (juce::Label::textColourId, lastNoticeUnsaved ? juce::Colour (0xffffb347) : kText);
        saveNotice.setVisible (noticeText.isNotEmpty());
        retrySaveButton.setVisible (lastNoticeUnsaved);
        resized();
    }
    const auto pendingDelete = proc.getPendingTakeDelete();
    if (pendingDelete != lastPendingDelete)
    {
        lastPendingDelete = pendingDelete;
        const auto text = "Deleted " + pendingDelete + ". It moves to the Deleted folder in a few seconds.";
        deleteNotice.setText (text, juce::dontSendNotification);
        deleteNotice.setTooltip (text);
        deleteNotice.setVisible (pendingDelete.isNotEmpty());
        undoDeleteButton.setVisible (pendingDelete.isNotEmpty());
        resized();
    }
    {
        // From state: something is selected. The file is touched when Export is clicked.
        const auto sel = proc.audition.getSelected();
        bool selMissing = false;
        for (const auto& c : takeModel.cells)
            selMissing = selMissing || (c.selected && c.missing);
#if defined (HSLINK_TEST_HOOKS)
        const bool editorStats = TakeGrid::testEditorStats.load();
#else
        const bool editorStats = false;
#endif
        exportWavButton.setEnabled (sel != juce::File() && ! selMissing
                                    && (! editorStats || EditorFs::exists (sel, EditorFs::tagTick)));
        exportMidiButton.setEnabled (sel != juce::File() && ! midiBusy);
        const bool canDrag = sel != juce::File() && (! midiBusy || midiDrag.isBuilding());
        if (midiDrag.isEnabled() != canDrag)
        {
            midiDrag.setEnabled (canDrag);
            midiDrag.repaint();
        }
    }
    const float g = proc.audition.lastGain.load (std::memory_order_relaxed);
    if (std::abs (g - lastGainShown) > 1.0e-3f)
    {
        lastGainShown = g;
        gainLabel.setText ("Audition gain " + juce::String (juce::Decibels::gainToDecibels (g), 1) + " dB",
                           juce::dontSendNotification);
    }
}

void HotStepLinkEditor::refreshCollectionPanel()
{
    lastCollectionGen = proc.getCollectionGeneration();
    collectionPanel.setItems (proc.getCollection()); // cached copy, no disk IO
    collectionPanel.setSelectedFile (proc.audition.getSelected());
}

void HotStepLinkEditor::confirmRemoveFromCollection (const CollectionItem& item)
{
    // Async only: a blocking modal loop is not safe inside a plugin.
    const auto options = juce::MessageBoxOptions()
                             .withIconType (juce::MessageBoxIconType::QuestionIcon)
                             .withTitle ("Remove from Collection")
                             .withMessage ("Remove '" + item.name + "' from the Collection? The WAV and its info file move to the Deleted folder inside the Collection folder.")
                             .withButton ("Remove")
                             .withButton ("Cancel")
                             .withAssociatedComponent (this);
    juce::Component::SafePointer<HotStepLinkEditor> safe (this);
    juce::AlertWindow::showAsync (options, [safe, item] (int result)
    {
        if (safe == nullptr || result != 1)
            return;
        if (safe->proc.audition.getSelected() == item.wav)
            safe->proc.audition.select (juce::File(), safe->proc.getSourceFile()); // let go of the file first
        juce::String note;
        const bool removed = safe->proc.removeFromCollection (item, &note);
        // The note is the reason (false) or what stayed behind (true, rare); it is shown.
        safe->collectionPanel.showNote (CollectionStore::removeResultText (item.name, removed, note), ! removed || note.isNotEmpty());
    });
}

bool HotStepLinkEditor::openTakeEditor()
{
    // The selected cell, looked up now; the panel is keyed by the take's temp file.
    const auto model = buildTakeModel();
    for (const auto& cell : model.cells)
    {
        if (! cell.selected || ! cell.shown)
            continue;
        if (cell.missing)
        {
            collectionPanel.showNote ("Cannot edit: the take's file is missing: " + cell.file.getFullPathName(), true, 8000);
            return false;
        }
        TakeEditorPanel::Take t;
        t.temp = cell.temp;
        t.name = cell.key;
        const auto fc = withFitBars (cell, takeEditor.userFitBpm());
        t.tempoText = editorTempoText (fc);
        // The grid of the snap toggle and the beat ruler: the test of requestTakeEdit.
        t.grid = takeEditorGridOf (fc);
        for (const auto& e : proc.getTakes()) // a copy, no disk IO
            if (e.temp == cell.temp)
            {
                t.edit = e.edit;
                t.warpMarkers = e.warpMarkers; // C1: the stored warp
                t.warpMarkersEdited = e.warpMarkersEdited;
                t.warpTagBpm = e.warpTagBpm;
            }
        if (cell.tempoState == HslinkTakeEntry::tempoMeasured)
            t.tempo = cell.tempo; // F1: AUTO-WARP tracks beats on its onsets
        t.gridEdit = cell.gridEdit; // R1b: the hand grid stored on the take (setTakeGrid)
        if ((cell.tempoState == HslinkTakeEntry::tempoMeasured && cell.tempo.valid)
            || cell.tempoState == HslinkTakeEntry::tempoEstimated)
            t.takeBpm = cell.tempo.bpm > 0.0 ? cell.tempo.bpm : 0.0; // Fit to bars opens on the take tempo
        takeEditor.open (t);
#if defined (HSLINK_TEST_HOOKS)
        if (testKeyRevealMutant != 6) // teeth 6: the take editor leaves the key shown
#endif
        if (keyRevealed)
            setKeyRevealed (false); // the take editor covers SETTINGS: mask the key again (no save)
        takeEditor.setVisible (true);
        takeEditor.toFront (false);
#if defined (HSLINK_TEST_HOOKS)
        if (offlineMutantForTest == mutOverlayFullWindow && overlay.isVisible())
            overlay.toFront (false);
        else
#endif
            overlay.setVisible (false); // the offline banner sits on the Generate plate; the editor needs no server
        return true;
    }
    return false;
}

void HotStepLinkEditor::closeTakeEditor (bool restore)
{
    takeEditor.close (restore); // ends in onClosed, which hides the panel
}

TakeGrid::Model HotStepLinkEditor::buildTakeModel()
{
    auto m = TakeGrid::build (proc.getTakes(), selectedJob, DawWorker::unsavedGuard.load(), proc.audition.getSelected(),
                              [this] (const juce::File& f) { return proc.isStarred (f); });
    // Fit to bars at the host tempo (standalone: the manual BPM). The take tempo is the
    // measured one only; the asked-for tempo is the host tempo and would say nothing.
    const auto& hs = proc.hostSync;
    const double hostBpm = standalone ? manualBpm.getValue()
                                      : (hs.bpmKnown.load (std::memory_order_relaxed) ? hs.bpm.load (std::memory_order_relaxed) : 0.0);
    const int beatsPerBar = juce::jmax (1, hs.timeSigNum.load (std::memory_order_relaxed));
    for (auto& c : m.cells)
    {
        const double takeBpm = c.tempoState == HslinkTakeEntry::tempoMeasured && c.tempo.valid ? c.tempo.bpm : 0.0;
        // FIT option C: length vs the take's requested duration (its job request), tempo
        // folded against the host tempo. No requested duration: beat grid (option A).
        double requestedS = 0.0;
        if (DawJob job; c.jobId != 0 && proc.daw.getJob (c.jobId, job))
            requestedS = job.request.durationS;
        // Card 65840a44 option C: flag a take whose length is far from the asked duration, once.
        if (requestedS > 0.0 && c.durationS > 0.0 && ! durationNotedKeys.contains (c.key))
        {
            durationNotedKeys.add (c.key);
            DawJob job;
            proc.daw.getJob (c.jobId, job);
            const auto bid = job.request.expectedBackend.isNotEmpty() ? job.request.expectedBackend : proc.getSelectedBackendId();
            const auto note = DawJobRequest::durationMismatchNote (DawWorker::backendLabel (bid), requestedS, c.durationS);
            if (note.isNotEmpty())
            {
                takesNote = note;
                takesNoteUntilMs = juce::Time::getMillisecondCounter() + 8000;
            }
        }
        c.fitLen = TakeGrid::fitLengthScore (c.durationS, requestedS, hostBpm, beatsPerBar);
        c.fitTempo = TakeGrid::fitTempoScore (takeBpm, hostBpm);
        c.fitScore = TakeGrid::fitMinScore (c.fitLen, c.fitTempo);
    }
    return m;
}

void HotStepLinkEditor::selectTake (const juce::String& key)
{
    // The file is looked up now: the lasting copy when the take is saved, else the temp file.
    const auto model = buildTakeModel();
    const auto* c = model.find (key);
    if (c == nullptr)
        return; // the take left the list since the last refresh
    // Clicking the selected take again turns audition off (pass-through).
    const bool off = c->selected;
    if (! off && c->missing)
    {
        collectionPanel.showNote ("Cannot audition: the take's file is missing: " + c->file.getFullPathName(), true, 8000);
        return;
    }
    // One file check per click, so a take whose file is gone says so instead of
    // being selected in silence.
    // A saved take whose lasting file was removed by hand plays from its temp copy
    // while that is still there (TakeGrid::actionFileOf, at most two checks per click).
    bool found = true;
    const auto use = off ? juce::File() : TakeGrid::actionFileOf (*c, EditorFs::tagSelect, &found);
    if (! off && ! found)
    {
        collectionPanel.showNote ("Cannot audition: the take's file is missing: " + c->file.getFullPathName(), true, 8000);
        return;
    }
    proc.audition.select (use, proc.getSourceFile());
    proc.setPinnedTake (use); // the take list bound never drops it
    refreshTakes();
}

juce::File HotStepLinkEditor::dragFileFor (const juce::String& key)
{
    // From state, no stat: a take with a lasting copy hands out that path, any other
    // take is refused. A lasting file removed by hand is left to the host to report.
    const auto model = buildTakeModel();
    const auto* c = model.find (key);
    return c != nullptr && c->dragAllowed ? warpedFileFor (c->temp, c->stable) : juce::File();
}

juce::File HotStepLinkEditor::warpedFileFor (const juce::File& temp, const juce::File& src)
{
    // A warp is in effect only while the take editor shows this take with Warp on and a
    // marker moved (what Play hears). The render is TakeStretch::renderWarped, as Play,
    // written once as 32 bit float beside the lasting root and reused while the take
    // and the marker list stay the same.
#if defined (HSLINK_TEST_HOOKS)
    const bool noWarpExport = noWarpExportForTest;
#else
    const bool noWarpExport = false;
#endif
    if (noWarpExport || src == juce::File() || ! takeEditor.isOpen() || takeEditor.getTemp() != temp)
        return src;
    const auto markers = takeEditor.effectiveWarp();
    if (markers.empty())
        return src;
    juce::String sig (src.getFullPathName());
    for (const auto& m : markers)
        sig << ";" << (juce::int64) m.srcFrame << "," << (juce::int64) m.dstFrame;
    const auto root = proc.getStableRoot();
    const auto dir = root.getSiblingFile (root.getFileName() + "_warped");
    const auto out = dir.getChildFile (src.getFileNameWithoutExtension() + "_warp_" + juce::String::toHexString (sig.hashCode64()) + ".wav");
    if (out.existsAsFile())
        return out;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (new juce::FileInputStream (src), true));
    if (rd == nullptr || rd->lengthInSamples <= 0 || rd->lengthInSamples > (juce::int64) std::numeric_limits<int>::max())
        return {};
    juce::AudioBuffer<float> in ((int) rd->numChannels, (int) rd->lengthInSamples);
    rd->read (&in, 0, in.getNumSamples(), 0, true, true);
    const double sr = rd->sampleRate;
    rd.reset();
    const auto warped = TakeStretch::renderWarped (in, sr, markers);
    if (warped.getNumSamples() <= 0 || ! dir.createDirectory())
        return {};
    const auto part = out.withFileExtension ("wavpart");
    part.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> os (part.createOutputStream());
        if (os == nullptr)
            return {};
        auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions {}
                                              .withSampleRate (sr)
                                              .withNumChannels (warped.getNumChannels())
                                              .withBitsPerSample (32)
                                              .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (w == nullptr || ! w->writeFromAudioSampleBuffer (warped, 0, warped.getNumSamples()))
        {
            w.reset();
            part.deleteFile();
            return {};
        }
    }
#if defined (HSLINK_TEST_HOOKS)
    const bool noWarpTag = noWarpTagForTest;
#else
    const bool noWarpTag = false;
#endif
    if (! noWarpTag)
        WavTempoTag::inject (part, takeEditor.warpResultBpm());
    if (! part.moveFileTo (out))
    {
        part.deleteFile();
        return out.existsAsFile() ? out : juce::File();
    }
#if defined (HSLINK_TEST_HOOKS)
    const bool noWarpCacheCap = noWarpCacheCapForTest;
#else
    const bool noWarpCacheCap = false;
#endif
    if (! noWarpCacheCap)
        trimWarpCache (dir, out);
    return out;
}

void HotStepLinkEditor::trimWarpCache (const juce::File& dir, const juce::File& keep)
{
    // Only *_warp_*.wav in the app-owned cache folder. Files touched in the last minute
    // are kept too, so a path a drag is handing out right now is never pulled away.
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*_warp_*.wav");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getLastModificationTime() < b.getLastModificationTime(); });
    const auto now = juce::Time::getCurrentTime();
    const auto maxAge = juce::RelativeTime::days (kWarpCacheMaxAgeDays);
    int count = files.size();
    for (const auto& f : files)
    {
        const auto t = f.getLastModificationTime();
        const bool tooOld = now - t > maxAge;
        if (count <= kWarpCacheMax && ! tooOld)
            break;
        if (f == keep || now - t < juce::RelativeTime::minutes (1))
            continue;
        if (f.deleteFile())
            --count;
    }
}

#if defined (HSLINK_TEST_HOOKS)
int HotStepLinkEditor::takeCellMissingForTest (const juce::String& key) const
{
    const auto* c = static_cast<const TakeCell*> (takeList.cellForKey (key));
    if (c == nullptr)
        return -1;
    return c->isMissing() && c->getAlpha() < 0.9f ? 1 : 0;
}
#endif

juce::File HotStepLinkEditor::exportSource()
{
    auto src = proc.audition.getSelected();
    if (src == juce::File())
        return {};
    // A take selected before its lasting copy landed is exported from the lasting copy.
    // A saved take whose lasting file was removed by hand is exported from its temp copy
    // while that is still there: checked here, on the button press only. A take with no
    // lasting copy has one file, so it is not checked.
    for (const auto& c : buildTakeModel().cells)
        if (c.selected)
        {
            if (c.missing)
                return {};
            src = c.saved ? TakeGrid::actionFileOf (c, EditorFs::tagExport) : c.file;
            return warpedFileFor (c.temp, src);
        }
    return src;
}

void HotStepLinkEditor::exportWav()
{
    const auto src = exportSource();
    if (src == juce::File())
        return;
    exportChooser = std::make_unique<juce::FileChooser> (
        "Export take as WAV",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (DawWorker::takeNameOf (src)),
        "*.wav");
    exportChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                [this, src] (const juce::FileChooser& fc)
    {
        auto dest = fc.getResult();
        if (dest == juce::File())
            return;
        if (! dest.hasFileExtension ("wav"))
            dest = dest.withFileExtension ("wav");
        EditorFs::count (EditorFs::tagExport);
        if (dest != src && ! src.copyFileTo (dest))
        {
            // The one file check Export makes on the source, only after a failed copy,
            // to name the reason. Held in the Collection footer so a refresh cannot
            // overwrite it at once.
            const bool gone = ! EditorFs::exists (src, EditorFs::tagExport);
            jobLabel.setText ("Export failed: " + dest.getFullPathName(), juce::dontSendNotification);
            collectionPanel.showNote (gone ? "Export failed: the take's file is missing: " + src.getFullPathName()
                                           : "Export failed: could not write " + dest.getFullPathName(),
                                      true, 8000);
        }
        else
        {
            // Peak guard on export: a take edited or warped after import can be over the
            // ceiling again. A take already at or under it is written as copied.
            // dest == src is skipped: the take itself is never rewritten by an export.
            const float g = PeakGuard::applyToExport (src, dest);
            jobLabel.setText ("Exported " + dest.getFileName() + (g < 0.0f ? "  " + PeakGuard::text (g) : juce::String()),
                              juce::dontSendNotification);
            showExportResult (dest);
        }
    });
}

void HotStepLinkEditor::showExportResult (const juce::File& written, const juce::StringArray& names)
{
    // Same for Export WAV and Export MIDI: the full output folder stays on screen until the next export.
    // MIDI passes every file name it wrote: they follow the folder, and the tooltip lists them.
#if defined (HSLINK_TEST_HOOKS)
    if (testSkipExportResult)
        return;
#endif
    lastExportFile = written;
    const auto folder = written.getParentDirectory().getFullPathName();
    exportPathLabel.setText ("Saved to: " + folder + (names.isEmpty() ? juce::String() : "  (" + names.joinIntoString (", ") + ")"),
                             juce::dontSendNotification);
    exportPathLabel.setTooltip (names.isEmpty() ? written.getFullPathName()
                                                : folder + "\n" + names.joinIntoString ("\n"));
    const bool wasVisible = exportPathLabel.isVisible();
    exportPathLabel.setVisible (true);
    showExportButton.setVisible (true);
    if (! wasVisible)
        resized();
}

juce::StringArray HotStepLinkEditor::midiFilesForSelection() const
{
    if (proc.audition.getSelected() != lastMidiSelection || lastMidiFiles.isEmpty())
        return {};
    juce::StringArray out;
    for (const auto& path : lastMidiFiles)
        if (juce::File (path).existsAsFile())
            out.add (path);
    return out;
}

MidiBuildResult buildMidiFiles (const MidiBuildInputs& in, const juce::File& destDir)
{
    MidiBuildResult r;
    auto& tempoOut = r.tempo;
    auto& hitsOut = r.hits;
    auto& err = r.err;
    const auto& src = in.src;
    const auto& grid = in.grid;
    const auto& markers = in.markers;
    const double srcBpm = in.srcBpm;
    const bool hasTempo = in.hasTempo;
    const int beatsPerBar = in.beatsPerBar, sigDen = in.sigDen, mutant = in.mutant;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (new juce::FileInputStream (src), true));
    if (rd == nullptr || rd->lengthInSamples <= 0 || rd->lengthInSamples > (juce::int64) std::numeric_limits<int>::max())
        err = "could not read " + src.getFullPathName();
    else
    {
        const double sr = rd->sampleRate;
        juce::AudioBuffer<float> buf ((int) rd->numChannels, (int) rd->lengthInSamples);
        rd->read (&buf, 0, buf.getNumSamples(), 0, true, true);
        rd.reset();
        std::vector<float> mono ((size_t) buf.getNumSamples(), 0.0f);
        const float k = 1.0f / (float) juce::jmax (1, buf.getNumChannels());
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            const float* p = buf.getReadPointer (ch);
            for (size_t i = 0; i < mono.size(); ++i)
                mono[i] += p[i] * k;
        }
        std::vector<double> onSecs;
        for (const auto& on : TempoMeasure::onsets (mono.data(), mono.size(), sr))
            onSecs.push_back (on.sec);
        // Hits only from drum-like transients (horns and pads stay out of the hits file);
        // the notes track below still splits at every onset.
        auto hits = MidiExport::hitsFromOnsets (buf, sr, MidiExport::percussiveOnsets (buf, sr, onSecs));
        const auto pitch = MidiExport::trackMelody (mono.data(), (int) mono.size(), sr, onSecs);
        auto notes = pitch.notes;
        if (mutant == 2)
            for (auto& h : hits)
                h.sec += 1.0 / 400.0;
        // The loop / crop region in this file's frames (already warped time).
        const juce::int64 len = buf.getNumSamples();
        const juce::int64 r0 = juce::jlimit<juce::int64> (0, len, in.regionStart < 0 ? 0 : in.regionStart);
        const juce::int64 r1 = juce::jlimit<juce::int64> (r0, len, in.regionEnd < 0 ? len : in.regionEnd);
        const bool cut = r1 > r0 && (r0 > 0 || r1 < len);
        const double regionSec = cut ? (double) r0 / sr : 0.0;
        if (cut)
        {
            hits = MidiExport::hitsInRegion (hits, regionSec, (double) r1 / sr);
            notes = MidiExport::notesInRegion (notes, regionSec, (double) r1 / sr);
        }
        const double bpm = (hasTempo && mutant != 1) ? grid.bpm : MidiExport::kDefaultBpm;
        // The downbeat in the file's time: through the warp markers (piecewise linear).
        double down = grid.downbeatSec;
        if (markers.size() >= 2)
        {
            const double f = down * sr;
            for (size_t i = 0; i + 1 < markers.size(); ++i)
            {
                const double s0 = (double) markers[i].srcFrame, s1 = (double) markers[i + 1].srcFrame;
                if (f >= s0 && f <= s1 && s1 > s0)
                {
                    const double d0 = (double) markers[i].dstFrame, d1 = (double) markers[i + 1].dstFrame;
                    down = (d0 + (f - s0) / (s1 - s0) * (d1 - d0)) / sr;
                    break;
                }
            }
        }
        // One tempo per warp segment (empty map = the single tempo above).
        auto map = (hasTempo && mutant != 1) ? MidiExport::tempoMapFromWarp (markers, srcBpm, sr)
                                             : MidiExport::TempoMap();
        double bpmOut = bpm;
        if (cut)
        {
            map = MidiExport::shiftTempoMap (map, regionSec);
            if (map.size() == 1) // the region sits in one segment: that is its tempo
            {
                bpmOut = map[0].bpm;
                map.clear();
            }
            down -= regionSec;
            const double barSec = 60.0 / (map.empty() ? bpmOut : map[0].bpm) * MidiExport::quartersPerBar (beatsPerBar, sigDen);
            if (barSec > 0.0 && std::isfinite (barSec))
                while (down < 0.0)
                    down += barSec;
        }
        const auto& dir = destDir;
    dir.createDirectory();
        const auto name = src.getFileNameWithoutExtension();
        const double dur = (double) (cut ? r1 - r0 : len) / sr;
        const double origin = MidiExport::exportOriginTicks (hits, bpmOut, beatsPerBar, sigDen, map, down);
        // Chords (opt-in): analysed on the region, the detected key goes into every file.
        std::optional<MidiExport::KeySig> key;
        ChordExport::ChordResult chordRes;
        if (in.chords)
        {
            juce::AudioBuffer<float> region (buf.getNumChannels(), (int) juce::jmax<juce::int64> (1, r1 - r0));
            region.clear();
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                if (r1 > r0)
                    region.copyFrom (ch, 0, buf, ch, (int) r0, (int) (r1 - r0));
            chordRes = ChordExport::analyseChords (region, sr, bpmOut, down, beatsPerBar, sigDen, map,
                                                   in.chordMode == 1 ? ChordExport::Mode::PerBeat : ChordExport::Mode::PerBar);
            key = chordRes.key.sig;
        }
        // Build every asked-for part first; then write them apart, or as one multitrack file.
        const bool wantTempo = hasTempo && in.tempo;
        if (in.tempo && ! hasTempo)
            r.skipped.add ("tempo: the take has no tempo (hits at 120 BPM)");
        std::optional<juce::MidiFile> tempoF, hitsF, melodyF, chordsF;
        if (wantTempo)
            tempoF = MidiExport::buildTempoFile (bpmOut, down, beatsPerBar, dur, sigDen, key, map, origin);
        if (in.drums)
            hitsF = MidiExport::buildHitsFile (hits, bpmOut, beatsPerBar, sigDen, key, map,
                                               in.quantDiv, in.quantStrength, down);
        // Tonal takes also get a pitched note track (Auto: skipped with a reason otherwise).
        if (in.melody)
        {
            if (MidiExport::isTonal (pitch) && ! notes.empty())
                melodyF = MidiExport::buildNotesFile (notes, name + " melody", bpmOut, beatsPerBar, sigDen, key, map,
                                                      in.quantDiv, in.quantStrength, origin);
            else
                r.skipped.add ("melody: the take is not tonal");
        }
        if (in.chords)
        {
            if (! chordRes.chords.empty())
                chordsF = ChordExport::buildChordsFile (chordRes.chords, name + " chords", bpmOut, beatsPerBar, sigDen,
                                                        key, map, origin);
            else
                r.skipped.add ("chords: no chord found");
        }
        auto put = [&] (const juce::MidiFile& f, const juce::String& suffix, juce::File& out)
        {
            out = dir.getChildFile (name + suffix);
            if (! MidiExport::writeFile (f, out))
            {
                err = "could not write " + out.getFullPathName();
                out = juce::File();
            }
        };
        if (in.combined)
        {
            // Track 0: the tempo file's conductor (tempo, meter, key, bar markers), else a
            // plain tempo track; then track 1 of each part (each part's own track 0 is the
            // same tempo track, so it is dropped).
            if (hitsF || melodyF || chordsF || tempoF)
            {
                juce::MidiFile all;
                all.setTicksPerQuarterNote (MidiExport::kPpq);
                if (tempoF)
                    all.addTrack (*tempoF->getTrack (0));
                else
                    all.addTrack (MidiExport::tempoTrack (bpmOut, beatsPerBar, sigDen, key, map, origin));
                for (const auto* f : { &hitsF, &melodyF, &chordsF })
                    if (f->has_value() && (*f)->getNumTracks() >= 2)
                        all.addTrack (*(*f)->getTrack (1));
                put (all, "_multitrack.mid", r.combined);
            }
        }
        else
        {
            if (tempoF)
                put (*tempoF, "_tempo.mid", tempoOut);
            if (hitsF)
                put (*hitsF, "_hits.mid", hitsOut);
            if (melodyF)
                put (*melodyF, "_melody.mid", r.notes);
            if (chordsF)
                put (*chordsF, "_chords.mid", r.chords);
        }
        if (err.isEmpty() && r.files().isEmpty())
            err = r.skipped.isEmpty() ? juce::String ("nothing to export: pick a part in the MIDI picker")
                                      : "nothing to export (" + r.skipped.joinIntoString ("; ") + ")";
    }
    return r;
}

juce::StringArray buildMidiFilesForDrag (const MidiBuildInputs& in, const juce::File& destDir)
{
    const auto r = buildMidiFiles (in, destDir);
    juce::StringArray out;
    if (r.err.isNotEmpty())
        return out;
    for (const auto& f : r.files())
        if (f.existsAsFile())
            out.add (f.getFullPathName());
    return out;
}

juce::File midiDragTempRoot()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("HOTSTEP_LINK_midi");
}

int clearMidiDragTempRoot (const juce::File& root, juce::RelativeTime minAge, juce::Time now)
{
    if (now == juce::Time())
        now = juce::Time::getCurrentTime();
    if (root.getFileName() != "HOTSTEP_LINK_midi" || ! root.isDirectory())
        return -1;
#if defined (HSLINK_TEETH_MIDI_CLEAN_PARENT)
    const auto target = root.getParentDirectory();
#else
    const auto& target = root;
#endif
    int left = 0;
    for (const auto& c : target.findChildFiles (juce::File::findFilesAndDirectories, false, "*"))
    {
#if ! defined (HSLINK_TEETH_MIDI_CLEAN_FRESH)
        // another LINK window may be writing this one right now: only clear children over minAge old
        if (now - c.getLastModificationTime() < minAge)
            continue;
#endif
        if (! (c.isDirectory() ? c.deleteRecursively (false) : c.deleteFile()))
            ++left;
    }
    return left;
}

juce::File midiDragTempDir()
{
    auto d = midiDragTempRoot()
                 .getNonexistentChildFile ("drag_" + juce::String (juce::Time::currentTimeMillis()), {}, false);
    d.createDirectory();
    return d;
}

bool HotStepLinkEditor::midiInputsForSelection (MidiBuildInputs& in)
{
    const auto sel = proc.audition.getSelected();
    if (sel == juce::File())
        return false;
    in.quantDiv = quantizeBox.getSelectedId() == 2 ? 16 : quantizeBox.getSelectedId() == 3 ? 8 : 0;
    in.quantStrength = quantStrengthSlider.getValue() / 100.0;
    // The file Export WAV reads (a warped copy while a warp is in effect), and the grid
    // of that file: the take's measured tempo, or with a warp the tempo the warp tags
    // its result at and the downbeat moved through the markers.
    juce::File src;
    TakeEditorMath::Grid grid;
    std::vector<WarpMarkers::Marker> markers;
    double srcBpm = 0.0; // the unwarped take's tempo, for the per-segment tempo map
    juce::int64 regionStart = -1, regionEnd = -1;
    for (const auto& c : buildTakeModel().cells)
        if (c.selected)
        {
            const auto base = c.saved ? TakeGrid::actionFileOf (c, EditorFs::tagExport) : c.file;
            src = warpedFileFor (c.temp, base);
            grid = takeEditorGridOf (c);
            // The loop, else the crop, in frames of the take (the source of any warp).
            for (const auto& e : proc.getTakes()) // a copy, no disk IO
                if (e.temp == c.temp)
                {
                    const auto ed = e.edit.sanitized();
                    if (ed.hasLoop())
                    {
                        regionStart = ed.loopStart;
                        regionEnd = ed.loopEnd;
                    }
                    else if (ed.hasCrop())
                    {
                        regionStart = ed.cropStart;
                        regionEnd = ed.cropEnd;
                    }
                }
            if (src != base)
            {
                markers = takeEditor.effectiveWarp();
                grid.bpm = takeEditor.warpResultBpm();
                // The source tempo the warp's result tempo was taken from, so the
                // per-segment map averages to exactly that result tempo.
                if (markers.size() >= 2)
                {
                    const double s = (double) (markers.back().srcFrame - markers.front().srcFrame);
                    const double d = (double) (markers.back().dstFrame - markers.front().dstFrame);
                    srcBpm = s > 0.0 && d > 0.0 ? grid.bpm * d / s : 0.0;
                }
                // The region into the warped file's frames (onsets are read there).
                if (regionStart >= 0)
                    regionStart = (juce::int64) WarpMarkers::mapFrame (markers, regionStart, true);
                if (regionEnd >= 0)
                    regionEnd = (juce::int64) WarpMarkers::mapFrame (markers, regionEnd, true);
            }
            break;
        }
    if (src == juce::File())
        src = sel;
    const bool hasTempo = grid.bpm > 0.0 && std::isfinite (grid.bpm) && (grid.measured || grid.fallback);
    const int beatsPerBar = juce::jmax (1, proc.hostSync.timeSigNum.load (std::memory_order_relaxed));
    const int sigDen = MidiExport::validDen (proc.hostSync.timeSigDen.load (std::memory_order_relaxed));
    in.src = src;
    in.grid = grid;
    in.markers = markers;
    in.srcBpm = srcBpm;
    in.hasTempo = hasTempo;
    in.beatsPerBar = beatsPerBar;
    in.sigDen = sigDen;
    in.regionStart = regionStart;
    in.regionEnd = regionEnd;
    return true;
}

void HotStepLinkEditor::exportMidi()
{
    const auto sel = proc.audition.getSelected();
    if (sel == juce::File() || midiBusy)
        return;
    MidiBuildInputs in;
    if (! midiInputsForSelection (in))
        return;
#if defined (HSLINK_TEST_HOOKS)
    in.mutant = midiMutantForTest;
#endif
    applyMidiPicker (in);
    midiBusy = true;
    exportMidiButton.setEnabled (false);
    setMidiPickerOpen (false);
    jobLabel.setText ("Exporting MIDI...", juce::dontSendNotification);
    juce::Component::SafePointer<HotStepLinkEditor> safe (this);
#if defined (HSLINK_TEST_HOOKS)
    const bool midiSync = midiSyncForTest;
#else
    const bool midiSync = false;
#endif
    const bool sync = midiSync;
    auto work = [safe, sel, in, sync]
    {
        const auto r = buildMidiFiles (in, in.src.getParentDirectory());
        auto done = [safe, sel, r]
        {
            if (safe == nullptr)
                return;
            auto& ed = *safe;
            ed.midiBusy = false;
            ed.lastMidiSelection = sel;
            ed.lastMidiTempo = r.tempo;
            ed.lastMidiHits = r.hits;
            ed.lastMidiNotes = r.notes;
            ed.lastMidiFiles.clear();
            juce::StringArray names;
            for (const auto& f : r.files())
            {
                ed.lastMidiFiles.add (f.getFullPathName());
                names.add (f.getFileName());
            }
            if (r.err.isNotEmpty())
            {
                ed.jobLabel.setText ("MIDI export failed", juce::dontSendNotification);
                ed.collectionPanel.showNote ("MIDI export failed: " + r.err, true, 8000);
                return;
            }
            const auto first = r.files().getFirst();
            juce::String note ("Exported MIDI to " + first.getParentDirectory().getFullPathName() + ": "
                               + names.joinIntoString (", "));
            if (! r.skipped.isEmpty())
                note << " (skipped " << r.skipped.joinIntoString ("; ") << ")";
            ed.jobLabel.setText ("Exported " + juce::String (names.size()) + (names.size() == 1 ? " MIDI file" : " MIDI files"),
                                 juce::dontSendNotification);
            ed.collectionPanel.showNote (note, false, 8000);
            ed.showExportResult (first, names);
        };
        if (sync)
            done();
        else
            juce::MessageManager::callAsync (done);
    };
    if (sync)
        work();
    else
        juce::Thread::launch (work);
}

bool HotStepLinkEditor::buildMidiForDrag()
{
    const auto sel = proc.audition.getSelected();
    if (sel == juce::File() || midiBusy)
        return false;
    MidiBuildInputs in;
    if (! midiInputsForSelection (in))
        return false;
    applyMidiPicker (in); // the drag chip carries the same files as Export MIDI
    midiBusy = true;
    exportMidiButton.setEnabled (false);
    jobLabel.setText ("Building MIDI for drag...", juce::dontSendNotification);
    juce::Component::SafePointer<HotStepLinkEditor> safe (this);
    juce::Thread::launch ([safe, sel, in]
    {
        const auto r = buildMidiFiles (in, midiDragTempDir());
        juce::MessageManager::callAsync ([safe, sel, r]
        {
            if (safe == nullptr)
                return;
            auto& ed = *safe;
            ed.midiBusy = false;
            juce::StringArray files;
            if (r.err.isEmpty())
            {
                ed.lastMidiSelection = sel;
                ed.lastMidiTempo = r.tempo;
                ed.lastMidiHits = r.hits;
                ed.lastMidiNotes = r.notes;
                ed.lastMidiFiles.clear();
                for (const auto& f : r.files())
                    ed.lastMidiFiles.add (f.getFullPathName());
                files = ed.midiFilesForSelection();
                ed.jobLabel.setText ("MIDI ready to drag", juce::dontSendNotification);
            }
            else
            {
                ed.jobLabel.setText ("MIDI build failed", juce::dontSendNotification);
                ed.collectionPanel.showNote ("MIDI build failed: " + r.err, true, 8000);
            }
            ed.midiDrag.buildFinished (files);
        });
    });
    return true;
}

void MidiDragChip::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const auto c = isEnabled() ? kAccent : kDim.withAlpha (0.5f);
    g.setColour (c.withAlpha (0.15f));
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (c);
    g.drawRoundedRectangle (r, 3.0f, 1.0f);
    g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
    g.drawText (building ? "..." : "MIDI", getLocalBounds(), juce::Justification::centred);
}

void MidiDragChip::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStarted || ! isEnabled() || e.getDistanceFromDragStart() < 6 || filesFor == nullptr)
        return;
    if (building)
        return;
    const auto files = filesFor();
    if (files.isEmpty())
    {
        // Not exported yet: build to a temp folder off the message thread, drag when ready.
        if (requestBuild != nullptr && requestBuild())
        {
            dragStarted = true;
            building = true;
            repaint();
        }
        return;
    }
    dragStarted = true;
    juce::DragAndDropContainer::performExternalDragDropOfFiles (files, false, this);
}

void MidiDragChip::buildFinished (const juce::StringArray& files)
{
    building = false;
    repaint();
    // Only start the OS drag while the button is still held; otherwise the files stay
    // ready and the next drag starts at once.
    if (files.isEmpty() || ! juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown())
        return;
    dragStarted = true;
    juce::DragAndDropContainer::performExternalDragDropOfFiles (files, false, this);
}

void HotStepLinkEditor::applyModeGating (const DawStatus& st)
{
    // Repaint needs a non-turbo DiT, lego needs the base model.
    const bool nonTurbo = st.online && st.nonTurboAvailable;
    const bool base = st.online && st.baseAvailable;
    if (gatingInit && nonTurbo == lastNonTurbo && base == lastBase && st.online == lastOnline)
        return;
    gatingInit = true;
    lastNonTurbo = nonTurbo;
    lastBase = base;
    lastOnline = st.online;
    applyBackendRefLock();
}

float HotStepLinkEditor::keySaveNoteWidth (const juce::String& text)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText (juce::Font (juce::FontOptions (10.0f)), text, 0.0f, 0.0f);
    return ga.getBoundingBox (0, -1, true).getWidth();
}

juce::String HotStepLinkEditor::keySaveNoteTextFor (float availWidth)
{
    for (const char* t : { "Key not saved to disk. It works until you close LINK.", "Not saved to disk. Works until LINK closes." })
        if (keySaveNoteWidth (t) <= availWidth)
            return t;
    return {};
}

void HotStepLinkEditor::noteKeySave (bool saveTried, bool stored)
{
    // Never reads or shows the key: only whether the save was tried and what it returned.
#if defined (HSLINK_TEST_HOOKS)
    const auto teeth = juce::SystemStats::getEnvironmentVariable ("HSLINK_TEETH", {});
    if (teeth == "RESULT_IGNORED")
        return; // teeth: the handler ignores the result, as before
    if (teeth == "CLEAR_ON_UNCHANGED" && ! saveTried)
        keySaveNote.setVisible (false); // teeth: an unchanged key clears the note
#endif
    if (! saveTried)
        return; // unchanged key, nothing was saved: neither shows nor clears
    keySaveNote.setVisible (! stored);
}

juce::String HotStepLinkEditor::refLockNoteText (const juce::String& backendId)
{
    if (backendId.isEmpty())
        return "pick a backend to use reference audio";
    return backendId == "ace" ? juce::String() : juce::String ("reference audio needs ACE-Step");
}

void HotStepLinkEditor::dismissRefHint()
{
    if (! refHintSeen)
    {
        // Seen for this session whatever the write returned (the hint stays hidden after a
        // click). A write that failed here is tried again by the processor on the next
        // finished capture, one try per capture, so this never loops from the poll.
        refHintSeen = true;
        HotStepLinkProcessor::saveRefHintSeen();
    }
    sourceWave.setRefHint (false, standalone);
    sourceWave.setTooltip ("Captured source clip (repaint / lego)");
}

// Only ACE-Step reads reference audio (src_audio): repaint, lego and the strength box lock
// for any other backend, with a note beside the row saying why. Message thread.
void HotStepLinkEditor::applyBackendRefLock()
{
    const auto bid = proc.getSelectedBackendId();
    const bool ace = bid == "ace";
    const bool autoB = bid.isEmpty();
    const bool nonTurbo = lastNonTurbo && ace;
    const bool base = lastBase && ace;
    modeBox.setItemEnabled (2, nonTurbo);
    modeBox.setItemEnabled (3, base);
    juce::String tip ("text2music: always available");
    if (! ace)
        tip << "\nrepaint, lego: " << (autoB ? "pick a backend to use reference audio" : "need the ACE-Step backend");
    else
        tip << "\nrepaint: " << (lastNonTurbo ? "available" : "needs a non-turbo DiT model on the server")
            << "\nlego: " << (lastBase ? "available" : "needs the base DiT model on the server");
    modeBox.setTooltip (tip);
    strengthEditor.setEnabled (ace);
    strengthEditor.setReadOnly (! ace);
    // Locked text uses the same dimmed tone as disabled button text (kDim at 0.6).
    strengthEditor.applyColourToAllText (ace ? kText : kDim.withAlpha (0.6f));
    strengthEditor.setColour (juce::TextEditor::textColourId, ace ? kText : kDim.withAlpha (0.6f));
    const auto note = refLockNoteText (bid);
    if (note.isNotEmpty())
        refLockNote.setText (note, juce::dontSendNotification);
    refLockNote.setVisible (note.isNotEmpty());
    const int id = modeBox.getSelectedId();
    if ((id == 2 && ! nonTurbo) || (id == 3 && ! base))
    {
        // Leaving ACE-Step: keep the ACE mode so switching back restores it.
        if (! ace)
            proc.setAceModeId (id);
        modeBox.setSelectedId (1, juce::dontSendNotification);
        proc.setModeId (1);
    }
    else if (ace && id == 1)
    {
        const int saved = proc.getAceModeId();
        if ((saved == 2 && nonTurbo) || (saved == 3 && base))
        {
            modeBox.setSelectedId (saved, juce::dontSendNotification);
            proc.setModeId (saved);
        }
    }
}

//==============================================================================
OfflineOverlay::OfflineOverlay()
{
    message.setJustificationType (juce::Justification::centred);
    message.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    message.setColour (juce::Label::textColourId, kText);
    detail.setJustificationType (juce::Justification::centred);
    detail.setColour (juce::Label::textColourId, kText.withAlpha (0.6f));
    retryButton.setComponentID ("pink");
    addAndMakeVisible (message);
    addAndMakeVisible (detail);
    attemptsLabel.setJustificationType (juce::Justification::centredLeft);
    attemptsLabel.setFont (juce::FontOptions (11.0f));
    attemptsLabel.setColour (juce::Label::textColourId, kText.withAlpha (0.55f));
    attemptsLabel.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (attemptsLabel);
    addAndMakeVisible (retryButton);
}

void OfflineOverlay::setMessage (const juce::String& url, const juce::String& err, const juce::StringArray& attempts)
{
    const auto a = attempts.joinIntoString (juce::newLine);
    if (attemptsLabel.getText() != a)
        attemptsLabel.setText (a, juce::dontSendNotification);
    const auto m = "HOT-Step not reachable at " + url;
    if (message.getText() != m)
        message.setText (m, juce::dontSendNotification);
    const auto d = err.isNotEmpty() ? "Last error: " + err + "  (retrying)" : juce::String ("(retrying)");
    if (detail.getText() != d)
        detail.setText (d, juce::dontSendNotification);
}

void OfflineOverlay::paint (juce::Graphics& g)
{
    // A banner over the Generate controls: opaque enough to read, framed so it reads as one.
    const auto b = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (kBg.withAlpha (0.95f));
    g.fillRoundedRectangle (b, 6.0f);
    g.setColour (kText.withAlpha (0.25f));
    g.drawRoundedRectangle (b, 6.0f, 1.0f);
}

void OfflineOverlay::resized()
{
    message.setMinimumHorizontalScale (0.6f);
    detail.setMinimumHorizontalScale (0.6f);
    auto r = getLocalBounds().withSizeKeepingCentre (juce::jmin (getWidth() - 20, 560), juce::jmin (getHeight() - 8, 180));
    message.setBounds (r.removeFromTop (34));
    detail.setBounds (r.removeFromTop (28));
    r.removeFromTop (6);
    retryButton.setBounds (r.removeFromTop (30).withSizeKeepingCentre (110, 30));
    r.removeFromTop (6);
    attemptsLabel.setBounds (r); // last attempts, newest at the bottom
}

void HotStepLinkEditor::paintSection (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title,
                                      int count) const
{
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.setColour (kText.withAlpha (0.85f));
    juce::String t;
    for (auto ch : title.toUpperCase())
        t << juce::String::charToString (ch);
    juce::GlyphArrangement ga;
    ga.addLineOfText (g.getCurrentFont(), t, 0.0f, 0.0f);
    const int tw = (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth());
    g.drawText (t, r.removeFromLeft (tw + 2), juce::Justification::centredLeft);
    if (count >= 0)
    {
        g.setColour (kAccent);
        g.drawText (juce::String (count), r.removeFromLeft (30).withTrimmedLeft (6), juce::Justification::centredLeft);
    }
}

void HotStepLinkEditor::paintSoon (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title,
                                   const juce::String& hint) const
{
    if (r.getHeight() < 20 || r.getWidth() < 80)
        return;
    auto rf = r.toFloat().reduced (0.5f);
    g.setColour (kField.withAlpha (0.35f));
    g.fillRoundedRectangle (rf, 5.0f);
    juce::Path p;
    p.addRoundedRectangle (rf, 5.0f);
    const float dash[] = { 3.0f, 3.0f };
    juce::Path d;
    juce::PathStrokeType (1.0f).createDashedStroke (d, p, dash, 2);
    g.setColour (kEdge);
    g.fillPath (d);
    auto in = r.reduced (8, 4);
    auto tag = in.removeFromRight (78);
    auto tagR = tag.withSizeKeepingCentre (78, 16).toFloat();
    g.setColour (kDim.withAlpha (0.18f));
    g.fillRoundedRectangle (tagR, 8.0f);
    g.setColour (kDim);
    g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
    g.drawText ("COMING SOON", tagR, juce::Justification::centred);
    g.setColour (kDim.withAlpha (0.75f));
    if (in.getHeight() >= 32)
    {
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (title, in.removeFromTop (16), juce::Justification::centredLeft, true);
        g.setColour (kDim.withAlpha (0.5f));
        g.setFont (juce::FontOptions (10.0f));
        g.drawText (hint, in.removeFromTop (16), juce::Justification::centredLeft, true);
    }
    else
    {
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (title, in, juce::Justification::centredLeft, true);
    }
}

// Header bars child: repaints the plate, grid and separator under it, then the bars.
void HotStepLinkEditor::paintHeaderBars (juce::Graphics& g)
{
    const auto hb = headerArea;
    g.setColour (juce::Colour (0xff0a0a0f));
    g.fillRect (headerBars.getBounds());
    g.setColour (juce::Colour (0xff1a1a2e).withAlpha (0.8f));
    for (int x = hb.getX() + 14; x < hb.getRight(); x += 14)
        g.drawVerticalLine (x, (float) hb.getY(), (float) hb.getBottom());
    for (int y = hb.getY() + 14; y < hb.getBottom(); y += 14)
        g.drawHorizontalLine (y, (float) hb.getX(), (float) hb.getRight());
    g.setColour (juce::Colour (0xff3b2a5e));
    g.drawHorizontalLine (hb.getBottom() - 1, 0.0f, (float) getWidth());

    const auto tb = titleLabel.getBounds();
    {
        // Beat grid: 16 bars, downbeat bars tall, a decaying ramp within each beat.
        const int gx = tb.getX() + 112, gw = juce::jmax (0, tb.getRight() - gx);
        if (gw > 60)
        {
            const float top = (float) hb.getY() + 10.0f, bot = (float) hb.getBottom() - 10.0f;
            const float step = (float) gw / 16.0f;
            for (int i = 0; i <= 16; i += 4)
            {
                g.setColour (juce::Colour (0xff3b2a5e));
                g.fillRect ((float) gx + step * (float) i - (i % 16 == 0 ? 1.0f : 0.5f), top,
                            i % 16 == 0 ? 2.0f : 1.0f, bot - top);
            }
            // Light animation: a step cursor sweeps the 16 bars at 5 steps a second
            // (headerAnimFrame advances at 20 fps); each bar swells as it passes.
            const float sweep = std::fmod ((float) headerAnimFrame * 0.25f, 16.0f);
            for (int i = 0; i < 16; ++i)
            {
                const float d = std::fmod (sweep - (float) i + 16.0f, 16.0f);
                const float glow = headerAnimOn ? std::exp (-0.7f * d) : 0.0f;
                const float frac = ((i % 4 == 0) ? 0.95f : 0.85f - 0.12f * (float) (i % 4)) * (0.82f + 0.18f * glow);
                const float h = (bot - top - 4.0f) * frac;
                g.setColour (kAccent.withAlpha (juce::jmin (1.0f, (i % 4 == 0 ? 0.85f : 0.4f) * (0.8f + 0.4f * glow))));
                g.fillRect ((float) gx + step * (float) i + 1.5f, bot - h, juce::jmax (2.0f, step * 0.6f), h);
            }
        }
    }
}

void HotStepLinkEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);

    // Header strip
    // Header B "Beat Grid": dark plate, faint square grid, glowing LINK wordmark,
    // a 16-step bar motif between two bar lines.
    {
        const auto hb = headerArea;
        g.setColour (juce::Colour (0xff0a0a0f));
        g.fillRect (hb);
        g.setColour (juce::Colour (0xff1a1a2e).withAlpha (0.8f));
        for (int x = hb.getX() + 14; x < hb.getRight(); x += 14)
            g.drawVerticalLine (x, (float) hb.getY(), (float) hb.getBottom());
        for (int y = hb.getY() + 14; y < hb.getBottom(); y += 14)
            g.drawHorizontalLine (y, (float) hb.getX(), (float) hb.getRight());
        g.setColour (juce::Colour (0xff3b2a5e));
        g.drawHorizontalLine (hb.getBottom() - 1, 0.0f, (float) getWidth());

        const auto tb = titleLabel.getBounds();
        auto word = juce::Rectangle<int> (tb.getX(), hb.getY() + 4, 96, 30);
        juce::Font wf (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 28.0f, juce::Font::bold));
        wf.setExtraKerningFactor (0.14f);
        g.setFont (wf);
        g.setColour (kAccent.withAlpha (0.18f)); // soft glow
        for (int dx = -2; dx <= 2; dx += 2)
            for (int dy = -2; dy <= 2; dy += 2)
                if (dx != 0 || dy != 0)
                    g.drawText ("LINK", word.translated (dx, dy), juce::Justification::centredLeft, false);
        g.setColour (juce::Colour (0xfff3e8ff));
        g.drawText ("LINK", word, juce::Justification::centredLeft, false);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
        g.setColour (juce::Colour (0xff9d7cd8));
        g.drawText ("for HOT-Step", word.getX() + 2, word.getBottom() - 1, 110, 14, juce::Justification::centredLeft, false);

        // Beat grid: drawn by the headerBars child (paintHeaderBars).
    }

    // Server pill
    {
        auto pill = serverLabel.getBounds().expanded (8, 0).withX (serverLabel.getX() - 22).toFloat();
        pill.setWidth ((float) serverLabel.getRight() + 8.0f - pill.getX());
        const auto c = pillState == PillState::rendering ? kAccent
                     : pillState == PillState::queued    ? kAmberA
                     : pillState == PillState::idle      ? kOk
                                                         : kBad;
        g.setColour (c.withAlpha (0.08f));
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
        g.setColour (c.withAlpha (0.45f));
        g.drawRoundedRectangle (pill, pill.getHeight() * 0.5f, 1.0f);
        g.setColour (c);
        g.fillEllipse (pill.getX() + 10.0f, pill.getCentreY() - 3.5f, 7.0f, 7.0f);
    }

    // Plates
    for (auto p : { setupPlate, genPlate, queuePlate })
    {
        g.setColour (kPanel);
        g.fillRect (p);
        g.setColour (kEdge);
        g.drawRect (p.expanded (1, 0), 1);
    }
    const int inset = 12;
    paintSection (g, genPlate.reduced (inset, 0).withTop (genPlate.getY() + 8).withHeight (20), "Generate", -1);
    // Mockup A right column: TAKES on top, QUEUE under it. PREVIEW sits under GENERATE.
    paintSection (g, queuePlate.reduced (inset, 0).withY (jobCard.getY() - 22).withHeight (20), "Queue",
                  queueList.getNumActiveCards());
    paintSection (g, queuePlate.reduced (inset, 0).withY (takeList.getY() - 22).withHeight (20).withTrimmedRight (100),
                  "Takes", takeList.getNumCells());
    paintSection (g, previewHeaderArea(), "Preview", -1);

    // SOURCE panel (Mockup A, left column) and the GENERATE knob row card
    if (! sourcePanel.isEmpty())
        paintSection (g, setupPlate.reduced (inset, 0).withY (sourcePanel.getY() - 22).withHeight (20), "Source", -1);
    if (! syncPanel.isEmpty())
    {
        paintSection (g, setupPlate.reduced (inset, 0).withY (syncPanel.getY() - 22).withHeight (20), "Host sync", -1);
        auto sp = syncPanel.reduced (10, 6);
        const int cw = sp.getWidth() / 3;
        const juce::String caps[] = { "TEMPO", "METER", "BARS" };
        const juce::String vals[] = { syncTempoText, syncMeterText, syncBarsText };
        for (int i = 0; i < 3; ++i)
        {
            auto c = sp.withX (sp.getX() + i * cw).withWidth (cw);
            g.setColour (kDim);
            g.setFont (juce::FontOptions (10.5f));
            g.drawText (caps[i], c.removeFromTop (15), juce::Justification::centredLeft);
            g.setColour (kBright);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (vals[i], c, juce::Justification::centredLeft);
        }
    }
    if (! understandPanel.isEmpty())
        paintSection (g, setupPlate.reduced (inset, 0).withY (understandPanel.getY() - 22).withHeight (20), "Understand", -1);
    if (! inputPanel.isEmpty())
        paintSection (g, setupPlate.reduced (inset, 0).withY (inputPanel.getY() - 22).withHeight (20), "Input", -1);
    for (auto box : { sourcePanel, knobPanel, syncPanel, understandPanel, inputPanel })
    {
        if (box.isEmpty())
            continue;
        g.setColour (kField.withAlpha (0.6f));
        g.fillRoundedRectangle (box.toFloat(), 5.0f);
        g.setColour (kEdge);
        g.drawRoundedRectangle (box.toFloat().reduced (0.5f), 5.0f, 1.0f);
    }
    if (! understandPanel.isEmpty())
    {
        auto u = understandPanel.reduced (10, 6);
        auto kv = [&] (const juce::String& cap, const juce::String& val, bool accent)
        {
            auto line = u.removeFromTop (22);
            g.setColour (kDim);
            g.setFont (juce::FontOptions (10.5f));
            g.drawText (cap, line, juce::Justification::centredLeft);
            g.setColour (accent ? kAccent : kBright);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (val, line, juce::Justification::centredRight);
        };
        kv ("KEY", understandKeyText, true);
        kv ("TEMPO EST.", understandTempoText, false);
        g.setColour (kDim);
        g.setFont (juce::FontOptions (10.5f));
        g.drawText ("TAGS", u.removeFromTop (18), juce::Justification::centredLeft);
        auto chips = u.removeFromTop (22).toFloat();
        if (understandTags.isEmpty())
        {
            g.drawText ("none yet", chips, juce::Justification::centredLeft);
        }
        else
        {
            g.setFont (juce::FontOptions (11.0f));
            for (const auto& tag : understandTags)
            {
                const float w = 14.0f + 7.0f * (float) tag.length();
                if (chips.getWidth() < w)
                    break;
                auto c = chips.removeFromLeft (w);
                chips.removeFromLeft (6.0f);
                g.setColour (kAccent.withAlpha (0.15f));
                g.fillRoundedRectangle (c, c.getHeight() * 0.5f);
                g.setColour (kAccent.withAlpha (0.6f));
                g.drawRoundedRectangle (c.reduced (0.5f), c.getHeight() * 0.5f, 1.0f);
                g.setColour (kBright);
                g.drawText (tag, c, juce::Justification::centred);
            }
        }
    }
    if (! inputPanel.isEmpty())
    {
        auto u = inputPanel.reduced (10, 6);
        auto line = u.removeFromTop (19);
        g.setColour (kDim);
        g.setFont (juce::FontOptions (10.5f));
        g.drawText ("SOURCE", line, juce::Justification::centredLeft);
        g.setColour (kText);
        g.drawText (standalone ? "dropped WAV" : "track input (pre-FX)", line, juce::Justification::centredRight);
        line = u.removeFromTop (19);
        g.setColour (kDim);
        g.drawText ("LEVEL", line, juce::Justification::centredLeft);
        auto bar = line.withTrimmedLeft (line.getWidth() / 2).withSizeKeepingCentre (line.getWidth() / 2, 5).toFloat();
        g.setColour (juce::Colour (0xff1c2131));
        g.fillRoundedRectangle (bar, 2.5f);
        g.setColour (kAccent);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, inputLevelShown)), 2.5f);
    }
    if (! knobPanel.isEmpty())
    {
        g.setColour (kText);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        for (auto* k : { &guidanceKnob, &stepsKnob, &variationKnob })
            g.drawText (k == &guidanceKnob ? "GUIDANCE" : k == &stepsKnob ? "STEPS" : "VARIATION",
                        k->getBounds().withY (knobPanel.getY() + 6).withHeight (16), juce::Justification::centred);
    }

    // Job card with progress bar
    if (! jobCard.isEmpty())
    {
        g.setColour (kField.withAlpha (0.8f));
        g.fillRoundedRectangle (jobCard.toFloat(), 5.0f);
        g.setColour (currentJob != 0 ? kAccent.withAlpha (0.45f) : kEdge);
        g.drawRoundedRectangle (jobCard.toFloat().reduced (0.5f), 5.0f, 1.0f);
        g.setColour (kBright);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (currentJob != 0 ? "Job #" + juce::String (currentJob) : juce::String ("No job yet"),
                    jobCard.reduced (8, 7).removeFromTop (22).withTrimmedRight (70), juce::Justification::centredLeft);
    }
    if (! progressArea.isEmpty())
    {
        auto pa = progressArea.toFloat();
        g.setColour (juce::Colour (0xff1c2131));
        g.fillRoundedRectangle (pa, pa.getHeight() * 0.5f);
        g.setColour (kAccent);
        g.fillRoundedRectangle (pa.withWidth (pa.getWidth() * (float) juce::jlimit (0.0, 1.0, jobProgress)),
                                pa.getHeight() * 0.5f);
    }

    // Preview card
    if (! previewCard.isEmpty())
    {
        auto pc = previewCard.toFloat();
        g.setColour (kField.withAlpha (0.8f));
        g.fillRoundedRectangle (pc, 5.0f);
        g.setColour (kEdge);
        g.drawRoundedRectangle (pc.reduced (0.5f), 5.0f, 1.0f);
        // No stat in paint: "a take is selected" is the audition selection being set.
        const auto sel = proc.audition.getSelected();
#if defined (HSLINK_TEST_HOOKS)
        const bool editorStats = TakeGrid::testEditorStats.load();
#else
        const bool editorStats = false;
#endif
        const bool haveSel = sel != juce::File()
                             && (! editorStats || EditorFs::exists (sel, EditorFs::tagPaint));
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.setColour (kDim);
        auto line = previewCard.reduced (8, 6).removeFromTop (16);
        g.drawText ("AUDITION", line, juce::Justification::centredLeft);
        g.setColour (haveSel ? kAccent : kDim);
        g.setFont (juce::FontOptions (11.5f));
        g.drawText (haveSel ? sel.getFileNameWithoutExtension() : juce::String ("pass-through (no take selected)"),
                    previewCard.reduced (8, 6).withTrimmedTop (18).removeFromTop (18),
                    juce::Justification::centredLeft, true);
        const auto wa = previewWaveArea();
        if (! wa.isEmpty())
        {
            g.setColour (juce::Colour (0xff0d1018));
            g.fillRect (wa);
            const int nb = (int) peakMax.size();
            if (haveSel && peakPoll.file == sel && nb > 0)
            {
                const float mid = (float) wa.getCentreY(), half = (float) wa.getHeight() * 0.5f;
                g.setColour (kAccent.withAlpha (0.85f));
                for (int x = 0; x < wa.getWidth(); ++x)
                {
                    const int b0 = x * nb / wa.getWidth();
                    const int b1 = juce::jmax (b0 + 1, (x + 1) * nb / wa.getWidth());
                    float lo = 0.0f, hi = 0.0f;
                    for (int b = b0; b < b1 && b < nb; ++b)
                    {
                        lo = juce::jmin (lo, peakMin[(size_t) b]);
                        hi = juce::jmax (hi, peakMax[(size_t) b]);
                    }
                    const float y0 = mid - juce::jlimit (-1.0f, 1.0f, hi) * half;
                    const float y1 = mid - juce::jlimit (-1.0f, 1.0f, lo) * half;
                    g.drawVerticalLine (wa.getX() + x, y0, juce::jmax (y0 + 1.0f, y1));
                }
                if (lastPlayhead >= 0.0)
                {
                    g.setColour (juce::Colours::white);
                    g.drawVerticalLine (wa.getX() + (int) (lastPlayhead * wa.getWidth()),
                                        (float) wa.getY(), (float) wa.getBottom());
                }
            }
            else
            {
                g.setColour (kDim);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (haveSel ? juce::String ("loading waveform...")
                                               : juce::String ("Select a take to see its waveform"),
                            wa, juce::Justification::centred, true);
            }
        }
    }

    // Status bar
    g.setColour (juce::Colour (0xff090c14));
    g.fillRect (statusArea);
    g.fillRect (collectionArea);
    g.setColour (kEdge);
    g.drawHorizontalLine (collectionArea.getY(), 0.0f, (float) getWidth());
    g.setColour (kEdge);
    g.drawHorizontalLine (statusArea.getY(), 0.0f, (float) getWidth());
}

juce::Rectangle<int> HotStepLinkEditor::previewWaveArea() const
{
    if (previewCard.getHeight() < 60)
        return {};
    return previewCard.reduced (8, 6).withTrimmedTop (38);
}

void HotStepLinkEditor::refreshPreview()
{
    const auto sel = proc.audition.getSelected();
    bool dirty = false;
    // Read the generation before fetching: a publish racing the fetch bumps it
    // again, so the next poll refetches. No file stat or lock while unchanged.
    const auto gen = proc.audition.peaksGeneration.load (std::memory_order_acquire);
    if (peakPoll.needsFetch (sel, gen))
    {
        peakMin.clear();
        peakMax.clear();
        if (sel != juce::File())
            proc.audition.getPeaks (sel, peakMin, peakMax);
        dirty = true;
    }
    const double ph = AuditionMath::unpackPlayhead (proc.audition.playhead.load (std::memory_order_relaxed));
    if (std::abs (ph - lastPlayhead) > 1.0e-4)
    {
        lastPlayhead = ph;
        dirty = true;
    }
    if (dirty)
        repaint (previewCard);
}

juce::Rectangle<int> HotStepLinkEditor::previewHeaderArea() const
{
    // No card at small heights: then there is no room for the header either.
    if (previewCard.isEmpty())
        return {};
    return genPlate.reduced (12, 0).withY (previewCard.getY() - 22).withHeight (20);
}

void HotStepLinkEditor::resized()
{
    auto r = getLocalBounds();
    headerArea = r.removeFromTop (52);
    statusArea = r.removeFromBottom (24);
    collectionArea = r.removeFromBottom (64); // Mockup A: Collection lives in the bottom chip bar only
    collectionPanel.setBounds (collectionArea.reduced (12, 5));

    {
        auto h = headerArea.reduced (14, 6);
        titleLabel.setBounds (h.removeFromLeft (220)); // painted wordmark + beat grid area
        {
            const auto tb = titleLabel.getBounds();
            const int gx = tb.getX() + 112;
            headerBars.setVisible (tb.getRight() - gx > 60);
            headerBars.setBounds (gx - 2, headerArea.getY(), tb.getRight() - gx + 4, headerArea.getHeight());
        }
        settingsButton.setBounds (h.removeFromRight (84).withSizeKeepingCentre (84, 26));
        h.removeFromRight (10);
        // Presets group needs 320 px; the server pill gives way first on narrow windows.
        const int pillW = juce::jlimit (160, 340, h.getWidth() - 330);
        auto pill = h.removeFromRight (pillW).withSizeKeepingCentre (pillW, 26);
        serverLabel.setBounds (pill.withTrimmedLeft (24));
        h.removeFromLeft (10);
        // Width must be read once: h.removeFromLeft shrinks h before the second argument is evaluated.
        const int psW = juce::jmin (320, h.getWidth());
        auto ps = h.removeFromLeft (psW).withSizeKeepingCentre (psW, 26);
        soonPresets = ps;
        copyAbButton.setBounds (ps.removeFromRight (40));
        ps.removeFromRight (4);
        slotBButton.setBounds (ps.removeFromRight (26));
        ps.removeFromRight (2);
        slotAButton.setBounds (ps.removeFromRight (26));
        ps.removeFromRight (8);
        presetDeleteButton.setBounds (ps.removeFromRight (36));
        ps.removeFromRight (2);
        presetSaveButton.setBounds (ps.removeFromRight (54));
        ps.removeFromRight (4);
        presetBox.setBounds (ps);
    }
    {
        auto s = statusArea.reduced (12, 0);
        versionLabel.setBounds (s.removeFromRight (280));
        exposureLabel.setBounds (s.removeFromRight (190));
        syncLabel.setBounds (s);
    }

    const int sideW = juce::jlimit (240, 320, r.getWidth() / 4);
    setupPlate = r.removeFromLeft (sideW);
    queuePlate = r.removeFromRight (sideW);
    genPlate = r;
    takeEditor.setBounds (genPlate); // set whether it is shown or not

    const int pad = 12, gap = 6;
    auto field = [] (juce::Rectangle<int>& area, juce::Label& cap, juce::Component& c, int h)
    {
        cap.setBounds (area.removeFromTop (16));
        c.setBounds (area.removeFromTop (h));
        area.removeFromTop (8);
    };

    // Left plate: setup
    {
        auto a = setupPlate.reduced (pad).withTrimmedTop (22);
        // SOURCE panel (Mockup A): waveform card, then ARM (plugin) / Drop WAV (standalone).
        // The waveform gives way on short windows so the rows under it stay whole;
        // kBelowSource is everything removed from a below this line.
        const int kBelowSource = 6 + 28 + 20 + 24 + 46 + 6 + 24 + 96 + 6 + 24 + 50 + 8 + 28;
        sourcePanel = a.removeFromTop (juce::jlimit (60, 104, a.getHeight() - kBelowSource));
        sourceWave.setBounds (sourcePanel.reduced (6));
        a.removeFromTop (6);
        {
            auto br = a.removeFromTop (28);
            if (standalone)
            {
                // Drop WAV (Source) | Add as take | CAPTURE
                const int w3 = (br.getWidth() - 2 * gap) / 3;
                dropButton.setBounds (br.removeFromLeft (w3));
                br.removeFromLeft (gap);
                addTakeButton.setBounds (br.removeFromLeft (w3));
                br.removeFromLeft (gap);
                captureNowButton.setBounds (br);
            }
            else if (addTakeButton.isVisible())
            {
                // ARM | Add as take | CAPTURE
                const int w3 = (br.getWidth() - 2 * gap) / 3;
                captureButton.setBounds (br.removeFromLeft (w3));
                br.removeFromLeft (gap);
                addTakeButton.setBounds (br.removeFromLeft (w3));
                br.removeFromLeft (gap);
                captureNowButton.setBounds (br);
            }
            else
            {
                captureButton.setBounds (br.removeFromLeft ((br.getWidth() - gap) / 2));
                br.removeFromLeft (gap);
                captureNowButton.setBounds (br);
            }
        }
        sourceLabel.setBounds (a.removeFromTop (20));
        a.removeFromTop (24); // painted HOST SYNC header
        syncPanel = a.removeFromTop (46);
        a.removeFromTop (6);
        sourceSlot = {};
        a.removeFromTop (24); // painted UNDERSTAND header
        understandPanel = a.removeFromTop (96);
        a.removeFromTop (6 + 24); // painted INPUT header
        inputPanel = a.removeFromTop (50);
        a.removeFromTop (8);
        reanalyzeButton.setBounds (a.removeFromTop (28));
    }

    // SETTINGS popover: the SETUP form, anchored under the SETTINGS button.
    {
        const int pw = 300;
        int ph = 12 + 24 + 5 * (16 + 26 + 8) + 16 + 34 + 34 + (standalone ? 30 : 0);
        auto pb = juce::Rectangle<int> (settingsButton.getRight() - pw, headerArea.getBottom() + 4, pw, ph);
        settingsPopover.setBounds (pb.constrainedWithin (getLocalBounds()));
        auto a = settingsPopover.getLocalBounds().reduced (12, 10);
        settingsPopover.closeButton.setBounds (a.removeFromTop (24).removeFromRight (64));
        a.removeFromTop (8);
        field (a, urlCaption, urlEditor, 26);
        field (a, keyCaption, keyEditor, 26);
        {
            // Show / Hide: right end of the key field row, one gap clear of the field. Button
            // height is the panel's (Close, 24 px), centred on the 26 px field row.
            auto row = keyEditor.getBounds();
            int revealH = kKeyRevealH;
#if defined (HSLINK_TEST_HOOKS)
            if (testKeyRevealMutant == 7) // teeth 7: the field row's height, as it was
                revealH = 26;
#endif
            keyRevealButton.setBounds (row.removeFromRight (kKeyRevealW).withSizeKeepingCentre (kKeyRevealW, revealH));
            row.removeFromRight (gap);
            keyEditor.setBounds (row);
        }
        {
            // Refused-save note: right part of the "DAW key" caption row, kKeyNoteGap clear of the caption text.
            const auto cap = keyCaption.getBounds();
            juce::GlyphArrangement ga;
            ga.addLineOfText (keyCaption.getFont(), keyCaption.getText(), 0.0f, 0.0f);
            const int capText = keyCaption.getBorderSize().getLeft() + (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth());
            const auto nb = cap.withTrimmedLeft (juce::jmin (cap.getWidth(), capText + kKeyNoteGap));
            keySaveNote.setBounds (nb);
            keySaveNote.setText (keySaveNoteTextFor ((float) (nb.getWidth() - keySaveNote.getBorderSize().getRight())), juce::dontSendNotification);
        }
        {
            // Mode row: mode box left half, strength box right half (no extra height).
            auto cap = a.removeFromTop (16);
            modeCaption.setBounds (cap);
            refLockNote.setBounds (cap.withTrimmedLeft (96));
            auto row = a.removeFromTop (26);
            modeBox.setBounds (row.removeFromLeft ((row.getWidth() - gap) / 2));
            row.removeFromLeft (gap);
            strengthEditor.setBounds (row);
            a.removeFromTop (8);
        }
        field (a, backendCaption, backendBox, 26);
        field (a, aceModelCaption, aceModelBox, 26);
        regionCaption.setBounds (a.removeFromTop (16));
        if (standalone)
        {
            auto reg = a.removeFromTop (26);
            regionStartEditor.setBounds (reg.removeFromLeft ((reg.getWidth() - gap) / 2));
            reg.removeFromLeft (gap);
            regionEndEditor.setBounds (reg);
            a.removeFromTop (4);
        }
        regionInfo.setBounds (a.removeFromTop (34));
    }

    // Middle plate: generate
    {
        auto a = genPlate.reduced (pad).withTrimmedTop (22);
        roleCards.setBounds (a.removeFromTop (RoleCardRow::kHeight));
        a.removeFromTop (6);
        field (a, captionCaption, captionEditor, 34);
        {
            auto lc = a.removeFromTop (16);
            instrumentalButton.setBounds (lc.removeFromRight (110));
            lyricsCaption.setBounds (lc);
            lyricsEditor.setBounds (a.removeFromTop (44));
            a.removeFromTop (8);
        }

        auto row = a.removeFromTop (16 + 26);
        const int cols = standalone ? 3 : 2;
        const int cw = (row.getWidth() - gap * (cols - 1)) / cols;
        auto cell = [&] (juce::Label* cap, juce::Component& c)
        {
            auto col = row.removeFromLeft (cw);
            row.removeFromLeft (gap);
            if (cap != nullptr)
                cap->setBounds (col.removeFromTop (16));
            else
                col.removeFromTop (16);
            c.setBounds (col);
        };
        if (standalone)
            cell (&bpmCaption, manualBpm);
        cell (&durationCaption, durationSlider);
        cell (&takesCaption, takesSlider);
        a.removeFromTop (8);
        // Knob row (Mockup A): GUIDANCE, STEPS, VARIATION; labels painted above each knob
        knobPanel = a.removeFromTop (98);
        {
            for (auto* l : { &variationCaption, &guidanceCaption, &stepsCaption })
                l->setVisible (false);
            auto k = knobPanel.reduced (8, 4).withTrimmedTop (20);
            const int kw = k.getWidth() / 3;
            guidanceKnob.setBounds (k.removeFromLeft (kw));
            stepsKnob.setBounds (k.removeFromLeft (kw));
            variationKnob.setBounds (k);
        }
        a.removeFromTop (8);

        // Seed + key row
        {
            auto row2 = a.removeFromTop (16 + 26);
            const int half = (row2.getWidth() - gap) / 2;
            auto sc = row2.removeFromLeft (half);
            row2.removeFromLeft (gap);
            seedCaption.setBounds (sc.removeFromTop (16));
            diceButton.setBounds (sc.removeFromRight (52));
            sc.removeFromRight (4);
            seedRandomButton.setBounds (sc.removeFromRight (84));
            sc.removeFromRight (4);
            seedEditor.setBounds (sc);
            auto pk = row2.removeFromRight ((row2.getWidth() - gap) / 2);
            row2.removeFromRight (gap);
            peakModeCaption.setBounds (pk.removeFromTop (16));
            peakModeBox.setBounds (pk);
            songKeyCaption.setBounds (row2.removeFromTop (16));
            songKeyBox.setBounds (row2);
        }
        a.removeFromTop (10);

        generateButton.setBounds (a.removeFromTop (44));
        // Offline banner: over the GENERATE controls (knobs, seed/key, Generate) only, so the
        // take list, Edit take, audition, export and drag-out stay usable with no server.
#if defined (HSLINK_TEST_HOOKS)
        const bool overlayFullWindow = offlineMutantForTest == mutOverlayFullWindow;
#else
        const bool overlayFullWindow = false;
#endif
        if (overlayFullWindow)
            overlay.setBounds (getLocalBounds().withTrimmedTop (headerArea.getHeight()));
        else
            overlay.setBounds (juce::Rectangle<int>::leftTopRightBottom (genPlate.getX() + 4, knobPanel.getY(),
                                                                         genPlate.getRight() - 4, generateButton.getBottom()));
        a.removeFromTop (8);
        if (saveNotice.isVisible())
        {
            auto n = a.removeFromBottom (42);
            a.removeFromBottom (4);
            if (retrySaveButton.isVisible())
            {
                retrySaveButton.setBounds (n.removeFromRight (84).withSizeKeepingCentre (84, 24));
                n.removeFromRight (6);
            }
            saveNotice.setBounds (n);
        }
        if (undoDeleteButton.isVisible())
        {
            auto n = a.removeFromBottom (24);
            a.removeFromBottom (4);
            undoDeleteButton.setBounds (n.removeFromRight (64));
            n.removeFromRight (6);
            deleteNotice.setBounds (n);
        }
        // PREVIEW (audition + export) under GENERATE, freed by moving TAKES right.
        if (exportPathLabel.isVisible())
        {
            auto row = a.removeFromBottom (22);
            showExportButton.setBounds (row.removeFromRight (104));
            row.removeFromRight (gap);
            exportPathLabel.setBounds (row);
            a.removeFromBottom (4);
        }
        auto exp = a.removeFromBottom (26);
        exportWavButton.setBounds (exp.removeFromLeft ((exp.getWidth() - gap) / 2));
        exp.removeFromLeft (gap);
        midiDrag.setBounds (exp.removeFromRight (40));
        exp.removeFromRight (gap);
        // The Q combo needs about 80 px for "Q 1/16" plus its arrow; size it first, then the
        // strength slider takes what is left so Export MIDI keeps at least half the row.
#if defined (HSLINK_TEST_HOOKS)
        const bool oldQuantLayout = testOldQuantLayout;
#else
        const bool oldQuantLayout = false;
#endif
        if (oldQuantLayout)
        {
            quantStrengthSlider.setBounds (exp.removeFromRight (juce::jmin (60, exp.getWidth() / 4)));
            exp.removeFromRight (gap);
            quantizeBox.setBounds (exp.removeFromRight (juce::jmin (72, exp.getWidth() / 3)));
        }
        else
        {
            const int qW = juce::jmin (82, exp.getWidth() * 2 / 5);
            quantStrengthSlider.setBounds (exp.removeFromRight (juce::jmin (56, (exp.getWidth() - qW) / 3)));
            exp.removeFromRight (gap);
            quantizeBox.setBounds (exp.removeFromRight (qW));
        }
        exp.removeFromRight (gap);
        // Split button: the picker arrow is the right 22 px of Export MIDI, flush.
        exportMidiMenuButton.setBounds (exp.removeFromRight (22));
        exportMidiButton.setBounds (exp);
        exportMidiButton.setConnectedEdges (juce::Button::ConnectedOnRight);
        exportMidiMenuButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
        // MIDI picker card: above the split button, right edge on the arrow, kept on screen.
        midiPicker.setBounds (juce::Rectangle<int> (exportMidiMenuButton.getRight() - MidiExportPopover::kW,
                                                    exportMidiButton.getY() - 4 - MidiExportPopover::kH,
                                                    MidiExportPopover::kW, MidiExportPopover::kH)
                                  .constrainedWithin (getLocalBounds()));
        a.removeFromBottom (6);
        // All or nothing: no squeezed transport row while the window is too short for it.
        auto tr = a.removeFromBottom (a.getHeight() >= 26 ? 26 : 0);
        playButton.setBounds (tr.removeFromLeft (70));
        tr.removeFromLeft (gap);
        gainLabel.setBounds (tr.removeFromRight (130));
        tr.removeFromRight (gap);
        if (! standalone)
        {
            followHostButton.setBounds (tr.removeFromLeft (tr.getWidth() / 2));
            tr.removeFromLeft (gap);
        }
        levelMatchButton.setBounds (tr);
        a.removeFromBottom (6);
        a.removeFromTop (22); // painted PREVIEW header
        previewCard = a;
    }

    // Right plate: queue + preview + export
    {
        // TAKES list (rows), then QUEUE: job card, queue cards, batch actions.
        auto a = queuePlate.reduced (pad).withTrimmedTop (22);
        editTakeButton.setBounds (a.getRight() - 96, a.getY() - 23, 96, 22);
        {
            auto row = a.removeFromBottom (28);
            const int half = (row.getWidth() - gap) / 2;
            moreLikeButton.setBounds (row.removeFromLeft (half));
            // Re-fit the label to the new width (the take id is dropped when it would clip).
            moreLikeButton.setButtonText (moreLikeTextFor (moreLikeLabel, (float) moreLikeButton.getWidth() - 8.0f));
            row.removeFromLeft (gap);
            clearUnstarredButton.setBounds (row);
        }
        a.removeFromBottom (8);
        const int qh = juce::jlimit (0, 3 * QueueListComponent::kRowH + 4, juce::jmax (0, a.getHeight() / 3 - 70));
        queueList.setBounds (a.removeFromBottom (qh));
        a.removeFromBottom (qh > 0 ? 6 : 0);
        jobCard = a.removeFromBottom (64);
        {
            auto j = jobCard.reduced (8, 7);
            auto top = j.removeFromTop (22);
            cancelButton.setBounds (top.removeFromRight (64));
            j.removeFromTop (2);
            progressArea = j.removeFromTop (6);
            jobLabel.setBounds (jobCard.getX() + 6, jobCard.getY() + 34, jobCard.getWidth() - 12, jobCard.getHeight() - 36);
        }
        a.removeFromBottom (8 + 22); // painted QUEUE header above the job card
        takeList.setBounds (a);
    }
    repaint();
}

//==============================================================================
juce::String QueueListComponent::stateLabel (const DawJob& j)
{
    return DawJob::label (j); // one source, shared with the job card header
}

void QueueListComponent::setCards (const std::vector<Card>& c, int selectedId)
{
    if (c == cards && selectedId == selected)
        return;
    cards = c;
    selected = selectedId;
    repaint();
}

juce::Rectangle<int> QueueListComponent::rowBounds (int i) const
{
    // When the list overflows, the newest cards stay visible.
    const int maxRows = juce::jmax (1, getHeight() / kRowH);
    const int first = juce::jmax (0, (int) cards.size() - maxRows);
    if (i < first)
        return {};
    return { 0, (i - first) * kRowH, getWidth(), kRowH - 2 };
}

void QueueListComponent::paint (juce::Graphics& g)
{
    if (cards.empty())
    {
        g.setColour (kDim);
        g.setFont (juce::FontOptions (kEmptyHintFontH));
        g.drawFittedText (kEmptyHint, getLocalBounds().reduced (kEmptyHintInsetX, 0),
                          juce::Justification::centredLeft, kEmptyHintLines, 1.0f);
        return;
    }
    for (int i = 0; i < (int) cards.size(); ++i)
    {
        auto r = rowBounds (i);
        if (r.isEmpty())
            continue;
        const auto& c = cards[(size_t) i];
        const bool sel = c.id == selected;
        g.setColour (sel ? kAccent.withAlpha (0.10f) : kField.withAlpha (0.8f));
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        g.setColour (sel ? kAccent.withAlpha (0.6f) : kEdge);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 4.0f, 1.0f);
        auto in = r.reduced (6, 0);
        if (c.removable)
        {
            g.setColour (kDim);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText ("x", in.removeFromRight (14), juce::Justification::centred);
        }
        const juce::Colour stc = c.state == "running" ? kAccent
                               : c.state == "failed" ? juce::Colour (0xffef4444)
                               : c.state == "done" ? juce::Colour (0xff34d399) : kDim;
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.setColour (kDim);
        g.drawText (juce::String (c.takes) + "/" + juce::String (c.takeCount), in.removeFromRight (30),
                    juce::Justification::centredRight);
        g.setColour (stc);
        g.drawText (c.state, in.removeFromRight (60), juce::Justification::centredRight);
        g.setColour (kText);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("#" + juce::String (c.id) + " " + c.caption.replaceCharacters ("\r\n", "  "), in,
                    juce::Justification::centredLeft, true);
    }
}

juce::Rectangle<int> RoleCardRow::cardBounds (int i) const
{
    const int gap = 6;
    const int w = (getWidth() - gap * (RoleCards::kCount - 1)) / RoleCards::kCount;
    return { i * (w + gap), 0, w, getHeight() };
}

int RoleCardRow::cardAt (juce::Point<int> p) const
{
    for (int i = 0; i < RoleCards::kCount; ++i)
        if (cardBounds (i).contains (p))
            return i;
    return -1;
}

void RoleCardRow::paint (juce::Graphics& g)
{
    for (int i = 0; i < RoleCards::kCount; ++i)
    {
        const bool on = i == active;
        const float dimA = (dimmed & (1u << i)) != 0 ? 0.4f : 1.0f; // backend not listed by the server
        auto r = cardBounds (i).toFloat().reduced (0.5f);
        g.setColour (on ? kAccent.withAlpha (0.12f) : (i == hover ? kField.brighter (0.08f) : kField));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour ((on ? kAccent.withAlpha (0.75f) : kEdge).withMultipliedAlpha (dimA));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
        auto in = cardBounds (i).reduced (9, 4);
        g.setColour ((on ? kAccent : kBright).withMultipliedAlpha (dimA));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (RoleCards::title (i), in.removeFromTop (in.getHeight() / 2), juce::Justification::centredLeft, true);
        g.setColour ((on ? kText : kDim).withMultipliedAlpha (dimA));
        g.setFont (juce::FontOptions (10.0f));
        g.drawText (RoleCards::subtitle (i), in, juce::Justification::centredLeft, true);
    }
}

void RoleCardRow::mouseMove (const juce::MouseEvent& e)
{
    const int h = cardAt (e.getPosition());
    if (h != hover)
    {
        hover = h;
        repaint();
    }
}

void RoleCardRow::mouseExit (const juce::MouseEvent&)
{
    if (hover != -1)
    {
        hover = -1;
        repaint();
    }
}

void RoleCardRow::mouseUp (const juce::MouseEvent& e)
{
    const int c = cardAt (e.getPosition());
    if (c >= 0 && onPick != nullptr)
        onPick (c);
}

void QueueListComponent::mouseDown (const juce::MouseEvent& e)
{
    for (int i = 0; i < (int) cards.size(); ++i)
    {
        const auto r = rowBounds (i);
        if (! r.contains (e.getPosition()))
            continue;
        const auto& c = cards[(size_t) i];
        if (c.removable && e.x >= r.getRight() - 20)
        {
            if (onRemove) onRemove (c.id);
        }
        else if (c.finished && onSelect)
            onSelect (c.id);
        return;
    }
}

//==============================================================================
juce::uint32 TakeCell::thumbNowMs()
{
#if defined (HSLINK_TEST_HOOKS)
    if (testThumbClock)
        return testThumbClock();
#endif
    return juce::Time::getMillisecondCounter();
}

bool TakeCell::repointThumbIfGone (const juce::File& src)
{
    if (src == juce::File())
        return false;
    // A cell whose thumbnail has a length is done: no file call for it here.
    const bool empty = thumb.getTotalLength() <= 0.0;
    if (src == thumbFile)
    {
        // Same path, nothing read from it. The cell was made while the take was listed
        // and its file was still <name>.part (registerTake lists before the rename), so
        // the first read found no file. Read it again once it is there. A file that is
        // there and gives no length (empty, not audio) uses up the retries; a file that
        // is not there yet does not.
        // The retries are spaced kThumbRetryGapMs apart (a counter compare, no file
        // call), so a file held by another program right after the rename is not
        // given up on within three refreshes. A capped or waiting cell makes no call.
        int cap = kThumbRetries;
#if defined (HSLINK_TEST_HOOKS)
        if (testThumbRetryCap > 0)
            cap = testThumbRetryCap; // teeth: the cap before
#endif
        if (! empty || thumbRetries >= cap)
            return false;
        const auto nowMs = thumbNowMs();
        bool waiting = thumbRetries > 0 && (juce::uint32) (nowMs - thumbRetryAtMs) < (juce::uint32) kThumbRetryGapMs;
#if defined (HSLINK_TEST_HOOKS)
        waiting = waiting && ! testThumbNoSpacing; // teeth: the code before
        if (! waiting)
            ++testThumbExistsCalls;
#endif
        if (waiting || ! thumbFile.existsAsFile())
            return false;
        ++thumbRetries;
        thumbRetryAtMs = nowMs;
        EditorFs::count (EditorFs::tagCell);
        EditorFs::countThumbSource (thumbFile);
        thumb.setSource (new juce::FileInputSource (thumbFile));
        repaint();
        return thumb.getTotalLength() > 0.0;
    }
    if (thumbFile.existsAsFile() || ! src.existsAsFile())
        return false;
    thumbFile = src;
    thumbRetries = 0;
    thumbRetryAtMs = 0;
    thumb.setSource (new juce::FileInputSource (thumbFile));
    repaint();
    return true;
}

TakeCell::TakeCell (const juce::String& k, const juce::String& lab, const juce::File& thumbSource, int idx,
                    juce::AudioFormatManager& fm, juce::AudioThumbnailCache& cache)
    : key (k), label (lab), thumbFile (thumbSource), index (idx), thumb (256, fm, cache)
{
    thumb.addChangeListener (this);
    // setSource opens the file on this thread (JUCE reads the header here, the peaks on
    // the cache's thread). Once per cell, when the cell is made; counted as tagCell.
    // The file is the take's temp copy (TakeGrid::thumbSourceOf), never the lasting one.
    // No file given = no source and a blank waveform.
    if (thumbFile != juce::File())
    {
        EditorFs::count (EditorFs::tagCell);
        EditorFs::countThumbSource (thumbFile);
        thumb.setSource (new juce::FileInputSource (thumbFile));
    }
    starButton.setClickingTogglesState (true);
    starButton.setTooltip ("Star this take");
    starButton.onClick = [this] { if (onStar) onStar (key, starButton.getToggleState()); };
    addAndMakeVisible (starButton);
    addButton.setTooltip ("Keep this take in the Collection");
    addButton.onClick = [this] { if (onAdd) onAdd (key); };
    addAndMakeVisible (addButton); // a child button: its click never reaches the cell (no select, no drag)
    deleteButton.setTooltip ("Delete this take (moves it to a Deleted folder, with a short undo)");
    deleteButton.onClick = [this] { if (onDelete) onDelete (deleteKey); }; // the take's temp path, not its name
    addAndMakeVisible (deleteButton);
    slotAButton.setTooltip ("Load this take's settings into slot A");
    slotAButton.onClick = [this] { if (onLoadSlot) onLoadSlot (key, 0); };
    addAndMakeVisible (slotAButton);
    slotBButton.setTooltip ("Load this take's settings into slot B");
    slotBButton.onClick = [this] { if (onLoadSlot) onLoadSlot (key, 1); };
    addAndMakeVisible (slotBButton);
}

void TakeCell::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().reduced (3).toFloat();
    g.setColour (selected ? kAccent.withAlpha (0.07f) : kField.withAlpha (0.8f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (selected ? kAccent.withAlpha (0.65f) : kEdge);
    g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
    auto ri = getLocalBounds().reduced (9, 8);
    auto head = ri.removeFromTop (18);
    g.setColour (kBright);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("T" + juce::String (index + 1), head.removeFromLeft (28), juce::Justification::centredLeft);
    g.setColour (kDim);
    g.setFont (juce::FontOptions (10.0f));
    auto nameArea = head.withTrimmedRight (128); // A, B, delete, add and star buttons
    if (unsaved)
    {
        auto badge = nameArea.removeFromRight (58).reduced (0, 2);
        g.setColour (juce::Colour (0xffffb347));
        g.drawRoundedRectangle (badge.toFloat().reduced (0.5f), 3.0f, 1.0f);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText ("NOT SAVED", badge, juce::Justification::centred);
        g.setColour (kDim);
        g.setFont (juce::FontOptions (10.0f));
        nameArea.removeFromRight (4);
    }
    g.drawText (label, nameArea, juce::Justification::centredLeft, true);
    ri.removeFromTop (4);
    if (missing)
    {
        g.setColour (juce::Colour (0xffff6b6b));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawFittedText ("FILE MISSING", ri, juce::Justification::centred, 1);
        return;
    }
    if (dragRefused)
    {
        g.setColour (juce::Colour (0xffffb347));
        g.drawFittedText (dragMissing ? "File is missing or empty: cannot be dragged to the DAW."
                                      : "Not saved yet: cannot be dragged to the DAW. Use Export WAV.",
                          ri, juce::Justification::centred, 2);
        return;
    }
    // Mockup A: a FIT row under the waveform (label, bar, score) when a fit score exists.
    juce::Rectangle<int> fitRow;
    if (fitTag.text.isNotEmpty())
    {
        fitRow = ri.removeFromBottom (12);
        ri.removeFromBottom (3);
    }
#if defined (HSLINK_TEST_HOOKS)
    const int lengthMutant = TakeGrid::testLengthMutant.load();
#else
    const int lengthMutant = 0;
#endif
    if (hasChipRow() && lengthMutant != 3)
        ri.removeFromBottom (kChipRowH + kChipGap); // the bpm / length chips get their own row under the waveform
    g.setColour (selected ? kAccent : kLav.withAlpha (0.85f));
    if (thumb.getTotalLength() > 0.0)
        thumb.drawChannels (g, ri, 0.0, thumb.getTotalLength(), 1.0f);
    else
        g.drawText ("loading...", ri, juce::Justification::centred);
    if (tempoShown().isNotEmpty())
    {
        // Left end of the chip row under the waveform, on a dark plate.
        const auto plate = tempoPlate();
        g.setColour (juce::Colours::black.withAlpha (0.62f));
        g.fillRoundedRectangle (plate.toFloat(), 3.0f);
        g.setColour (tempoTag.colour == TakeGrid::TempoTag::green ? juce::Colour (0xff6fd08c)
                     : tempoTag.colour == TakeGrid::TempoTag::amber ? juce::Colour (0xffffb347)
                     : tempoTag.colour == TakeGrid::TempoTag::red ? juce::Colour (0xffff6b6b) : kDim);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText (tempoShown(), plate.reduced (4, 0), juce::Justification::centredLeft, true);
    }
    if (const auto lp = lengthPlate(); ! lp.isEmpty())
    {
        // Right end of the chip row: the take length, always shown; the tempo tag gives way (tempoShown).
        g.setColour (juce::Colours::black.withAlpha (0.62f));
        g.fillRoundedRectangle (lp.toFloat(), 3.0f);
        g.setColour (kText);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText (lengthText, lp.reduced (4, 0), juce::Justification::centredRight, false);
    }
    if (! fitRow.isEmpty())
    {
        const int score = juce::jlimit (0, 100, fitScore);
        const auto col = fitTag.colour == TakeGrid::TempoTag::green ? juce::Colour (0xff6fd08c)
                       : fitTag.colour == TakeGrid::TempoTag::amber ? juce::Colour (0xffffb347)
                       : fitTag.colour == TakeGrid::TempoTag::red ? juce::Colour (0xffff6b6b) : kDim;
        auto fr = fitRow;
        g.setColour (kDim);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText ("FIT", fr.removeFromLeft (22), juce::Justification::centredLeft);
        g.setColour (col);
        g.drawText (fitTag.text, fr.removeFromRight (juce::jmin (100, fr.getWidth() / 2)), juce::Justification::centredRight, true);
        fr.removeFromRight (4);
        auto bar = fr.withSizeKeepingCentre (fr.getWidth(), 4).toFloat();
        g.setColour (juce::Colour (0xff1c2131));
        g.fillRoundedRectangle (bar, 2.0f);
        g.setColour (col);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) score / 100.0f), 2.0f);
    }
}

juce::Rectangle<int> TakeCell::waveArea() const
{
    auto ri = getLocalBounds().reduced (9, 8);
    ri.removeFromTop (22); // head row 18 + gap 4
    if (fitTag.text.isNotEmpty())
        ri.removeFromBottom (15); // FIT row 12 + gap 3
#if defined (HSLINK_TEST_HOOKS)
    const int lengthMutant = TakeGrid::testLengthMutant.load();
#else
    const int lengthMutant = 0;
#endif
    if (hasChipRow() && lengthMutant != 3) // mutant 3: chips back over the waveform
        ri.removeFromBottom (kChipRowH + kChipGap); // chip row, never over the waveform
    return ri;
}

bool TakeCell::hasChipRow() const
{
    return (tempoTag.text.isNotEmpty() || lengthText.isNotEmpty()) && ! missing && ! dragRefused;
}

juce::Rectangle<int> TakeCell::chipRow() const
{
    if (! hasChipRow())
        return {};
    const auto wa = waveArea();
#if defined (HSLINK_TEST_HOOKS)
    if (TakeGrid::testLengthMutant.load() == 3)
        return { wa.getX(), wa.getBottom() - kChipRowH, wa.getWidth(), kChipRowH }; // the old corner plates
#endif
    return { wa.getX(), wa.getBottom() + kChipGap, wa.getWidth(), kChipRowH };
}

#if defined (HSLINK_TEST_HOOKS)
int TakeCell::chipGapForTest() const
{
    // Smallest vertical gap from the waveform bottom to any chip (negative = overlap); 1000 = no chips.
    int gap = 1000;
    const auto wa = waveArea();
    for (const auto& c : { tempoPlate(), lengthPlate() })
        if (! c.isEmpty())
            gap = juce::jmin (gap, c.getY() - wa.getBottom());
    return gap;
}
#endif

juce::Rectangle<int> TakeCell::tempoPlate() const
{
    const auto t = tempoShown();
    if (t.isEmpty())
        return {};
    const auto ri = chipRow();
    const int w = juce::jmin (ri.getWidth(), 8 + (int) std::ceil (lengthTextWidth (t)));
    return { ri.getX(), ri.getY(), w, ri.getHeight() };
}

float TakeCell::lengthTextWidth (const juce::String& t)
{
    juce::AttributedString as;
    as.append (t, juce::FontOptions (9.0f, juce::Font::bold));
    juce::TextLayout tl;
    tl.createLayout (as, 100000.0f);
    return tl.getWidth();
}

juce::String TakeCell::tempoShown() const
{
    const auto& full = tempoTag.text;
#if defined (HSLINK_TEST_HOOKS)
    const int lengthMutant = TakeGrid::testLengthMutant.load();
#else
    const int lengthMutant = 0;
#endif
    if (full.isEmpty() || lengthMutant == 2)
        return full;
    const auto ri = waveArea();
    const auto lp = lengthPlate();
    // Room left of the length plate (4 px gap); the whole width with no length.
    const int room = lp.isEmpty() ? ri.getWidth() : lp.getX() - 4 - ri.getX();
    const auto fits = [room] (const juce::String& t) { return 8.0f + lengthTextWidth (t) <= (float) room; };
    if (fits (full))
        return full;
    // Shorter forms: the number and the est mark, then the number alone.
    const auto first = full.upToFirstOccurrenceOf (" ", false, false);
    const bool est = juce::StringArray::fromTokens (full, " ", {}).contains ("est");
    if (est && fits (first + " est"))
        return first + " est";
    if (fits (first))
        return first;
    return {}; // not even the number fits: the tempo gives way to the length
}

juce::Rectangle<int> TakeCell::lengthPlate() const
{
    if (lengthText.isEmpty() || missing || dragRefused)
        return {};
    const auto ri = chipRow();
    const int w = 8 + (int) std::ceil (lengthTextWidth (lengthText));
    if (w > ri.getWidth())
        return {}; // narrower than the length text itself
    return { ri.getRight() - w, ri.getY(), w, ri.getHeight() };
}

#if defined (HSLINK_TEST_HOOKS)
bool TakeCell::lengthFitsForTest (juce::Rectangle<int>* plateOut, juce::Rectangle<int>* tempoOut) const
{
    const auto lp = lengthPlate();
    if (plateOut != nullptr) *plateOut = lp;
    if (tempoOut != nullptr) *tempoOut = tempoPlate();
    return ! lp.isEmpty() && chipRow().contains (lp) && lengthTextWidth (lengthText) <= (float) (lp.getWidth() - 8);
}
#endif

void TakeCell::resized()
{
    starButton.setBounds (getWidth() - 30, 7, 22, 20);
    addButton.setBounds (getWidth() - 54, 7, 22, 20);
    deleteButton.setBounds (getWidth() - 78, 7, 22, 20);
    slotBButton.setBounds (getWidth() - 104, 7, 22, 20);
    slotAButton.setBounds (getWidth() - 128, 7, 22, 20);
}

void TakeCell::setUnsaved (bool u, const juce::String& why, bool pending)
{
    setTooltip (u ? why : juce::String());
    if (u != unsaved || pending != notCopiedYet)
    {
        unsaved = u;
        notCopiedYet = pending;
        repaint();
    }
}

void TakeCell::mouseDrag (const juce::MouseEvent& e)
{
    // Decided once per gesture (dragStarted / dragRefused hold until the mouse is released).
    if (dragStarted || dragRefused || e.getDistanceFromDragStart() < 6)
        return;
    // From the take's entry at this moment: the lasting copy, or {} while there
    // is none. The temp file is deleted later, so its path is never handed to the host.
    const auto lasting = dragFileFor ? dragFileFor (key) : juce::File();
    if (lasting == juce::File())
    {
        dragRefused = true; // the reason is painted in the cell until the mouse is released
        dragMissing = false;
        repaint();
        return;
    }
    // One file check per drag gesture, as CollectionRow::checkDragFile: a host is never
    // handed a lasting path that is gone or a file of 0 bytes.
    if (! dragFileUsable (lasting))
    {
        dragRefused = true;
        dragMissing = true;
        repaint();
        return;
    }
    dragStarted = true;
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ lasting.getFullPathName() }, false, this,
                                                               dragDoneFor (this));
}

bool TakeCell::dragFileUsable (const juce::File& f)
{
    return EditorFs::exists (f, EditorFs::tagDrag) && f.getSize() > 0;
}

std::function<void()> TakeCell::dragDoneFor (TakeCell* cell)
{
    // The host calls this after the drag, from a posted message: the cell may be gone by
    // then (take list trimmed, queue-card filter changed, editor closed).
    juce::Component::SafePointer<TakeCell> safe (cell);
    return [safe] { if (safe != nullptr) safe->dragStarted = false; };
}

void TakeCell::mouseUp (const juce::MouseEvent& e)
{
    dragStarted = false;
    if (dragRefused)
    {
        dragRefused = false;
        dragMissing = false;
        repaint();
    }
    if (e.mouseWasClicked() && onSelect)
        onSelect (key);
}

TakeListComponent::TakeListComponent()
{
    formats.registerBasicFormats();
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
}

const TakeCell* TakeListComponent::cellForKey (const juce::String& key) const
{
    for (auto* c : cells)
        if (c->key == key)
            return c;
    return nullptr;
}

void TakeListComponent::setTakes (const std::vector<TakeGrid::Cell>& shownCells, const juce::String& unsavedWhy)
{
    juce::StringArray keys;
    for (const auto& m : shownCells)
        keys.add (m.key);
    if (keys != current)
    {
        // A cell whose key is still listed is kept (same component, same waveform);
        // only takes that are new to the grid get a new cell.
        juce::OwnedArray<TakeCell> old;
        old.swapWith (cells);
        for (int i = 0; i < (int) shownCells.size(); ++i)
        {
            const auto& m = shownCells[(size_t) i];
            TakeCell* c = nullptr;
            for (int k = 0; k < old.size(); ++k)
                if (old[k]->key == m.key)
                {
                    c = old.removeAndReturn (k);
                    break;
                }
            if (c == nullptr)
            {
                // The waveform reads the temp copy. A kept cell is never given another
                // file, so a save does not re-point it at the lasting copy.
                c = new TakeCell (m.key, m.label, TakeGrid::thumbSourceOf (m), m.number > 0 ? m.number - 1 : i, formats, cache);
                c->onSelect = [this] (const juce::String& k) { if (onSelect) onSelect (k); };
                c->onStar = [this] (const juce::String& k, bool on) { if (onStar) onStar (k, on); };
                c->onAdd = [this] (const juce::String& k) { if (onAdd) onAdd (k); };
                c->onDelete = [this] (const juce::String& k) { if (onDelete) onDelete (k); };
                c->onLoadSlot = [this] (const juce::String& k, int slot) { if (onLoadSlot) onLoadSlot (k, slot); };
                c->dragFileFor = [this] (const juce::String& k) { return dragFileFor ? dragFileFor (k) : juce::File(); };
                content.addAndMakeVisible (c);
            }
            c->setIndex (m.number > 0 ? m.number - 1 : i); // D2: T<stable number>
            cells.add (c);
        }
        current = keys;
        layoutCells(); // old goes out of scope: cells of takes that left the grid
    }
    for (int i = 0; i < cells.size() && i < (int) shownCells.size(); ++i)
    {
        const auto& m = shownCells[(size_t) i];
#if defined (HSLINK_TEST_HOOKS)
        const bool noThumbFallback = TakeGrid::testNoThumbFallback.load();
#else
        const bool noThumbFallback = false;
#endif
        if (! noThumbFallback)
            cells[i]->repointThumbIfGone (TakeGrid::thumbSourceOf (m));
        cells[i]->deleteKey = m.temp.getFullPathName(); // set on every refresh: a kept cell follows its place in the list
        cells[i]->setSelected (m.selected);
        cells[i]->setStarred (m.starred);
        cells[i]->setUnsaved (m.badge, unsavedWhy, m.pending);
        cells[i]->setMissing (m.missing);
        if (m.missing)
            cells[i]->setTooltip ("File missing: " + m.file.getFullPathName());
        cells[i]->setTempoTag (TakeGrid::tempoTagOf (m.tempoState, m.tempo, m.resolved));
        cells[i]->setFitTag (TakeGrid::fitTagOf (m.fitLen, m.fitTempo), m.fitScore);
        cells[i]->setLengthText (TakeGrid::lengthTextOf (m.durationS));
    }
}

void TakeListComponent::paint (juce::Graphics& g)
{
    if (cells.isEmpty())
    {
        g.setColour (kDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Takes appear here when a job finishes", getLocalBounds(), juce::Justification::centred);
    }
}

void TakeListComponent::resized()
{
    viewport.setBounds (getLocalBounds());
    layoutCells();
}

void TakeListComponent::layoutCells()
{
    const int w = juce::jmax (1, viewport.getWidth() - viewport.getScrollBarThickness());
    const int cols = columnsFor (w);
    const int cw = w / cols;
    const int rows = (cells.size() + cols - 1) / cols;
    content.setSize (w, juce::jmax (1, rows * kCellH));
    for (int i = 0; i < cells.size(); ++i)
        cells[i]->setBounds ((i % cols) * cw, (i / cols) * kCellH, cw, kCellH);
    repaint();
}

//==============================================================================
CollectionRow::CollectionRow (const CollectionItem& it) : item (it)
{
    setTooltip (it.sidecarMissing ? juce::String ("No info file next to this WAV: the details were read from the audio file")
                                  : it.caption);
}

CollectionRow::Zone CollectionRow::zoneAt (juce::Point<int> p) const
{
    if (p.x >= getWidth() - 24)
        return zoneRemove;
    if (p.x >= getWidth() - 46)
        return zoneStar;
    return zoneBody;
}

void CollectionRow::paint (juce::Graphics& g)
{
    const auto box = getLocalBounds().withTrimmedBottom (2);
    g.setColour (selected ? kAccent.withAlpha (0.10f) : kField.withAlpha (0.8f));
    g.fillRoundedRectangle (box.toFloat(), 4.0f);
    g.setColour (selected ? kAccent.withAlpha (0.6f) : kEdge);
    g.drawRoundedRectangle (box.toFloat().reduced (0.5f), 4.0f, 1.0f);

    auto in = box.reduced (7, 3);
    g.setColour (kDim);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("x", in.removeFromRight (16), juce::Justification::centred);
    {
        const auto sr = in.removeFromRight (22).toFloat();
        juce::Path star;
        star.addStar (sr.getCentre(), 5, 3.0f, 6.5f);
        if (item.starred)
        {
            g.setColour (kAmberA);
            g.fillPath (star);
        }
        else
        {
            g.setColour (kDim.withAlpha (0.8f));
            g.strokePath (star, juce::PathStrokeType (1.0f));
        }
    }
    in.removeFromRight (2);
    auto top = in.removeFromTop (in.getHeight() / 2 + 1);
    const auto badge = CollectionView::badge (item.backend);
    if (badge.isNotEmpty())
    {
        const auto c = badge == "SA3" ? kAmberA : badge == "YuE2" ? kLav : badge == "ACE" ? kOk : kDim;
        const auto br = top.removeFromRight (36).withSizeKeepingCentre (36, 13).toFloat();
        g.setColour (c.withAlpha (0.14f));
        g.fillRoundedRectangle (br, 3.0f);
        g.setColour (c);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText (badge, br, juce::Justification::centred);
        top.removeFromRight (4);
    }
    g.setColour (kBright);
    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    g.drawText (item.name, top, juce::Justification::centredLeft, true); // ellipsis when too long
    if (item.sidecarMissing)
    {
        g.setColour (kDim.withAlpha (0.65f));
        g.setFont (juce::FontOptions (9.0f, juce::Font::italic));
        g.drawText ("no info file", in.removeFromRight (56), juce::Justification::centredRight);
    }
    g.setColour (kDim);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText (CollectionView::subtitle (item), in, juce::Justification::centredLeft, true);
}

void CollectionRow::mouseDown (const juce::MouseEvent& e)
{
    pressed = zoneAt (e.getPosition());
    dragStarted = false;
    dragChecked = false;
}

// One file check per drag gesture (mouseDown clears it), so a host is never handed a
// Collection path that is gone and a folder that does not answer is asked once, not on
// every mouse move.
bool CollectionRow::checkDragFile()
{
    if (! dragChecked)
    {
        dragChecked = true;
        dragFileOk = EditorFs::exists (item.wav, EditorFs::tagDrag);
    }
    return dragFileOk;
}

void CollectionRow::mouseDrag (const juce::MouseEvent& e)
{
    // Only the row body drags; the star and the x never start a drag.
    if (pressed != zoneBody || dragStarted || e.getDistanceFromDragStart() < 6 || ! checkDragFile())
        return;
    dragStarted = true;
    juce::Component::SafePointer<CollectionRow> safe (this); // a rescan may rebuild the rows mid-drag
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ item.wav.getFullPathName() }, false, this,
                                                               [safe] { if (safe != nullptr) safe->dragStarted = false; });
}

void CollectionRow::mouseUp (const juce::MouseEvent& e)
{
    dragStarted = false;
    if (! e.mouseWasClicked() || zoneAt (e.getPosition()) != pressed)
        return;
    // The callbacks may rebuild the list and delete this row: copy first, touch nothing after.
    const auto it = item;
    if (pressed == zoneStar)
    {
        if (auto f = onStar) f (it, ! it.starred);
    }
    else if (pressed == zoneRemove)
    {
        if (auto f = onRemove) f (it);
    }
    else if (auto f = onSelect)
        f (it);
}

//==============================================================================
CollectionPanel::CollectionPanel()
{
    search.setTextToShowWhenEmpty ("search snippets, tags, captions...", kText.withAlpha (0.4f));
    search.setFont (juce::FontOptions (11.5f));
    search.onTextChange = [this]
    {
        chips.search = search.getText();
        rebuild();
    };
    addAndMakeVisible (search);
    auto backendChip = [this] (juce::TextButton& b, int id, const char* tip)
    {
        b.setTooltip (tip);
        b.onClick = [this, id]
        {
            chips.backend = id;
            syncChipButtons();
            rebuild();
        };
        addAndMakeVisible (b);
    };
    backendChip (chipAll, CollectionChips::all, "Show every backend");
    backendChip (chipSa3, CollectionChips::sa3, "Only takes made with SA3");
    backendChip (chipYue2, CollectionChips::yue2, "Only takes made with YuE2");
    backendChip (chipAce, CollectionChips::ace, "Only takes made with ACE");
    backendChip (chipMiniMax, CollectionChips::minimax, "Only takes made with MiniMax-M3");
    backendChip (chipHeart, CollectionChips::heartmula, "Only takes made with HeartMuLa");
    chipStar.setTooltip ("Only starred items");
    chipStar.onClick = [this]
    {
        chips.starredOnly = ! chips.starredOnly;
        syncChipButtons();
        rebuild();
    };
    addAndMakeVisible (chipStar);
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (false, true);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);
    syncChipButtons();
}

void CollectionPanel::syncChipButtons()
{
    chipAll.setToggleState (chips.backend == CollectionChips::all, juce::dontSendNotification);
    chipSa3.setToggleState (chips.backend == CollectionChips::sa3, juce::dontSendNotification);
    chipYue2.setToggleState (chips.backend == CollectionChips::yue2, juce::dontSendNotification);
    chipAce.setToggleState (chips.backend == CollectionChips::ace, juce::dontSendNotification);
    chipMiniMax.setToggleState (chips.backend == CollectionChips::minimax, juce::dontSendNotification);
    chipHeart.setToggleState (chips.backend == CollectionChips::heartmula, juce::dontSendNotification);
    chipStar.setToggleState (chips.starredOnly, juce::dontSendNotification);
}

void CollectionPanel::setChips (const CollectionChips& c)
{
    chips = c;
    search.setText (c.search, juce::dontSendNotification);
    syncChipButtons();
    rebuild();
}

void CollectionPanel::setItems (const std::vector<CollectionItem>& items)
{
    all = items;
    loaded = true;
    rebuild();
}

void CollectionPanel::setSelectedFile (const juce::File& f)
{
    if (f == selectedFile)
        return;
    selectedFile = f;
    for (auto* r : rows)
        r->setSelected (selectedFile != juce::File() && r->item.wav == selectedFile);
}

void CollectionPanel::showNote (const juce::String& text, bool isError, int holdMs)
{
    note = text;
    noteError = isError;
    noteUntilMs = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (500, holdMs);
    repaint (footerArea);
}

void CollectionPanel::tick()
{
    if (note.isNotEmpty() && (juce::int32) (juce::Time::getMillisecondCounter() - noteUntilMs) >= 0)
    {
        note.clear();
        repaint (footerArea);
    }
}

void CollectionPanel::rebuild()
{
    const auto shown = CollectionStore::filter (all, CollectionView::queryFor (chips));
    rows.clear();
    for (const auto& it : shown)
    {
        auto* r = rows.add (new CollectionRow (it));
        r->setSelected (selectedFile != juce::File() && it.wav == selectedFile);
        r->onSelect = [this] (const CollectionItem& i) { if (auto f = onSelect) f (i); };
        r->onRemove = [this] (const CollectionItem& i) { if (auto f = onRemove) f (i); };
        r->onStar = [this] (const CollectionItem& i, bool on) { if (auto f = onStar) f (i, on); };
        content.addAndMakeVisible (r);
    }
    layoutRows();
    repaint();
}

void CollectionPanel::layoutRows()
{
    // Bottom chip bar: one chip per saved take, left to right, scrolling sideways.
    const int need = rows.size() * (kChipW + kChipGap);
    const int h = juce::jmax (1, viewport.getHeight() - (need > viewport.getWidth() ? viewport.getScrollBarThickness() : 0));
    content.setSize (juce::jmax (1, need), h);
    for (int i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (i * (kChipW + kChipGap), 0, kChipW, juce::jmin (h, CollectionRow::kRowH));
}

void CollectionPanel::resized()
{
    // Bar layout: top line = title, count and footer note; second line = search, filter
    // chips, then the sideways list of saved takes.
    auto a = getLocalBounds();
    auto top = a.removeFromTop (14);
    headerArea = top.removeFromLeft (150);
    footerArea = top;
    a.removeFromTop (2);
    {
        auto c = a.withHeight (22).withY (a.getY() + 4);
        search.setBounds (c.removeFromLeft (150));
        c.removeFromLeft (6);
        const int gap = 3;
        int used = 150 + 6;
        const std::pair<juce::TextButton*, int> chipW[] = { { &chipAll, 44 }, { &chipSa3, 44 }, { &chipYue2, 44 }, { &chipAce, 44 },
                                                            { &chipMiniMax, 58 }, { &chipHeart, 70 } };
        for (const auto& [b, w] : chipW)
        {
            b->setBounds (c.removeFromLeft (w));
            c.removeFromLeft (gap);
            used += w + gap;
        }
        chipStar.setBounds (c.removeFromLeft (70));
        a.removeFromLeft (used + 70 + 10);
    }
    listArea = a;
    viewport.setBounds (listArea);
    layoutRows();
}

void CollectionPanel::paint (juce::Graphics& g)
{
    // Header in the plate caption style, with the count in the accent colour.
    {
        auto h = headerArea;
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.setColour (kText.withAlpha (0.85f));
        const juce::String title ("COLLECTION");
        juce::GlyphArrangement ga;
        ga.addLineOfText (g.getCurrentFont(), title, 0.0f, 0.0f);
        const int tw = (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth());
        g.drawText (title, h.removeFromLeft (tw + 2), juce::Justification::centredLeft);
        if (loaded)
        {
            g.setColour (kAccent);
            g.drawText (CollectionView::countText (rows.size(), (int) all.size(), CollectionView::isFiltering (chips)),
                        h.withTrimmedLeft (6), juce::Justification::centredLeft);
        }
    }
    if (rows.isEmpty())
    {
        g.setColour (kDim);
        g.setFont (juce::FontOptions (11.0f));
        const char* t = ! loaded ? "Loading..."
                      : all.empty() ? "No saved takes yet. Use + on a take to keep it."
                                    : "No matches";
        g.drawFittedText (t, listArea.reduced (6, 4), juce::Justification::centred, 3);
    }
    g.setFont (juce::FontOptions (10.0f));
    if (note.isNotEmpty())
    {
        g.setColour (noteError ? kBad : kOk);
        g.drawText (note, footerArea, juce::Justification::centredLeft, true);
    }
    else
    {
        g.setColour (kDim.withAlpha (0.8f));
        g.drawText ("Drag a row into the DAW. Click to audition.", footerArea, juce::Justification::centredLeft, true);
    }
}

// ---------------------------------------------------------------------------
// SourceWaveView

SourceWaveView::~SourceWaveView()
{
    if (pending != nullptr)
        pending->cancel = true;
    pool.removeAllJobs (true, 2000);
}

void SourceWaveView::setSource (const juce::File& f, double lenS, double bpmIn)
{
    if (f == file && lenS == lengthS)
    {
        if (bpmIn != bpm) { bpm = bpmIn; repaint(); }
        return;
    }
    file = f;
    lengthS = lenS;
    bpm = bpmIn;
    peaks.reset();
    if (pending != nullptr)
        pending->cancel = true;
    pending.reset();
    if (f != juce::File() && lenS > 0.0)
    {
        auto sh = std::make_shared<Shared>();
        pending = sh;
        pool.addJob ([sh, f]
        {
            auto d = std::make_shared<TakePeaks::Data> (TakePeaks::build (f, [sh] { return sh->cancel.load(); }));
            std::lock_guard<std::mutex> l (sh->lock);
            sh->result = std::move (d);
        });
    }
    repaint();
}

bool SourceWaveView::poll()
{
    if (pending == nullptr)
        return false;
    std::shared_ptr<TakePeaks::Data> d;
    {
        std::lock_guard<std::mutex> l (pending->lock);
        d = pending->result;
    }
    if (d == nullptr)
        return false;
    peaks = std::move (d);
    pending.reset();
    repaint();
    return true;
}

SourceWaveView::HeaderStyle SourceWaveView::headerStyleFor (CaptureState s)
{
    switch (s)
    {
        case CaptureState::recording: return { juce::Colour (0xffef4444), true };
        case CaptureState::captured:  return { kText, false };
        case CaptureState::idle:      break;
    }
    return { kDim, false };
}

juce::StringArray SourceWaveView::refHintLines (bool standaloneApp, bool compact)
{
    if (compact)
        return { standaloneApp ? "Reference: Drop WAV..., then repaint or lego in SETTINGS"
                               : "Reference: ARM + play, then repaint or lego in SETTINGS" };
    return { standaloneApp ? "To reference a track: Drop WAV..."
                           : "To reference a track: ARM, then play the host.",
             "In SETTINGS pick ACE-Step and repaint or lego." };
}

#if defined (HSLINK_TEST_HOOKS)
std::atomic<bool> SourceWaveView::testHintAlwaysFits { false };
std::atomic<bool> SourceWaveView::testHintIgnoresSource { false };
std::atomic<bool> SourceWaveView::testLabelAlwaysCaptured { false };
#endif

juce::String SourceWaveView::headerLabelFor (bool isRecording, bool hasSource)
{
    if (isRecording)
        return "RECORDING";
    return hasSource ? "CAPTURED" : "";
}

juce::String SourceWaveView::headerLabel() const
{
    bool hasSource = file != juce::File(); // the rule the CAPTURE button readout uses
#if defined (HSLINK_TEST_HOOKS)
    if (testLabelAlwaysCaptured.load())
        hasSource = true; // teeth: the old code, CAPTURED whatever the source
#endif
    return headerLabelFor (recording, hasSource);
}

bool SourceWaveView::refHintFits (const juce::StringArray& lines, juce::Rectangle<int> area)
{
#if defined (HSLINK_TEST_HOOKS)
    if (testHintAlwaysFits.load())
        return ! lines.isEmpty();
#endif
    if (lines.isEmpty() || lines.size() * kRefHintLineH > area.getHeight())
        return false;
    for (const auto& l : lines)
    {
        juce::AttributedString as;
        as.append (l, juce::FontOptions (kRefHintFontH));
        juce::TextLayout tl;
        tl.createLayout (as, 100000.0f);
        if (tl.getWidth() > (float) area.getWidth())
            return false;
    }
    return true;
}

juce::StringArray SourceWaveView::refHintFor (bool standaloneApp, juce::Rectangle<int> area)
{
    for (const bool compact : { false, true })
    {
        const auto lines = refHintLines (standaloneApp, compact);
        if (refHintFits (lines, area))
            return lines;
    }
    return {};
}

void SourceWaveView::setRefHint (bool on, bool standaloneApp)
{
    if (on != refHintOn || standaloneApp != refHintStandalone)
    {
        refHintOn = on;
        refHintStandalone = standaloneApp;
        repaint();
    }
}

juce::Rectangle<int> SourceWaveView::waveArea() const
{
    auto r = getLocalBounds();
    r.removeFromTop (16);
    r.removeFromBottom (14);
    return r.reduced (2, 2);
}

bool SourceWaveView::refHintShowing() const
{
    bool noSource = file == juce::File();
#if defined (HSLINK_TEST_HOOKS)
    if (testHintIgnoresSource.load())
        noSource = true; // teeth: the hint ignores the source
#endif
    return refHintOn && ! recording && noSource && ! refHintFor (refHintStandalone, waveArea()).isEmpty();
}

void SourceWaveView::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isLeftButtonDown() && refHintShowing() && waveArea().contains (e.getPosition()) && onRefHintDismiss)
        onRefHintDismiss();
}

void SourceWaveView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    auto head = r.removeFromTop (16);
    auto ruler = r.removeFromBottom (14);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    const auto st = headerStyleFor (recording ? CaptureState::recording
                                    : (peaks != nullptr && peaks->ok) ? CaptureState::captured : CaptureState::idle);
    auto label = head.reduced (4, 0);
    if (st.dot)
    {
        g.setColour (st.text);
        g.fillEllipse (label.removeFromLeft (8).toFloat().withSizeKeepingCentre (6.0f, 6.0f));
        label.removeFromLeft (3);
    }
    g.setColour (st.text);
    g.drawText (headerLabel(), label, juce::Justification::centredLeft); // empty with no source: nothing drawn

    const double bars = bpm > 0.0 ? lengthS * bpm / 240.0 : 0.0; // 4/4 bars
    if (bars > 0.0)
    {
        g.setColour (kAccent);
        g.drawText (juce::String (bars, bars == std::floor (bars) ? 0 : 1) + " BARS", head.reduced (4, 0), juce::Justification::centredRight);
    }

    const auto wave = r.reduced (2, 2);
    const float mid = (float) wave.getCentreY(), half = (float) wave.getHeight() * 0.5f;
    if (peaks != nullptr && peaks->ok && peaks->length > 0 && wave.getWidth() > 0)
    {
        const double fpp = (double) peaks->length / (double) wave.getWidth();
        g.setColour (kAccent.withAlpha (0.9f));
        for (int x = 0; x < wave.getWidth(); ++x)
        {
            float lo = 0.0f, hi = 0.0f, lo2 = 0.0f, hi2 = 0.0f;
            TakePeaks::columnMinMax (*peaks, 0, (juce::int64) (x * fpp), fpp, lo, hi);
            if (peaks->lanes > 1 && TakePeaks::columnMinMax (*peaks, 1, (juce::int64) (x * fpp), fpp, lo2, hi2))
            {
                lo = juce::jmin (lo, lo2);
                hi = juce::jmax (hi, hi2);
            }
            const float top = mid - juce::jlimit (0.0f, 1.0f, hi) * half;
            const float bot = mid - juce::jlimit (-1.0f, 0.0f, lo) * half;
            g.drawVerticalLine (wave.getX() + x, top, juce::jmax (top + 1.0f, bot));
        }
    }
    else if (refHintShowing())
    {
        // First-run hint in place of the centre line and "no capture yet" (same area, no overlap).
        const auto lines = refHintFor (refHintStandalone, wave);
        auto box = wave.withSizeKeepingCentre (wave.getWidth(), lines.size() * kRefHintLineH);
        g.setColour (kText);
        g.setFont (juce::FontOptions (kRefHintFontH));
        for (const auto& l : lines)
            g.drawText (l, box.removeFromTop (kRefHintLineH), juce::Justification::centred, false);
    }
    else
    {
        g.setColour (kEdge);
        g.drawHorizontalLine ((int) mid, (float) wave.getX(), (float) wave.getRight());
        g.setColour (kDim);
        g.setFont (juce::FontOptions (10.5f));
        g.drawText (file == juce::File() ? "no capture yet" : (pending != nullptr ? "reading..." : "unreadable"),
                    wave, juce::Justification::centred);
    }

    // Bar ruler: a tick per bar, numbered from 1 (every other bar when crowded)
    g.setColour (kEdge);
    g.drawHorizontalLine (ruler.getY(), (float) ruler.getX(), (float) ruler.getRight());
    if (bars > 0.0)
    {
        const int n = (int) std::ceil (bars - 1.0e-6);
        const float pxPerBar = (float) ruler.getWidth() / (float) bars;
        const int every = pxPerBar < 18.0f ? (int) std::ceil (18.0f / pxPerBar) : 1;
        g.setFont (juce::FontOptions (9.5f));
        for (int b = 0; b <= n; ++b)
        {
            const float x = (float) ruler.getX() + pxPerBar * (float) b;
            if (x > (float) ruler.getRight() + 0.5f)
                break;
            g.setColour (kEdge);
            g.drawVerticalLine ((int) x, (float) wave.getY(), (float) ruler.getY() + 3.0f);
            if (b < n && b % every == 0)
            {
                g.setColour (kDim);
                g.drawText (juce::String (b + 1), juce::Rectangle<float> (x + 2.0f, (float) ruler.getY() + 1.0f, 24.0f, 12.0f),
                            juce::Justification::centredLeft);
            }
        }
    }
}

// Dev shot hooks (Standalone only; the plugin wrappers never parse a command line):
//   --shot-load "<wav>"        drop that WAV in as Drop WAV does and list it as take 0
//   --shot-open-editor         open Edit take on take --shot-take (default 0) once listed
//   --shot-zoom "<s>,<e>"      zoom the take editor to s..e seconds (implies open)
//   --shot-warp "<t>,<m>,<d>"  open take t, drag warp marker m to d seconds through the
//                              mouse-drag path (Shift = free, so no snap; add --shot-snap to snap)
void HotStepLinkEditor::parseShotArgs (const juce::String& cmd)
{
    const auto args = juce::StringArray::fromTokens (cmd, true);
    auto valueOf = [&args] (const char* flag) {
        const int i = args.indexOf (flag);
        return i >= 0 && i + 1 < args.size() ? args[i + 1].unquoted() : juce::String();
    };
    shot.snap = args.contains ("--shot-snap");
    if (const auto w = valueOf ("--shot-load"); w.isNotEmpty())
    {
        useSourceFile (juce::File (w));
        // listed as take 0, so --shot-take 0 works on a fresh launch. A hot file is a pending
        // guarded import: wait for it here (dev shot hook only) so take 0 exists right away.
        const auto added = proc.addShotTakeChecked (juce::File (w));
        if (added == HotStepLinkProcessor::AddTake::pending)
            proc.waitTakeStretchIdle (30000);
        if (added == HotStepLinkProcessor::AddTake::added || added == HotStepLinkProcessor::AddTake::pending)
            refreshTakes();
    }
    if (const auto t = valueOf ("--shot-take"); t.isNotEmpty())
        shot.take = t.getIntValue();
    if (const auto z = valueOf ("--shot-zoom"); z.isNotEmpty())
    {
        const auto p = juce::StringArray::fromTokens (z, ",", "");
        if (p.size() == 2)
        {
            shot.zoomStart = p[0].getDoubleValue();
            shot.zoomEnd = p[1].getDoubleValue();
        }
    }
    if (const auto w = valueOf ("--shot-warp"); w.isNotEmpty())
    {
        const auto p = juce::StringArray::fromTokens (w, ",", "");
        if (p.size() == 3)
        {
            shot.take = p[0].getIntValue();
            shot.marker = p[1].getIntValue();
            shot.dstSec = p[2].getDoubleValue();
        }
    }
    if (args.contains ("--shot-open-editor") || shot.zoomEnd > shot.zoomStart || shot.marker >= 0)
        shot.step = 1;
}

void HotStepLinkEditor::runShotStep()
{
    // Periodic ticks (about 6 a second): 1 = wait for the take, open; 2 = zoom; 3 = warp.
    if (shot.step == 1)
    {
        const auto cells = takeModel.shown();
        if (shot.take < 0 || (std::size_t) shot.take >= cells.size())
            return;
        // Hold the open while the take is still being measured (cap about 60 s of ticks),
        // so the editor opens with its tempo, grid and warp markers like a normal take.
        if (cells[(std::size_t) shot.take].tempoState == HslinkTakeEntry::tempoMeasuring && ++shot.waitTicks < 360)
            return;
        selectTake (cells[(std::size_t) shot.take].key);
        if (openTakeEditor())
            shot.step = 2;
        return;
    }
    if (shot.step == 2)
    {
        if (shot.zoomEnd > shot.zoomStart)
            takeEditor.shotZoom (shot.zoomStart, shot.zoomEnd);
        shot.step = 3;
        return;
    }
    if (shot.marker >= 0)
        takeEditor.shotDragWarp (shot.marker, shot.dstSec, shot.snap);
    shot.step = 0;
}

void HotStepLinkEditor::commitServerUrl()
{
    if (urlEditor.isReadOnly())
        return;
    const bool ok = proc.setServerUrl (urlEditor.getText());
    if (ok)
    {
        urlEditor.removeColour (juce::TextEditor::outlineColourId);
        urlEditor.removeColour (juce::TextEditor::focusedOutlineColourId);
    }
    else
    {
        urlEditor.setColour (juce::TextEditor::outlineColourId, juce::Colours::red);
        urlEditor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::red);
    }
    urlEditor.setTooltip (ok ? juce::String() : juce::String ("Expected http://host:port"));
    if (ok)
        updateExposureWarning();
    urlEditor.repaint();
}

void HotStepLinkEditor::updateExposureWarning()
{
    // Never reads or logs the key value: only whether one is set. Never blocks connecting.
    const bool noKey = keyEditor.getText().trim().isEmpty();
    const bool exposed = HotStepLinkProcessor::shouldWarnExposure (proc.getServerUrl(), ! noKey);
    exposureLabel.setText (exposed ? "Remote server, no key set" : "", juce::dontSendNotification);
    exposureLabel.setVisible (exposed);
    // The red invalid-URL outline wins over amber.
    const bool redShown = urlEditor.isColourSpecified (juce::TextEditor::outlineColourId)
                          && urlEditor.findColour (juce::TextEditor::outlineColourId) == juce::Colours::red;
    if (redShown)
        return;
    const bool overridden = HotStepLinkProcessor::serverUrlOverride().isNotEmpty();
    if (exposed)
    {
        urlEditor.setColour (juce::TextEditor::outlineColourId, kAmberB);
        urlEditor.setColour (juce::TextEditor::focusedOutlineColourId, kAmberB);
        urlEditor.setTooltip ("Remote server with no Key: anyone on your network can drive this HOT-Step. "
                              "Set HOTSTEP_DAW_KEY on the server and enter it here.");
    }
    else
    {
        urlEditor.removeColour (juce::TextEditor::outlineColourId);
        urlEditor.removeColour (juce::TextEditor::focusedOutlineColourId);
        urlEditor.setTooltip (overridden ? juce::String ("Server URL set by HSLINK_SERVER_URL (launch override, not saved)")
                                         : juce::String());
    }
    urlEditor.repaint();
}
