include(FetchContent)

# ---------------------------------------------------------------------------
# miniaudio — single-header, cross-platform audio I/O
# ---------------------------------------------------------------------------
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        master
)
cmake_policy(SET CMP0169 OLD)  # allow FetchContent_Populate for header-only libs
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
endif()

# ---------------------------------------------------------------------------
# cJSON — single-file JSON parser
# ---------------------------------------------------------------------------
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        master
)
FetchContent_GetProperties(cjson)
if(NOT cjson_POPULATED)
    FetchContent_Populate(cjson)
endif()
# cjson_SOURCE_DIR is used below after add_executable

# ---------------------------------------------------------------------------
# whisper.cpp — on-the-fly transcription (deferred, uncomment when ready)
# ---------------------------------------------------------------------------
# FetchContent_Declare(
#     whisper_cpp
#     GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
#     GIT_TAG        master
# )
# set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
# set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# FetchContent_MakeAvailable(whisper_cpp)

# ---------------------------------------------------------------------------
# ASIO — optional, requires Steinberg ASIO SDK (free download from steinberg.net)
# Set -DASIO_SDK_DIR=<path> on the cmake command line, or place the SDK at
# C:/ASIOSDK2.3 (the default check location).
# ---------------------------------------------------------------------------
set(ASIO_SDK_DIR "" CACHE PATH "Path to Steinberg ASIO SDK root (contains common/asio.h)")

# Try the user-supplied path first, then a common default location.
if(ASIO_SDK_DIR AND EXISTS "${ASIO_SDK_DIR}/common/asio.h")
    set(_ASIO_FOUND TRUE)
    set(_ASIO_INCLUDE "${ASIO_SDK_DIR}/common")
elseif(EXISTS "C:/Users/Mark/dev/ASIOSDK2.3/ASIOSDK/common/asio.h")
    set(_ASIO_FOUND TRUE)
    set(_ASIO_INCLUDE "C:/Users/Mark/dev/ASIOSDK2.3/ASIOSDK/common")
else()
    set(_ASIO_FOUND FALSE)
endif()

# ---------------------------------------------------------------------------
# Engine sources
# ---------------------------------------------------------------------------
add_executable(wavrec_engine
    src/main.c
    src/engine.c

    src/util/crc32.c

    src/audio/miniaudio_impl.c
    src/audio/audio_io.c
    src/audio/record_engine.c
    src/audio/playback_engine.c
    src/audio/mixer.c

    src/ipc/ipc.c
    src/ipc/ipc_shm.c

    src/timecode/timecode.c

    src/track/track.c

    src/meter/metering.c
    src/waveform/waveform.c
    src/disk/disk_writer.c

    src/metadata/bwf.c
    src/metadata/ixml.c
    src/metadata/wav_reader.c

    src/transcription/transcription.c
)

target_sources(wavrec_engine PRIVATE ${cjson_SOURCE_DIR}/cJSON.c)

target_include_directories(wavrec_engine PRIVATE
    src
    ${miniaudio_SOURCE_DIR}
    ${cjson_SOURCE_DIR}
)

if(_ASIO_FOUND)
    message(STATUS "ASIO SDK found at ${_ASIO_INCLUDE} — ASIO custom backend enabled")
    target_sources(wavrec_engine PRIVATE src/audio/miniaudio_asio.cpp)
    target_include_directories(wavrec_engine PRIVATE
        ${_ASIO_INCLUDE}
        "C:/Users/Mark/dev/ASIOSDK2.3/ASIOSDK/host"
    )
    target_compile_definitions(wavrec_engine PRIVATE WAVREC_HAVE_ASIO)
    target_link_libraries(wavrec_engine PRIVATE ole32)  # CoCreateInstance / COM
else()
    message(STATUS "ASIO SDK not found — ASIO disabled (set ASIO_SDK_DIR to enable)")
endif()

# Platform-specific sources and libraries
if(WIN32)
    target_sources(wavrec_engine PRIVATE src/platform/platform_win32.c)
    target_link_libraries(wavrec_engine PRIVATE ws2_32)   # sockets
    target_compile_definitions(wavrec_engine PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    if(MSVC)
        # Inject stdatomic.h shim before system headers (VS2019 has no C11 atomics)
        target_include_directories(wavrec_engine BEFORE PRIVATE src/compat/msvc)
        # strdup → _strdup; treat as warning not error
        target_compile_options(wavrec_engine PRIVATE /wd4996 /wd4127)
    endif()
else()
    target_sources(wavrec_engine PRIVATE src/platform/platform_posix.c)
    target_link_libraries(wavrec_engine PRIVATE pthread m)
    if(APPLE)
        target_link_libraries(wavrec_engine PRIVATE
            "-framework CoreAudio"
            "-framework AudioToolbox"
        )
    endif()
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(wavrec_engine PRIVATE /W4)
else()
    target_compile_options(wavrec_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Whisper linkage (deferred)
# if(whisper_cpp_POPULATED)
#     target_link_libraries(wavrec_engine PRIVATE whisper)
#     target_compile_definitions(wavrec_engine PRIVATE WAVREC_TRANSCRIPTION_ENABLED)
# endif()
