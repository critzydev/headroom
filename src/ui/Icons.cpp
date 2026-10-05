// Icons: parses the embedded Lucide SVGs once per (icon, size) with the stroke width fixed at 1.5 px.
#include "ui/Icons.h"

#include <BinaryData.h>
#include <map>

namespace hr
{
namespace
{
    struct CachedIcon
    {
        std::unique_ptr<juce::Drawable> drawable;
        juce::Colour colour { 0xffffffff };
    };

    std::map<juce::String, CachedIcon>& cache()
    {
        static std::map<juce::String, CachedIcon> icons;
        return icons;
    }

    juce::String loadSvgText (const juce::String& name)
    {
        const auto wanted = name + ".svg";

        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        {
            if (wanted == BinaryData::originalFilenames[i])
            {
                int size = 0;

                if (auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size))
                    return juce::String::fromUTF8 (data, size);
            }
        }

        jassertfalse;       // unknown icon name
        return {};
    }

    std::unique_ptr<juce::Drawable> parse (const juce::String& name, int sizePx)
    {
        auto svg = loadSvgText (name);

        if (svg.isEmpty())
            return nullptr;

        // Lucide draws on a 24 px grid with stroke-width 2. Rescale the stroke so it renders 1.5 px at `sizePx`.
        const auto strokeWidth = juce::String (1.5 * 24.0 / (double) sizePx, 3);
        svg = svg.replace ("currentColor", "#ffffff").replace ("stroke-width=\"2\"", "stroke-width=\"" + strokeWidth + "\"");

        return juce::Drawable::createFromSVGString (svg);
    }

    CachedIcon& get (const juce::String& name, int sizePx)
    {
        const auto key = name + "@" + juce::String (sizePx);
        auto& icons = cache();

        if (auto it = icons.find (key); it != icons.end())
            return it->second;

        CachedIcon icon;
        icon.drawable = parse (name, sizePx);
        return icons.emplace (key, std::move (icon)).first->second;
    }
}

void Icons::draw (juce::Graphics& g, const juce::String& name, juce::Rectangle<float> area, float size, juce::Colour colour)
{
    draw (g, name, area.getCentre(), size, colour);
}

void Icons::draw (juce::Graphics& g, const juce::String& name, juce::Point<float> centre, float size, juce::Colour colour)
{
    auto& icon = get (name, juce::roundToInt (size));

    if (icon.drawable == nullptr)
        return;

    if (icon.colour != colour)
    {
        icon.drawable->replaceColour (icon.colour, colour);
        icon.colour = colour;
    }

    const auto target = juce::Rectangle<float> (size, size).withCentre (centre);
    icon.drawable->drawWithin (g, target, juce::RectanglePlacement::centred, 1.0f);
}

std::unique_ptr<juce::Drawable> Icons::createDrawable (const juce::String& name, float size, juce::Colour colour)
{
    auto drawable = parse (name, juce::roundToInt (size));

    if (drawable != nullptr)
    {
        drawable->replaceColour (juce::Colour (0xffffffff), colour);
    }

    return drawable;
}

juce::String Icons::fileForChannelIcon (const juce::String& modelName)
{
    if (modelName == "mic")         return "mic";
    if (modelName == "monitor")     return "monitor";
    if (modelName == "gamepad")     return "gamepad-2";
    if (modelName == "music")       return "music";
    if (modelName == "message")     return "message-circle";
    if (modelName == "headphones")  return "headphones";
    if (modelName == "globe")       return "globe";
    if (modelName == "video")       return "video";
    if (modelName == "radio")       return "radio";
    if (modelName == "tv")          return "tv-minimal";
    return "volume-2";              // "speaker" and anything unknown
}

const juce::StringArray& Icons::channelIconChoices()
{
    static const juce::StringArray choices { "monitor", "gamepad", "music", "message", "headphones", "globe", "video", "radio", "tv", "speaker", "mic" };
    return choices;
}

juce::String Icons::displayNameForChannelIcon (const juce::String& modelName)
{
    if (modelName == "monitor")     return "Computer";
    if (modelName == "gamepad")     return "Game";
    if (modelName == "music")       return "Music";
    if (modelName == "message")     return "Chat";
    if (modelName == "headphones")  return "Headphones";
    if (modelName == "globe")       return "Web";
    if (modelName == "video")       return "Video";
    if (modelName == "radio")       return "Radio";
    if (modelName == "tv")          return "TV";
    if (modelName == "mic")         return "Microphone";
    return "Speaker";
}

void Icons::releaseAll()
{
    cache().clear();
}
}
