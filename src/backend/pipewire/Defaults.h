// Keeps Headroom's sink and mic as the system default devices (metadata "default"), re-asserting when they are cleared, or changed if asked.
#pragma once

#include "PwCommon.h"
#include "PwRegistry.h"

#include "backend/DefaultKeeper.h"

namespace hr::pw
{

/**
    Writes default.configured.audio.sink / .source (Spa:String:JSON, {"name":"..."}).
    It asserts at start, when its nodes are (re)created, when the metadata itself is recreated
    (WirePlumber restart), and when someone clears the value (Steam's wpctl clear-default).

    Without keepDefault, a device the user explicitly picked is left alone. With keepDefault (the Wave Link
    style lock, the default) any other value (another device, or none) is put back about 300 ms after it appeared,
    at most 10 times a minute per device: past that another program is fighting us, so it gives up, says so
    (gaveUp()) and only tries again once that program has been quiet for a minute (DefaultKeeper).

    While disabled, values that still point at Headroom are removed so apps go back to the hardware.
*/
class Defaults
{
public:
    explicit Defaults (PwRegistry& registryToUse);

    void setEnabled (bool shouldBeDefault);
    void setKeep (bool shouldKeep);
    void setTargets (const std::string& sinkNodeName, const std::string& sourceNodeName);

    void evaluate (int64_t nowMs);

    /** The metadata object was (re)bound: assert everything again. */
    void metadataRebound();

    void reset();

    /** Headroom stopped putting a default back because another program keeps changing it. */
    bool gaveUp() const noexcept    { return sink.keeper.gaveUp() || source.keeper.gaveUp(); }

    static juce::String gaveUpMessage();

private:
    struct Kind
    {
        const char* key = "";
        const char* what = "";           // for the log: "output" / "input"
        std::string target;
        bool asserted = false;
        uint32_t assertedNodeId = SPA_ID_INVALID;
        int64_t lastWriteMs = 0;
        hr::DefaultKeeper keeper;
    };

    void evaluateKind (Kind& kind, int64_t now);
    void keepKind (Kind& kind, const std::string& current, int64_t now);
    bool writeTarget (Kind& kind, int64_t now);
    static std::string nameOf (const std::string& jsonValue);

    PwRegistry& registry;
    bool enabled = true;
    bool keep = true;                    // the setting's default; the app pushes the real value before start()
    Kind sink;
    Kind source;
};

} // namespace hr::pw
