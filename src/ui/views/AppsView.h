// AppsView: which channel each running app plays through, the remembered rules, and where new apps go.
#pragma once

#include "Juce.h"
#include "app/AppController.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Dropdown.h"
#include "ui/widgets/ScrollView.h"

#include <memory>
#include <vector>

namespace hr
{
class AppsView final : public juce::Component
{
public:
    explicit AppsView (AppController& controller);
    ~AppsView() override;

    // Rebuilds the rows only if what they show has changed (so an open menu isn't pulled out from under the user).
    void syncFromModel();

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Content;

    AppController& controller;
    std::unique_ptr<Content> content;
    ScrollView viewport { false, 64 };
    juce::String signature;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppsView)
};
}
