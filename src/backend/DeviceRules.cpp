// Device choice, port mapping, WaveGuard's allowances and the mic sentences (see DeviceRules.h).
#include "backend/DeviceRules.h"

#include <algorithm>

namespace hr::devices
{
// ---------------------------------------------------------------------------------------------------
// Which device
// ---------------------------------------------------------------------------------------------------

int pickCandidate (const std::vector<Candidate>& candidates, const std::string& requestedId)
{
    if (! requestedId.empty() && requestedId != "auto")
    {
        for (size_t i = 0; i < candidates.size(); ++i)
            if (candidates[i].name == requestedId)
                return (int) i;
    }

    int bestWave = -1, bestReal = -1, bestAny = -1;

    const auto better = [&] (int candidate, int current)
    {
        return current < 0 || candidates[(size_t) candidate].priority > candidates[(size_t) current].priority;
    };

    // Among the Wave's own nodes a Pro Audio one ("...pro-input-0") always loses to a normal one: Pro Audio has no hardware
    // gain control. Seen on a real Wave:3, where an old drop-in had raised the Pro Audio nodes' priority and both node sets
    // existed at once, so the gain could not be restored.
    const auto isProAudio = [&] (int index) { return candidates[(size_t) index].name.find (".pro-") != std::string::npos; };
    const auto betterWave = [&] (int candidate, int current)
    {
        if (current >= 0 && isProAudio (candidate) != isProAudio (current))
            return ! isProAudio (candidate);

        return better (candidate, current);
    };

    for (int i = 0; i < (int) candidates.size(); ++i)
    {
        const auto& candidate = candidates[(size_t) i];

        if (candidate.wave && betterWave (i, bestWave))
            bestWave = i;

        if (candidate.real && better (i, bestReal))
            bestReal = i;

        if (better (i, bestAny))
            bestAny = i;
    }

    if (bestWave >= 0)
        return bestWave;

    return bestReal >= 0 ? bestReal : bestAny;
}

void sortDeviceList (std::vector<hr::DeviceInfo>& list)
{
    std::sort (list.begin(), list.end(), [] (const hr::DeviceInfo& a, const hr::DeviceInfo& b)
    {
        if (a.isWave != b.isWave)
            return a.isWave;

        const int order = a.label.compareIgnoreCase (b.label);
        return order != 0 ? order < 0 : a.id < b.id;
    });
}

std::vector<OutputChoice> resolveOutputs (const std::vector<Candidate>& candidates, const std::vector<std::string>& requested,
                                          bool fallBackToAuto)
{
    std::vector<OutputChoice> result;
    std::vector<int> taken;
    bool anyPresent = false;

    const auto add = [&] (const std::string& id)
    {
        OutputChoice choice;
        choice.requested = id;
        choice.automatic = id == "auto";

        if (choice.automatic)
        {
            choice.index = pickCandidate (candidates, "auto");
        }
        else
        {
            for (size_t i = 0; i < candidates.size(); ++i)
                if (candidates[i].name == id)
                {
                    choice.index = (int) i;
                    break;
                }
        }

        if (choice.index >= 0)
        {
            if (std::find (taken.begin(), taken.end(), choice.index) != taken.end())
                return;         // the same device twice (an explicit entry and what "auto" means): once

            taken.push_back (choice.index);
            anyPresent = true;
        }
        else
        {
            for (const auto& other : result)
                if (other.requested == id)
                    return;     // the same missing device twice
        }

        result.push_back (std::move (choice));
    };

    for (const auto& id : requested)
        if (! id.empty())
            add (id);

    if (fallBackToAuto && ! anyPresent)
        add ("auto");

    return result;
}

std::string cardOfNodeName (const std::string& nodeName)
{
    for (const char* prefix : { "alsa_input.", "alsa_output." })
    {
        const std::string head (prefix);

        if (nodeName.compare (0, head.size(), head) != 0)
            continue;

        const auto rest = nodeName.substr (head.size());
        const auto lastDot = rest.rfind ('.');

        return lastDot == std::string::npos || lastDot == 0 ? std::string() : rest.substr (0, lastDot);
    }

    return {};
}

std::string friendlyNameFromNodeName (const std::string& nodeName)
{
    auto card = cardOfNodeName (nodeName);

    for (const char* bus : { "usb-", "pci-", "platform-" })
    {
        const std::string head (bus);

        if (card.compare (0, head.size(), head) == 0)
        {
            card = card.substr (head.size());
            break;
        }
    }

    // "..._ABC123-00": the serial number and the card's index are not a name.
    const auto dash = card.rfind ('-');

    if (dash != std::string::npos && dash + 1 < card.size()
        && card.find_first_not_of ("0123456789", dash + 1) == std::string::npos)
        card = card.substr (0, dash);

    for (auto& c : card)
        if (c == '_')
            c = ' ';

    return card.empty() ? nodeName : card;
}

// ---------------------------------------------------------------------------------------------------
// Which ports
// ---------------------------------------------------------------------------------------------------

StereoPorts pickStereoPorts (const std::vector<PortFacts>& ports)
{
    uint32_t fl = kNoPort, fr = kNoPort, mono = kNoPort;

    for (const auto& port : ports)
    {
        if (port.channel == "FL" && fl == kNoPort)
            fl = port.id;
        else if (port.channel == "FR" && fr == kNoPort)
            fr = port.id;
        else if (port.channel == "MONO" && mono == kNoPort)
            mono = port.id;
    }

    StereoPorts result;

    if (fl != kNoPort)
    {
        result.left = fl;
        result.right = fr;
    }
    else if (mono != kNoPort)
    {
        result.left = mono;
    }
    else if (! ports.empty())
    {
        result.left = ports[0].id;

        if (ports.size() > 1)
            result.right = ports[1].id;
    }

    return result;
}

namespace
{
    // Channel 1: FL or MONO by name, else the first port given.
    StereoPorts pickFirstChannel (const std::vector<PortFacts>& ports)
    {
        StereoPorts result;

        for (const char* wanted : { "FL", "MONO" })
            for (const auto& port : ports)
                if (port.channel == wanted)
                {
                    result.left = port.id;
                    return result;
                }

        if (! ports.empty())
            result.left = ports[0].id;

        return result;
    }
}

StereoPorts pickMicPorts (const std::vector<PortFacts>& ports, bool elgato)
{
    if (elgato || ports.size() > 2)
        return pickFirstChannel (ports);

    return pickStereoPorts (ports);
}

StereoPorts pickInputPorts (const std::vector<PortFacts>& ports, bool stereo)
{
    StereoPorts result;

    if (stereo)
    {
        result = pickStereoPorts (ports);

        if (result.right == kNoPort)
            result.right = result.left;     // one port: both sides get it
    }
    else
    {
        result = pickFirstChannel (ports);
        result.right = result.left;
    }

    return result;
}

std::vector<PlannedLink> planFanOut (uint32_t outLeft, uint32_t outRight, const std::vector<StereoPorts>& sinks)
{
    std::vector<PlannedLink> links;

    for (const auto& sink : sinks)
    {
        if (outLeft != kNoPort && sink.left != kNoPort)
            links.push_back ({ outLeft, sink.left });

        if (outRight != kNoPort && sink.right != kNoPort)
            links.push_back ({ outRight, sink.right });
    }

    return links;
}

std::vector<PlannedLink> planInputSlot (const StereoPorts& source, uint32_t slotLeft, uint32_t slotRight)
{
    std::vector<PlannedLink> links;

    if (source.left != kNoPort && slotLeft != kNoPort)
        links.push_back ({ source.left, slotLeft });

    if (source.right != kNoPort && slotRight != kNoPort)
        links.push_back ({ source.right, slotRight });

    return links;
}

StereoPorts pickPlaybackPorts (const std::vector<PortFacts>& ports, int pair)
{
    if (pair > 0 && ports.size() >= (size_t) (2 * (pair + 1)))
    {
        StereoPorts result;
        result.left = ports[(size_t) (2 * pair)].id;
        result.right = ports[(size_t) (2 * pair + 1)].id;
        return result;
    }

    return pickStereoPorts (ports);
}

// ---------------------------------------------------------------------------------------------------
// What WaveGuard may do
// ---------------------------------------------------------------------------------------------------

GuardPlan guardPlanFor (const elgato::Identity& input)
{
    GuardPlan plan;
    plan.captureFirst = plan.recoverSilentMic = plan.unmute = input.elgato && input.wave;
    return plan;
}

bool mayFixProfile (const elgato::Identity& device)
{
    return device.elgato && device.wave;
}

bool inputNeedsCaptureFirst (const elgato::Identity& device)
{
    return device.elgato && device.wave;
}

bool CaptureGate::update (bool needed, bool present, bool linked, uint32_t liveCycles, int64_t now)
{
    // Nothing to wait for: no capture to protect, or the device is not there (headphone audio must work without it).
    if (! needed || ! present)
    {
        phase = Phase::Idle;
        open = true;
        return open;
    }

    if (phase == Phase::Idle)
    {
        phase = Phase::Linking;
        phaseStartMs = now;
    }

    switch (phase)
    {
        case Phase::Linking:
            if (linked)
            {
                phase = Phase::Confirming;
                phaseStartMs = now;
            }
            else if (now - phaseStartMs >= kLinkGiveUpMs)
            {
                phase = Phase::Open;        // the capture cannot be linked: do not hold the headphones hostage
            }

            break;

        case Phase::Confirming:
            if (! linked)
            {
                phase = Phase::Linking;
                phaseStartMs = now;
            }
            else if (liveCycles >= kLiveCycles || now - phaseStartMs >= kConfirmMs)
            {
                phase = Phase::Open;
            }

            break;

        case Phase::Open:
        case Phase::Idle:
            break;
    }

    open = phase == Phase::Open;
    return open;
}

// ---------------------------------------------------------------------------------------------------
// Words
// ---------------------------------------------------------------------------------------------------

MicWording wordingFor (const elgato::Identity& input, const std::string& inputLabel)
{
    MicWording wording;
    wording.elgato = input.elgato && input.wave;
    wording.wave3 = input.isWave3();

    if (wording.elgato)
        wording.shortName = elgato::shortName (input, inputLabel);

    return wording;
}

juce::String micMessage (hr::MicState state, const MicWording& wording)
{
    const auto em = [] (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); };
    const juce::String name (wording.shortName);

    switch (state)
    {
        case hr::MicState::NotFound:
            return "No microphone found. Plug in your microphone or choose one in Settings.";

        case hr::MicState::Recovering:
            return "Fixing your mic. This takes a few seconds.";

        case hr::MicState::SilentFromStart:
            if (wording.wave3)
                return "No sound from your mic. If the ring on your Wave:3 is red, tap the top of the mic to unmute.";

            if (wording.elgato)
                return "No sound from your mic. If your " + name + " is muted, unmute it on the device.";

            return "No sound from your mic. Check that it is plugged in and not muted.";

        case hr::MicState::MutedOnDevice:
            if (wording.wave3)
                return em ("Mic muted on the Wave:3 \xe2\x80\x94 tap the top to unmute");

            if (wording.elgato)
                return em ("Mic muted on the ") + name + em (" \xe2\x80\x94 unmute it on the device");

            return "Your mic is muted. Unmute it on the device.";

        case hr::MicState::Starting:
        case hr::MicState::Ok:
            break;
    }

    return {};
}
}
