// Profile and route access for ALSA devices (Wave:3): read what the device offers, set profile, mute and volume.
#pragma once

#include "PwCommon.h"
#include "PwRegistry.h"

#include "backend/ProfileRules.h"       // ProfileRec, RouteRec, DeviceState and the pure profile rules

namespace hr::pw
{

/**
    Everything works on device ids from the registry. State arrives through the params the
    registry forwards (EnumProfile, Profile, Route); writes go back through the same device proxy.
*/
class DeviceControl
{
public:
    explicit DeviceControl (PwRegistry& registryToUse) : registry (registryToUse) {}

    void reset();

    /** Fed by PwRegistry::onDeviceParam. */
    void handleParam (uint32_t deviceId, uint32_t paramId, uint32_t index, const spa_pod* param);
    void handleDeviceRemoved (uint32_t deviceId);

    /** Starts (once) receiving profile and route updates for this device. */
    void watch (uint32_t deviceId);

    const DeviceState* state (uint32_t deviceId) const;

    // ---- profiles ---------------------------------------------------------------------------
    /** Which profile is usable and which one is the best: backend/ProfileRules.h (namespace profiles), which needs no
        PipeWire. For an Elgato device "pro-audio" is not usable (no ACP routes: no hardware gain). */
    bool setProfile (uint32_t deviceId, int profileIndex, bool save);

    // ---- routes -----------------------------------------------------------------------------
    /** cardDevice < 0 matches any route of that direction. */
    static const RouteRec* findRoute (const DeviceState& state, int direction, int cardDevice);

    /** Sets mute and/or a linear channel volume (same value on every channel) and saves it. */
    bool setRoute (uint32_t deviceId, const RouteRec& route, std::optional<bool> mute, std::optional<float> volume);

    /** Slider position 0..1 <-> PipeWire's linear channel volume (cubic, like pactl's percentages). */
    static float sliderToVolume (float slider) noexcept;
    static float volumeToSlider (float volume) noexcept;

    /** Something about a device's profiles or routes changed. */
    std::function<void()> onChanged;

private:
    void parseProfile (DeviceState& state, uint32_t paramId, uint32_t index, const spa_pod* param);
    void parseRoute (DeviceState& state, uint32_t index, const spa_pod* param);

    PwRegistry& registry;
    std::map<uint32_t, DeviceState> states;
    std::set<uint32_t> watched;
};

} // namespace hr::pw
