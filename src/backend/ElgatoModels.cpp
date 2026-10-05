// The Elgato model table and the rules for recognising a model (see ElgatoModels.h).
#include "backend/ElgatoModels.h"

#include <algorithm>
#include <cctype>

namespace hr::elgato
{
namespace
{
    constexpr const char* kAnalogMono = "output:analog-stereo+input:mono-fallback";

    // One line per product. The numbers come from USB descriptors, PipeWire and ALSA dumps, user reports and published protocol
    // descriptions; where nothing is known the table says so (0, or Clock::Assumed).
    //
    // captureChannels / playbackChannels are what a USB descriptor or a PipeWire dump showed for the mic and the headphones
    // (0 where nobody has published one). goodProfile is only given for the models that the WirePlumber drop-in pins and
    // whose cards have the Wave:3's UAC1 layout (one capture channel, one stereo playback): for the others the best profile
    // the card offers is as good as any, and a mono-fallback pin would be wrong for the multichannel interfaces.
    const std::vector<Model> kModels
    {
        //   pid     name                       short            node fragment           kind                clock            cap play profile     pair hardware                usb  gain
        { 0x006e, "Elgato Wave:1",          "Wave:1",        "Wave_1_",              Kind::Microphone, Clock::Reported, 1,  2,  kAnalogMono, 0, Hardware::None,         0,   0.0 },
        { 0x0070, "Elgato Wave:3",          "Wave:3",        "Wave_3_",              Kind::Microphone, Clock::Reported, 1,  2,  kAnalogMono, 0, Hardware::Tested,       16,  40.0 },
        { 0x00ca, "Elgato Wave:3 MK.2",     "Wave:3 MK.2",   "Wave_3_MK.2_",         Kind::Microphone, Clock::Assumed,  0,  0,  "",          0, Hardware::None,         0,   0.0 },
        { 0x0098, "Elgato Wave Neo",        "Wave Neo",      "Wave_Neo_",            Kind::Microphone, Clock::Assumed,  0,  0,  kAnalogMono, 0, Hardware::None,         0,   0.0 },
        { 0x007d, "Elgato Wave XLR",        "Wave XLR",      "Wave_XLR_",            Kind::Interface,  Clock::Reported, 1,  2,  kAnalogMono, 0, Hardware::Experimental, 34,  75.0 },
        { 0x00a6, "Elgato XLR Dock",        "XLR Dock",      "XLR_Dock_",            Kind::Interface,  Clock::Reported, 0,  0,  "",          0, Hardware::None,         0,   0.0 },
        { 0x00b6, "Elgato Wave XLR MK.2",   "Wave XLR MK.2", "Wave_XLR_MK.2_",       Kind::Interface,  Clock::Assumed,  2,  2,  "",          0, Hardware::None,         0,   0.0 },
        { 0x00c7, "Elgato XLR Dock MK.2",   "XLR Dock MK.2", "Wave_XLR_Dock_MK.2_",  Kind::Interface,  Clock::Assumed,  2,  2,  "",          0, Hardware::None,         0,   0.0 },
        { 0x00b4, "Elgato Wave XLR Pro",    "Wave XLR Pro",  "Wave_XLR_Pro_",        Kind::Interface,  Clock::Assumed,  18, 17, "",          1, Hardware::None,         0,   0.0 },
    };

    std::string lowerCase (std::string text)
    {
        std::transform (text.begin(), text.end(), text.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        return text;
    }

    // Elgato's other USB audio devices (capture cards, cameras) are not microphones: nothing is done to them.
    bool looksLikeNotAMicrophone (const std::string& text)
    {
        if (containsIgnoreCase (text, "wave") || containsIgnoreCase (text, "xlr"))
            return false;

        for (const char* word : { "capture", "hd60", "4k", "cam link", "camlink", "cam_link", "facecam", "stream deck",
                                  "streamdeck", "stream_deck", "key light", "key_light" })
            if (containsIgnoreCase (text, word))
                return true;

        return false;
    }

    // "usb-Elgato..." is how udev starts the name of any Elgato USB audio device ("Elgato_Systems_..." for the older
    // ones, "Elgato_Elgato_..." for the MK.2 generation, which says only "Elgato" in its manufacturer string).
    bool hasElgatoUsbName (const std::string& name)
    {
        return name.find ("usb-Elgato") != std::string::npos;
    }
}

bool containsIgnoreCase (const std::string& text, const std::string& part)
{
    return lowerCase (text).find (lowerCase (part)) != std::string::npos;
}

const std::vector<Model>& models()
{
    return kModels;
}

const Model* findByPid (int productId)
{
    for (const auto& model : kModels)
        if (productId == (int) model.pid)
            return &model;

    return nullptr;
}

const Model* findByNodeName (const std::string& name)
{
    if (! hasElgatoUsbName (name))
        return nullptr;

    const Model* best = nullptr;
    size_t bestLength = 0;

    for (const auto& model : kModels)
    {
        const std::string fragment = std::string ("_") + model.nodeFragment;

        if (name.find (fragment) != std::string::npos && fragment.size() > bestLength)
        {
            best = &model;
            bestLength = fragment.size();
        }
    }

    return best;
}

Identity identify (int vendorId, int productId, const std::string& name, const std::string& description)
{
    Identity who;
    who.productId = productId;

    if (vendorId >= 0 && vendorId != kVendorId)
        return who;                                         // somebody else's device

    const Model* byName = findByNodeName (name);

    if (vendorId == kVendorId)
    {
        who.elgato = true;

        // A product id that is not in the table stays unknown: the node name of a newer model can start like an older one's.
        who.model = productId >= 0 ? findByPid (productId) : byName;
        who.wave = who.model != nullptr || ! looksLikeNotAMicrophone (name + " " + description);
        return who;
    }

    // No ids behind the node (a fake in a test, or a PipeWire that does not say): the node name has to do.
    if (byName != nullptr)
    {
        who.elgato = who.wave = true;
        who.model = byName;
    }
    else if (hasElgatoUsbName (name) && (containsIgnoreCase (name, "wave") || containsIgnoreCase (name, "xlr")))
    {
        who.elgato = who.wave = true;
    }

    return who;
}

std::string label (const Identity& who, bool input, const std::string& deviceDescription, const std::string& deviceNick,
                   const std::string& nodeDescription, const std::string& nodeNick)
{
    std::string base;

    if (who.model != nullptr)
    {
        base = who.model->name;
    }
    else
    {
        for (const auto* text : { &deviceDescription, &deviceNick, &nodeDescription, &nodeNick })
            if (! text->empty())
            {
                base = *text;
                break;
            }

        if (base.empty())
            base = "Wave";

        if (! containsIgnoreCase (base, "elgato"))
            base = "Elgato " + base;
    }

    return input ? base : base + " Headphones";
}

std::string shortName (const Identity& who, const std::string& fallbackLabel)
{
    if (who.model != nullptr)
        return who.model->shortName;

    return fallbackLabel.size() > 7 && lowerCase (fallbackLabel.substr (0, 7)) == "elgato " ? fallbackLabel.substr (7) : fallbackLabel;
}

std::vector<uint16_t> pidsWith (Hardware level)
{
    std::vector<uint16_t> pids;

    for (const auto& model : kModels)
        if (model.hardware == level)
            pids.push_back (model.pid);

    return pids;
}

const char* describe (Hardware level)
{
    switch (level)
    {
        case Hardware::None:            return "none";
        case Hardware::Tested:          return "tested";
        case Hardware::Experimental:    return "experimental";
    }

    return "";
}

const char* describe (Clock clock)
{
    return clock == Clock::Reported ? "reported" : "assumed";
}
}
