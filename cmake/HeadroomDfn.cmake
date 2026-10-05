# DeepFilterNet (github.com/Rikorose/DeepFilterNet, MIT OR Apache-2.0) for the Fast and Best noise removal modes.
#
#  - libDF (tract runtime) is fetched at tag v0.5.6 (sparse checkout: only libDF/ is downloaded) and built by cargo,
#    together with our own small crate dfn-shim/ that puts a panic-safe C interface on it, into ONE static library.
#  - The two ONNX models are downloaded by URL + SHA256 into <build>/models/ (next to the binary). They are files,
#    not embedded: the program finds them relative to its own location (engine/denoise/DfnRuntime.cpp).
#
# Options / overrides (same idea as JUCE and rnnoise):
#   -DHEADROOM_DFN=OFF                                build without it; Fast and Best then use RNNoise
#   -DFETCHCONTENT_SOURCE_DIR_DEEPFILTERNET=<path>    use a local checkout of the repo instead of fetching it
#                                                     (its models/ folder is used too, after the same SHA256 check)
#   -DHEADROOM_CARGO=<path to cargo>
#
# Defines the imported static library `headroom_dfn` (link it to get the include path and the system libraries)
# and sets HEADROOM_DFN_ENABLED for the caller.
option(HEADROOM_DFN "Build with DeepFilterNet noise removal (needs Rust)" ON)

set(HEADROOM_DFN_ENABLED OFF)
add_library(headroom_dfn INTERFACE)

if(NOT HEADROOM_DFN)
    message(STATUS "DeepFilterNet: disabled (HEADROOM_DFN=OFF); noise removal modes Fast and Best fall back to RNNoise")
    return()
endif()

set(HEADROOM_DFN_REPO   "https://github.com/Rikorose/DeepFilterNet.git")
set(HEADROOM_DFN_TAG    "v0.5.6")
set(HEADROOM_DFN_COMMIT "978576aa8400552a4ce9730838c635aa30db5e61")

# file name -> SHA256 (the files at that commit; checked after every download / copy)
set(HEADROOM_DFN_MODELS
    "DeepFilterNet3_ll_onnx.tar.gz;5998e58e8ba0e09bb76986ef97b84afa065a571ef282d4a1222f341e3251cf3a"
    "DeepFilterNet3_onnx.tar.gz;c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616")

# ---- Rust ------------------------------------------------------------------------------------------------------
find_program(HEADROOM_CARGO cargo
    HINTS "$ENV{CARGO_HOME}/bin" "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin")

if(NOT HEADROOM_CARGO)
    message(FATAL_ERROR
        "DeepFilterNet needs Rust (cargo). Install it (Fedora: sudo dnf install rust cargo; elsewhere: https://rustup.rs) "
        "or configure with -DHEADROOM_DFN=OFF to build without it.")
endif()

# ---- DeepFilterNet source --------------------------------------------------------------------------------------
if(NOT FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")
endif()

if(FETCHCONTENT_SOURCE_DIR_DEEPFILTERNET)
    file(TO_CMAKE_PATH "${FETCHCONTENT_SOURCE_DIR_DEEPFILTERNET}" HEADROOM_DFN_SOURCE_DIR)     # forward slashes: it goes into a TOML file
else()
    set(HEADROOM_DFN_SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/deepfilternet-src")

    if(NOT EXISTS "${HEADROOM_DFN_SOURCE_DIR}/libDF/Cargo.toml")
        find_package(Git REQUIRED)
        message(STATUS "DeepFilterNet: fetching ${HEADROOM_DFN_TAG} (libDF only)")
        file(REMOVE_RECURSE "${HEADROOM_DFN_SOURCE_DIR}")
        # A blob-less shallow clone with a sparse checkout: a few MB instead of the whole repository (models, sample audio).
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" clone --quiet --depth 1 --filter=blob:none --no-checkout
                    --branch "${HEADROOM_DFN_TAG}" "${HEADROOM_DFN_REPO}" "${HEADROOM_DFN_SOURCE_DIR}"
            RESULT_VARIABLE dfn_result ERROR_VARIABLE dfn_error)
        if(dfn_result EQUAL 0)
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${HEADROOM_DFN_SOURCE_DIR}" sparse-checkout set --no-cone
                                    "/libDF/" "/LICENSE-MIT" "/LICENSE-APACHE"
                            RESULT_VARIABLE dfn_result ERROR_VARIABLE dfn_error)
        endif()
        if(dfn_result EQUAL 0)
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${HEADROOM_DFN_SOURCE_DIR}" checkout --quiet
                            RESULT_VARIABLE dfn_result ERROR_VARIABLE dfn_error)
        endif()
        if(NOT dfn_result EQUAL 0)
            file(REMOVE_RECURSE "${HEADROOM_DFN_SOURCE_DIR}")
            message(FATAL_ERROR "DeepFilterNet: could not fetch ${HEADROOM_DFN_REPO} at ${HEADROOM_DFN_TAG}:\n${dfn_error}")
        endif()
    endif()

    # The tag must still point at the commit this build was checked against.
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${HEADROOM_DFN_SOURCE_DIR}" rev-parse HEAD
                    OUTPUT_VARIABLE dfn_head OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT dfn_head STREQUAL HEADROOM_DFN_COMMIT)
        message(FATAL_ERROR "DeepFilterNet ${HEADROOM_DFN_TAG} is at ${dfn_head}, expected ${HEADROOM_DFN_COMMIT}")
    endif()
endif()

if(NOT EXISTS "${HEADROOM_DFN_SOURCE_DIR}/libDF/Cargo.toml")
    message(FATAL_ERROR "DeepFilterNet: ${HEADROOM_DFN_SOURCE_DIR}/libDF/Cargo.toml not found")
endif()

# ---- Models -> <build>/models/ ---------------------------------------------------------------------------------
set(HEADROOM_DFN_MODEL_DIR "${CMAKE_BINARY_DIR}/models")
file(MAKE_DIRECTORY "${HEADROOM_DFN_MODEL_DIR}")
set(HEADROOM_DFN_MODEL_FILES "")

list(LENGTH HEADROOM_DFN_MODELS dfn_model_count)
math(EXPR dfn_model_last "${dfn_model_count} - 1")

foreach(dfn_index RANGE 0 ${dfn_model_last} 2)
    math(EXPR dfn_hash_index "${dfn_index} + 1")
    list(GET HEADROOM_DFN_MODELS ${dfn_index} dfn_name)
    list(GET HEADROOM_DFN_MODELS ${dfn_hash_index} dfn_hash)
    set(dfn_dest "${HEADROOM_DFN_MODEL_DIR}/${dfn_name}")
    set(dfn_ok FALSE)

    if(EXISTS "${dfn_dest}")
        file(SHA256 "${dfn_dest}" dfn_actual)
        if(dfn_actual STREQUAL dfn_hash)
            set(dfn_ok TRUE)
        endif()
    endif()

    if(NOT dfn_ok AND FETCHCONTENT_SOURCE_DIR_DEEPFILTERNET AND EXISTS "${HEADROOM_DFN_SOURCE_DIR}/models/${dfn_name}")
        file(SHA256 "${HEADROOM_DFN_SOURCE_DIR}/models/${dfn_name}" dfn_actual)
        if(dfn_actual STREQUAL dfn_hash)
            file(COPY "${HEADROOM_DFN_SOURCE_DIR}/models/${dfn_name}" DESTINATION "${HEADROOM_DFN_MODEL_DIR}")
            set(dfn_ok TRUE)
        endif()
    endif()

    if(NOT dfn_ok)
        message(STATUS "DeepFilterNet: downloading ${dfn_name}")
        file(DOWNLOAD "https://raw.githubusercontent.com/Rikorose/DeepFilterNet/${HEADROOM_DFN_COMMIT}/models/${dfn_name}"
             "${dfn_dest}" EXPECTED_HASH SHA256=${dfn_hash} SHOW_PROGRESS STATUS dfn_status)
        list(GET dfn_status 0 dfn_code)
        if(NOT dfn_code EQUAL 0)
            file(REMOVE "${dfn_dest}")
            message(FATAL_ERROR "DeepFilterNet: could not download ${dfn_name}: ${dfn_status}")
        endif()
    endif()

    list(APPEND HEADROOM_DFN_MODEL_FILES "${dfn_dest}")
endforeach()

# The licences travel with the models (MIT OR Apache-2.0: we ship the network files and link the code).
foreach(dfn_licence MIT APACHE)
    if(EXISTS "${HEADROOM_DFN_SOURCE_DIR}/LICENSE-${dfn_licence}")
        file(COPY_FILE "${HEADROOM_DFN_SOURCE_DIR}/LICENSE-${dfn_licence}"
             "${HEADROOM_DFN_MODEL_DIR}/LICENSE-DeepFilterNet-${dfn_licence}.txt" ONLY_IF_DIFFERENT)
    endif()
endforeach()

# ---- cargo: dfn-shim + libDF -> one static library -------------------------------------------------------------
set(HEADROOM_DFN_SHIM_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/dfn-shim")
set(HEADROOM_DFN_LIBDF_DIR "${HEADROOM_DFN_SOURCE_DIR}/libDF")
set(HEADROOM_DFN_CARGO_DIR "${CMAKE_BINARY_DIR}/dfn-cargo")
set(HEADROOM_DFN_TARGET_DIR "${HEADROOM_DFN_CARGO_DIR}/target")
file(MAKE_DIRECTORY "${HEADROOM_DFN_CARGO_DIR}")

configure_file("${HEADROOM_DFN_SHIM_DIR}/Cargo.toml.in" "${HEADROOM_DFN_CARGO_DIR}/Cargo.toml" @ONLY)
configure_file("${HEADROOM_DFN_SHIM_DIR}/Cargo.lock" "${HEADROOM_DFN_CARGO_DIR}/Cargo.lock" COPYONLY)

if(MSVC)
    set(HEADROOM_DFN_LIBRARY "${HEADROOM_DFN_TARGET_DIR}/release/headroom_dfn.lib")
else()
    set(HEADROOM_DFN_LIBRARY "${HEADROOM_DFN_TARGET_DIR}/release/libheadroom_dfn.a")
endif()

# Always asks cargo (it is a no-op when nothing changed); the library file is only newer when something was rebuilt,
# which is what decides whether the executables relink. --locked: the checked-in Cargo.lock is what gets built.
file(GLOB HEADROOM_DFN_SHIM_SOURCES CONFIGURE_DEPENDS "${HEADROOM_DFN_SHIM_DIR}/src/*.rs")
add_custom_target(headroom_dfn_cargo
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${HEADROOM_DFN_TARGET_DIR}" CARGO_TERM_COLOR=never
            "${HEADROOM_CARGO}" build --release --locked --lib
            --manifest-path "${HEADROOM_DFN_CARGO_DIR}/Cargo.toml"
    BYPRODUCTS "${HEADROOM_DFN_LIBRARY}"
    SOURCES ${HEADROOM_DFN_SHIM_SOURCES} "${HEADROOM_DFN_SHIM_DIR}/Cargo.toml.in" "${HEADROOM_DFN_SHIM_DIR}/Cargo.lock"
    COMMENT "DeepFilterNet: cargo build (first build takes a few minutes)"
    VERBATIM)

add_library(headroom_dfn_static STATIC IMPORTED GLOBAL)
set_target_properties(headroom_dfn_static PROPERTIES IMPORTED_LOCATION "${HEADROOM_DFN_LIBRARY}")
add_dependencies(headroom_dfn_static headroom_dfn_cargo)

# What a Rust static library needs from the platform (rustc --print native-static-libs).
if(WIN32)
    target_link_libraries(headroom_dfn INTERFACE headroom_dfn_static bcrypt advapi32 kernel32 ntdll userenv ws2_32 dbghelp)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(headroom_dfn INTERFACE headroom_dfn_static Threads::Threads ${CMAKE_DL_LIBS} m gcc_s)
endif()

target_include_directories(headroom_dfn INTERFACE "${HEADROOM_DFN_SHIM_DIR}/include")
add_dependencies(headroom_dfn headroom_dfn_cargo)
set(HEADROOM_DFN_ENABLED ON)
message(STATUS "DeepFilterNet: ${HEADROOM_DFN_TAG} from ${HEADROOM_DFN_SOURCE_DIR}, models in ${HEADROOM_DFN_MODEL_DIR}")
