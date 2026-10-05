// AddEffectPanel: the "Add effect" popover of the FX drawer. Built-ins first, then the searchable plugin list grouped
// by vendor with format tags, then "Rescan plugins" and the scan progress.
#pragma once

#include "Juce.h"
#include "app/AppController.h"
#include "ui/widgets/ScrollView.h"

#include <functional>
#include <vector>

namespace hr
{
class AddEffectPanel final : public juce::Component,
                             private juce::Timer
{
public:
    explicit AddEffectPanel (AppController& controller);
    ~AddEffectPanel() override;

    std::function<void (const juce::String&)> onPickBuiltin;
    std::function<void (const juce::PluginDescription&)> onPickPlugin;
    std::function<void()> onDismiss;

    void open();                    // resets the search and focuses it
    void refresh();                 // plugin list or scan state changed

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void visibilityChanged() override;

    static constexpr int kMaxHeight = 440;

private:
    struct Entry
    {
        enum class Type { header, vendor, builtin, plugin, note };

        Type type = Type::note;
        juce::String text, sub, tag, builtinId;
        juce::PluginDescription description;
        int height = 0;
        bool selectable() const noexcept    { return type == Type::builtin || type == Type::plugin; }
    };

    class List;
    class SearchField;

    void timerCallback() override;
    void rebuildEntries();
    void pick (int entryIndex);
    void moveSelection (int direction);
    void paintFooter (juce::Graphics&, juce::Rectangle<int> area) const;

    AppController& controller;
    std::vector<Entry> entries;
    std::unique_ptr<SearchField> search;
    std::unique_ptr<List> list;
    ScrollView viewport { false, 44 };
    juce::TextButton rescanButton { "Rescan plugins" };
    juce::Rectangle<int> footerArea;
    bool wasScanning = false;
    int lastProgressPercent = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddEffectPanel)
};
}
