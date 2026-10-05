// createPlatformBackend() for platforms without PipeWire. On Linux the definition lives in
// backend/pipewire/PipeWireBackendFactory.cpp instead; exactly one of the two is ever compiled.
#include "backend/AudioBackend.h"

#if ! (JUCE_LINUX || JUCE_BSD)

#include "backend/NullBackend.h"

namespace hr
{
std::unique_ptr<AudioBackend> createPlatformBackend()
{
    return std::make_unique<NullBackend>();
}
}

#endif
