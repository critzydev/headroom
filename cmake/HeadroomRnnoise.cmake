# Fetches the RNNoise v0.2 release tarball (it ships rnnoise_data.c, so no model download is needed)
# and defines the `rnnoise` static library ourselves, because upstream has no CMake build.
# The dependency name must stay "rnnoise" so -DFETCHCONTENT_SOURCE_DIR_RNNOISE=<path> works.
include(FetchContent)

FetchContent_Declare(rnnoise
    URL      https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz
    URL_HASH SHA256=90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(rnnoise)   # extracts only; the tarball has no CMakeLists.txt

set(HEADROOM_RNNOISE_DIR "${rnnoise_SOURCE_DIR}")

add_library(rnnoise STATIC
    ${HEADROOM_RNNOISE_DIR}/src/denoise.c
    ${HEADROOM_RNNOISE_DIR}/src/rnn.c
    ${HEADROOM_RNNOISE_DIR}/src/pitch.c
    ${HEADROOM_RNNOISE_DIR}/src/kiss_fft.c
    ${HEADROOM_RNNOISE_DIR}/src/celt_lpc.c
    ${HEADROOM_RNNOISE_DIR}/src/nnet.c
    ${HEADROOM_RNNOISE_DIR}/src/nnet_default.c
    ${HEADROOM_RNNOISE_DIR}/src/parse_lpcnet_weights.c
    ${HEADROOM_RNNOISE_DIR}/src/rnnoise_data.c
    ${HEADROOM_RNNOISE_DIR}/src/rnnoise_tables.c)

target_include_directories(rnnoise
    PUBLIC  ${HEADROOM_RNNOISE_DIR}/include
    PRIVATE ${HEADROOM_RNNOISE_DIR}/src)

set_target_properties(rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_definitions(rnnoise PRIVATE
        _CRT_SECURE_NO_WARNINGS OPUS_X86_MAY_HAVE_SSE OPUS_X86_MAY_HAVE_SSE2)
    target_compile_options(rnnoise PRIVATE /W0)
else()
    target_compile_options(rnnoise PRIVATE -w)   # third-party C, not ours to fix
    target_link_libraries(rnnoise PRIVATE m)
endif()
