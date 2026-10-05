// Implementation of DeviceControl (profiles, routes, mute, hardware volume).
#include "DeviceControl.h"

#include "backend/wave/WaveGain.h"

#include <cmath>

namespace hr::pw
{

void DeviceControl::reset()
{
    states.clear();
    watched.clear();
}

void DeviceControl::handleDeviceRemoved (uint32_t deviceId)
{
    states.erase (deviceId);
    watched.erase (deviceId);
}

void DeviceControl::watch (uint32_t deviceId)
{
    if (deviceId == SPA_ID_INVALID || watched.count (deviceId) > 0)
        return;

    if (registry.subscribeDeviceParams (deviceId))
        watched.insert (deviceId);   // if the proxy isn't bound yet we simply try again next pass
}

const DeviceState* DeviceControl::state (uint32_t deviceId) const
{
    const auto found = states.find (deviceId);
    return found != states.end() ? &found->second : nullptr;
}

// -------------------------------------------------------------------------------------------------
// Reading params

void DeviceControl::handleParam (uint32_t deviceId, uint32_t paramId, uint32_t index, const spa_pod* param)
{
    if (param == nullptr)
        return;

    auto& deviceState = states[deviceId];

    if (paramId == SPA_PARAM_EnumProfile || paramId == SPA_PARAM_Profile)
        parseProfile (deviceState, paramId, index, param);
    else if (paramId == SPA_PARAM_Route)
        parseRoute (deviceState, index, param);
    else
        return;

    if (onChanged)
        onChanged();
}

void DeviceControl::parseProfile (DeviceState& deviceState, uint32_t paramId, uint32_t index, const spa_pod* param)
{
    int32_t profileIndex = -1;
    const char* name = nullptr;
    const char* description = nullptr;
    int32_t priority = 0;
    uint32_t available = SPA_PARAM_AVAILABILITY_unknown;
    spa_pod* classes = nullptr;

    if (spa_pod_parse_object (param, SPA_TYPE_OBJECT_ParamProfile, nullptr,
                              SPA_PARAM_PROFILE_index, SPA_POD_Int (&profileIndex),
                              SPA_PARAM_PROFILE_name, SPA_POD_String (&name),
                              SPA_PARAM_PROFILE_description, SPA_POD_OPT_String (&description),
                              SPA_PARAM_PROFILE_priority, SPA_POD_OPT_Int (&priority),
                              SPA_PARAM_PROFILE_available, SPA_POD_OPT_Id (&available),
                              SPA_PARAM_PROFILE_classes, SPA_POD_OPT_PodStruct (&classes)) < 0)
        return;

    if (paramId == SPA_PARAM_Profile)
    {
        deviceState.activeProfile = profileIndex;
        deviceState.activeProfileName = name != nullptr ? name : "";
        deviceState.activeSeen = true;

        // "off" has no routes; the server won't send an empty Route list, so drop the stale ones.
        if (deviceState.activeProfileName == "off")
            deviceState.routes.clear();

        return;
    }

    // EnumProfile: a new enumeration always starts at index 0.
    if (index == 0)
        deviceState.profiles.clear();

    ProfileRec profile;
    profile.index = profileIndex;
    profile.name = name != nullptr ? name : "";
    profile.description = description != nullptr ? description : profile.name;
    profile.priority = priority;
    profile.available = available != SPA_PARAM_AVAILABILITY_no;

    if (classes != nullptr)
    {
        const void* body = SPA_POD_BODY (classes);
        const uint32_t bodySize = SPA_POD_BODY_SIZE (classes);

        for (const spa_pod* item = static_cast<const spa_pod*> (body);
             spa_pod_is_inside (body, bodySize, item);
             item = static_cast<const spa_pod*> (spa_pod_next (item)))
        {
            spa_pod_parser parser;
            spa_pod_parser_pod (&parser, item);

            const char* mediaClass = nullptr;
            int32_t count = 0;

            if (spa_pod_parser_get_struct (&parser, SPA_POD_String (&mediaClass), SPA_POD_Int (&count)) < 0
                || mediaClass == nullptr)
                continue;

            if (std::strcmp (mediaClass, "Audio/Sink") == 0)
                profile.sinks += count;
            else if (std::strcmp (mediaClass, "Audio/Source") == 0)
                profile.sources += count;
        }
    }

    deviceState.profiles.push_back (std::move (profile));
    deviceState.profilesSeen = true;
}

void DeviceControl::parseRoute (DeviceState& deviceState, uint32_t index, const spa_pod* param)
{
    int32_t routeIndex = -1;
    int32_t device = -1;
    uint32_t direction = SPA_ID_INVALID;
    const char* name = nullptr;
    spa_pod* props = nullptr;

    if (spa_pod_parse_object (param, SPA_TYPE_OBJECT_ParamRoute, nullptr,
                              SPA_PARAM_ROUTE_index, SPA_POD_Int (&routeIndex),
                              SPA_PARAM_ROUTE_direction, SPA_POD_OPT_Id (&direction),
                              SPA_PARAM_ROUTE_device, SPA_POD_OPT_Int (&device),
                              SPA_PARAM_ROUTE_name, SPA_POD_OPT_String (&name),
                              SPA_PARAM_ROUTE_props, SPA_POD_OPT_PodObject (&props)) < 0)
        return;

    if (index == 0)
        deviceState.routes.clear();

    RouteRec route;
    route.index = routeIndex;
    route.direction = direction == SPA_ID_INVALID ? -1 : (int) direction;
    route.device = device;
    route.name = name != nullptr ? name : "";

    if (props != nullptr && spa_pod_is_object_type (props, SPA_TYPE_OBJECT_Props))
    {
        const auto* object = reinterpret_cast<const spa_pod_object*> (props);
        const spa_pod_prop* prop = nullptr;

        SPA_POD_OBJECT_FOREACH (object, prop)
        {
            switch (prop->key)
            {
                case SPA_PROP_mute:
                {
                    bool value = false;

                    if (spa_pod_get_bool (&prop->value, &value) >= 0)
                        route.mute = value;

                    break;
                }

                case SPA_PROP_softMute:
                {
                    bool value = false;

                    if (spa_pod_get_bool (&prop->value, &value) >= 0)
                        route.softMute = value;

                    break;
                }

                case SPA_PROP_channelVolumes:
                {
                    float values[32];
                    const uint32_t count = spa_pod_copy_array (&prop->value, SPA_TYPE_Float, values, 32);
                    route.volumes.assign (values, values + count);
                    break;
                }

                default:
                    break;
            }
        }
    }

    deviceState.routes.push_back (std::move (route));
}

// -------------------------------------------------------------------------------------------------
// Profiles

bool DeviceControl::setProfile (uint32_t deviceId, int profileIndex, bool save)
{
    if (profileIndex < 0)
        return false;

    uint8_t buffer[512];
    spa_pod_builder builder;
    spa_pod_builder_init (&builder, buffer, sizeof (buffer));

    const auto* pod = static_cast<const spa_pod*> (
        spa_pod_builder_add_object (&builder, SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
                                    SPA_PARAM_PROFILE_index, SPA_POD_Int (profileIndex),
                                    SPA_PARAM_PROFILE_save, SPA_POD_Bool (save)));

    const bool ok = registry.setDeviceParam (deviceId, SPA_PARAM_Profile, pod);
    pwLog ("set profile %d on device %u (save=%d): %s", profileIndex, deviceId, save ? 1 : 0, ok ? "ok" : "failed");
    return ok;
}

// -------------------------------------------------------------------------------------------------
// Routes

const RouteRec* DeviceControl::findRoute (const DeviceState& deviceState, int direction, int cardDevice)
{
    const RouteRec* fallback = nullptr;

    for (const auto& route : deviceState.routes)
    {
        if (route.direction != direction)
            continue;

        if (cardDevice >= 0 && route.device == cardDevice)
            return &route;

        if (fallback == nullptr)
            fallback = &route;
    }

    return cardDevice < 0 ? fallback : nullptr;
}

bool DeviceControl::setRoute (uint32_t deviceId, const RouteRec& route, std::optional<bool> mute, std::optional<float> volume)
{
    if (route.index < 0 || route.device < 0)
        return false;

    uint8_t buffer[1024];
    spa_pod_builder builder;
    spa_pod_builder_init (&builder, buffer, sizeof (buffer));

    spa_pod_frame outer;
    spa_pod_frame inner;

    spa_pod_builder_push_object (&builder, &outer, SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
    spa_pod_builder_add (&builder,
                         SPA_PARAM_ROUTE_index, SPA_POD_Int (route.index),
                         SPA_PARAM_ROUTE_device, SPA_POD_Int (route.device),
                         0);
    spa_pod_builder_prop (&builder, SPA_PARAM_ROUTE_props, 0);
    spa_pod_builder_push_object (&builder, &inner, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

    if (volume.has_value())
    {
        const uint32_t channels = route.volumes.empty() ? (route.direction == 0 ? 1u : 2u)
                                                        : (uint32_t) route.volumes.size();
        float values[32];

        for (uint32_t i = 0; i < channels && i < 32; ++i)
            values[i] = *volume;

        spa_pod_builder_add (&builder,
                             SPA_PROP_channelVolumes, SPA_POD_Array ((uint32_t) sizeof (float), (uint32_t) SPA_TYPE_Float,
                                                                     channels, values),
                             0);
    }

    if (mute.has_value())
    {
        const bool muteValue = *mute;
        spa_pod_builder_add (&builder, SPA_PROP_mute, SPA_POD_Bool (muteValue), 0);
    }

    spa_pod_builder_pop (&builder, &inner);
    spa_pod_builder_prop (&builder, SPA_PARAM_ROUTE_save, 0);
    spa_pod_builder_bool (&builder, true);

    const auto* pod = static_cast<const spa_pod*> (spa_pod_builder_pop (&builder, &outer));
    return registry.setDeviceParam (deviceId, SPA_PARAM_Route, pod);
}

// The one implementation lives in backend/wave/WaveGain.h, where Safe gain's dB maths and its unit tests use it too.
float DeviceControl::sliderToVolume (float slider) noexcept   { return wave::sliderToVolume (slider); }
float DeviceControl::volumeToSlider (float volume) noexcept   { return wave::volumeToSlider (volume); }

} // namespace hr::pw
