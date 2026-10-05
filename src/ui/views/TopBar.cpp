// TopBar implementation.
#include "ui/views/TopBar.h"

#include "ui/Theme.h"

namespace hr
{
TopBar::TopBar()
{
    auto setUpTab = [this] (TabButton& tab, Tab which)
    {
        tab.onClick = [this, which] { setTab (which, true); };
        addAndMakeVisible (tab);
    };

    setUpTab (mixerTab, Tab::mixer);
    setUpTab (appsTab, Tab::apps);
    setUpTab (settingsTab, Tab::settings);
    mixerTab.setToggleState (true, juce::dontSendNotification);

    pill.onClick = [this] { if (onPillClicked) onPillClicked(); };
    addAndMakeVisible (pill);
}

void TopBar::setTab (Tab tab, bool notify)
{
    current = tab;
    mixerTab.setToggleState (tab == Tab::mixer, juce::dontSendNotification);
    appsTab.setToggleState (tab == Tab::apps, juce::dontSendNotification);
    settingsTab.setToggleState (tab == Tab::settings, juce::dontSendNotification);

    if (notify && onTabChanged)
        onTabChanged (tab);
}

void TopBar::setMicStatus (juce::Colour dot, const juce::String& text, const juce::String& tooltip)
{
    pill.set (dot, text);
    pill.setTooltip (tooltip);
    resized();
}

void TopBar::resized()
{
    const int wMixer = mixerTab.getIdealWidth(), wApps = appsTab.getIdealWidth(), wSettings = settingsTab.getIdealWidth();
    const int gap = 8;
    const int total = wMixer + wApps + wSettings + gap * 2;
    int x = (getWidth() - total) / 2;

    mixerTab.setBounds (x, 0, wMixer, getHeight());     x += wMixer + gap;
    appsTab.setBounds (x, 0, wApps, getHeight());       x += wApps + gap;
    settingsTab.setBounds (x, 0, wSettings, getHeight());

    const int pillWidth = pill.getIdealWidth();
    pill.setBounds (getWidth() - theme::kSidePadding - pillWidth, (getHeight() - 24) / 2, pillWidth, 24);
}

void TopBar::paint (juce::Graphics& g)
{
    g.setColour (theme::bg0);
    g.fillRect (getLocalBounds());

    // Two-bar meter glyph, the same shapes as the app icon.
    const float glyphBottom = 31.0f;
    g.setColour (theme::accent);
    g.fillRoundedRectangle (16.0f, glyphBottom - 8.0f, 3.0f, 8.0f, 1.0f);
    g.fillRoundedRectangle (21.0f, glyphBottom - 14.0f, 3.0f, 14.0f, 1.0f);

    g.setColour (theme::text0);
    g.setFont (theme::sans (15.0f, theme::Sans::SemiBold));
    g.drawText ("headroom", juce::Rectangle<int> (32, 0, 120, getHeight()), juce::Justification::centredLeft, false);

    g.setColour (theme::line);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
}
}
