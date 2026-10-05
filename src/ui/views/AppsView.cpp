// AppsView implementation.
#include "ui/views/AppsView.h"

#include "ui/Theme.h"
#include "ui/views/RuleGroups.h"

namespace hr
{
namespace
{
    constexpr int kMaxColumn = 720;
    constexpr int kRowHeight = 48;
    constexpr int kRuleRowHeight = 40;

    std::vector<Dropdown::Item> channelItems (AppController& controller)
    {
        std::vector<Dropdown::Item> items;

        for (const auto& info : controller.getState().getChannelInfos())
        {
            if (! info.isApp())
                continue;           // apps play into app channels; the mic and the input channels take sound from a device

            Dropdown::Item item;
            item.key = info.id;
            item.text = info.name;
            item.dot = theme::colourFromHex (info.colour);
            item.hasDot = true;
            items.push_back (std::move (item));
        }

        return items;
    }

    // First letter in a tile tinted with the channel colour.
    class Monogram final : public juce::Component
    {
    public:
        void set (const juce::String& name, juce::Colour c)
        {
            letter = name.trim().substring (0, 1).toUpperCase();
            colour = c;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat();
            g.setColour (theme::tint (theme::bg2, colour, 0.18f));
            g.fillRoundedRectangle (r, theme::kControlRadius);
            g.setColour (colour);
            g.setFont (theme::sans (14.0f, theme::Sans::SemiBold));
            g.drawText (letter, r, juce::Justification::centred, false);
        }

    private:
        juce::String letter;
        juce::Colour colour { theme::text1 };
    };
}

namespace
{
// One line per running app.
class AppRow final : public juce::Component
{
public:
    AppRow (AppController& c, const AppStreamInfo& stream, juce::Colour channelColour) : controller (c), nodeId (stream.nodeId)
    {
        name = stream.appName;
        media = stream.mediaName;
        monogram.set (stream.appName, channelColour);
        addAndMakeVisible (monogram);

        picker.setItems (channelItems (controller));
        picker.setSelectedKey (stream.channelId);
        picker.setPlaceholder ("Choose a channel");
        picker.setTooltip ("The channel this app plays through");
        picker.onChange = [this] (const juce::String& key) { controller.routeApp (nodeId, key); };
        addAndMakeVisible (picker);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        monogram.setBounds (r.removeFromLeft (32).withSizeKeepingCentre (32, 32));
        picker.setBounds (r.removeFromRight (172).withSizeKeepingCentre (172, 32));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::line);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        auto text = getLocalBounds().withTrimmedLeft (32 + 12).withTrimmedRight (172 + 12);
        g.setColour (theme::text0);
        g.setFont (theme::sans (13.0f, theme::Sans::Medium));
        g.drawText (name, text.removeFromTop (getHeight() / 2 + 1).withTrimmedTop (4), juce::Justification::bottomLeft, true);
        g.setColour (theme::text2);
        g.setFont (theme::sans (12.0f));
        g.drawText (media, text.withTrimmedBottom (4), juce::Justification::topLeft, true);
    }

private:
    AppController& controller;
    const uint32_t nodeId;
    juce::String name, media;
    Monogram monogram;
    Dropdown picker;
};

// One remembered app (or a group of apps that belong together) -> channel, with a remove button. Changing the channel
// or removing the row applies to every pattern behind it; the raw patterns are only in the tooltip.
class RuleRow final : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    RuleRow (AppController& c, const RuleGroup& group) : controller (c), name (group.name), patterns (group.patterns)
    {
        setTooltip ("Matches: " + describePatterns (patterns));

        picker.setItems (channelItems (controller));
        picker.setSelectedKey (group.channelId);
        picker.setTooltip ("The channel these apps play through");
        picker.onChange = [this] (const juce::String& key)
        {
            for (const auto& pattern : patterns)
                controller.getState().setRule (pattern, key);
        };
        addAndMakeVisible (picker);

        remove.setTooltip ("Forget this");
        remove.onClick = [this]
        {
            for (const auto& pattern : patterns)
                controller.removeRule (pattern);
        };
        addAndMakeVisible (remove);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        remove.setBounds (r.removeFromRight (28).withSizeKeepingCentre (28, 28));
        r.removeFromRight (8);
        picker.setBounds (r.removeFromRight (172).withSizeKeepingCentre (172, 28));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::line);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        g.setColour (theme::text0);
        g.setFont (theme::sans (13.0f, theme::Sans::Medium));
        g.drawText (name, getLocalBounds().withTrimmedRight (172 + 8 + 28 + 12), juce::Justification::centredLeft, true);
    }

private:
    AppController& controller;
    const juce::String name;
    const juce::StringArray patterns;
    Dropdown picker;
    IconButton remove { "x", 14.0f };
};

}   // namespace

class AppsView::Content final : public juce::Component
{
public:
    Content (AppController& c) : controller (c)
    {
        fallback.setPrefix ("");
        fallback.onChange = [this] (const juce::String& key) { controller.setFallbackChannel (key); };
        fallback.setTooltip ("Apps Headroom hasn't seen before play through this channel");
        addAndMakeVisible (fallback);
    }

    void rebuild()
    {
        rows.clear();
        ruleRows.clear();
        removeAllChildren();
        addAndMakeVisible (fallback);

        const auto infos = controller.getState().getChannelInfos();

        auto colourOf = [&infos] (const juce::String& channelId)
        {
            for (const auto& info : infos)
                if (info.id == channelId)
                    return theme::colourFromHex (info.colour);

            return theme::text2;
        };

        for (const auto& stream : controller.getBackend().getAppStreams())
        {
            auto row = std::make_unique<AppRow> (controller, stream, colourOf (stream.channelId));
            addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }

        for (const auto& group : groupRules (controller.getState().getRoutingRules()))
        {
            auto row = std::make_unique<RuleRow> (controller, group);
            addAndMakeVisible (*row);
            ruleRows.push_back (std::move (row));
        }

        fallback.setItems (channelItems (controller));
        fallback.setSelectedKey (controller.getState().getFallbackChannel());
        layoutFor (getWidth());
    }

    int layoutFor (int width)
    {
        const int column = juce::jmin (kMaxColumn, width - 48);
        left = (width - column) / 2;

        int y = 32;
        titleY = y;          y += 32;
        subtitleY = y;       y += 20 + 24;

        playingHeaderY = y;  y += 24;

        if (rows.empty())
            { emptyPlayingY = y; y += 56; }

        for (auto& row : rows)
        {
            row->setBounds (left, y, column, kRowHeight);
            y += kRowHeight;
        }

        y += 24;
        rememberedHeaderY = y;  y += 24;

        if (ruleRows.empty())
            { emptyRememberedY = y; y += 56; }

        for (auto& row : ruleRows)
        {
            row->setBounds (left, y, column, kRuleRowHeight);
            y += kRuleRowHeight;
        }

        y += 28;
        footerY = y;
        fallbackLabelWidth = (int) std::ceil (theme::textWidth (theme::sans (13.0f), "New apps go to")) + 12;
        fallback.setBounds (left + fallbackLabelWidth, y, 172, 32);
        y += 32 + 32;

        columnWidth = column;
        setSize (width, y);
        return y;
    }

    void resized() override    { layoutFor (getWidth()); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::bg0);

        auto label = [&] (const juce::String& text, int y)
        {
            g.setColour (theme::text2);
            g.setFont (theme::caps (11.0f));
            g.drawText (text.toUpperCase(), juce::Rectangle<int> (left, y, columnWidth, 20), juce::Justification::centredLeft, false);
        };

        g.setColour (theme::text0);
        g.setFont (theme::sans (20.0f, theme::Sans::SemiBold));
        g.drawText ("Apps", juce::Rectangle<int> (left, titleY, columnWidth, 30), juce::Justification::centredLeft, false);

        g.setColour (theme::text1);
        g.setFont (theme::sans (13.0f));
        g.drawText ("Pick which channel each app plays through. Headroom remembers it.",
                    juce::Rectangle<int> (left, subtitleY, columnWidth, 20), juce::Justification::centredLeft, true);

        label ("Playing now", playingHeaderY);
        g.setColour (theme::line);
        g.fillRect (left, playingHeaderY + 22, columnWidth, 1);

        if (rows.empty())
            drawEmpty (g, emptyPlayingY, "Nothing is playing sound right now. Start something and it shows up here.");

        label ("Remembered", rememberedHeaderY);
        g.setColour (theme::line);
        g.fillRect (left, rememberedHeaderY + 22, columnWidth, 1);

        if (ruleRows.empty())
            drawEmpty (g, emptyRememberedY, "Nothing yet. When you move an app to a channel, Headroom remembers it here.");

        g.setColour (theme::text1);
        g.setFont (theme::sans (13.0f));
        g.drawText ("New apps go to", juce::Rectangle<int> (left, footerY, fallbackLabelWidth, 32), juce::Justification::centredLeft, false);
    }

private:
    void drawEmpty (juce::Graphics& g, int y, const juce::String& text) const
    {
        g.setColour (theme::text2);
        g.setFont (theme::sans (13.0f));
        g.drawFittedText (text, juce::Rectangle<int> (left, y + 8, columnWidth, 40), juce::Justification::topLeft, 2);
    }

    AppController& controller;
    std::vector<std::unique_ptr<AppRow>> rows;
    std::vector<std::unique_ptr<RuleRow>> ruleRows;
    Dropdown fallback;
    int left = 0, columnWidth = 0, titleY = 0, subtitleY = 0, playingHeaderY = 0, rememberedHeaderY = 0, footerY = 0;
    int emptyPlayingY = 0, emptyRememberedY = 0, fallbackLabelWidth = 0;
};

AppsView::AppsView (AppController& c) : controller (c)
{
    content = std::make_unique<Content> (c);
    viewport.setViewedComponent (content.get(), false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    viewport.setWantsKeyboardFocus (false);
    addAndMakeVisible (viewport);

    syncFromModel();
}

AppsView::~AppsView()
{
    viewport.setViewedComponent (nullptr, false);
}

void AppsView::syncFromModel()
{
    juce::String sig;

    for (const auto& stream : controller.getBackend().getAppStreams())
        sig << juce::String (stream.nodeId) << "|" << stream.appName << "|" << stream.mediaName << "|" << stream.channelId << ";";

    sig << "#";

    for (const auto& rule : controller.getState().getRoutingRules())
        sig << rule.pattern << "|" << rule.channelId << ";";

    sig << "#" << controller.getState().getFallbackChannel() << "#";

    for (const auto& info : controller.getState().getChannelInfos())
        sig << info.id << "|" << info.name << "|" << info.colour << ";";

    if (sig != signature)
    {
        signature = sig;
        content->rebuild();
    }
}

void AppsView::resized()
{
    viewport.setBounds (getLocalBounds());
    content->layoutFor (viewport.getMaximumVisibleWidth());
}

void AppsView::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg0);
}
}
