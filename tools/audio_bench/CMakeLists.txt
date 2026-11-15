# Standalone build of the audio library + offline benchmark (no GL, no window).
#   cmake -S tools/audio_bench -B build-audio -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-audio -j && ./build-audio/audio_bench --out docs/_measurements/audio.md
cmake_minimum_required(VERSION 3.16)
project(gfx_audio_bench CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AUDIO_DIR ${CMAKE_CURRENT_LIST_DIR}/../../src/modules/audio)
add_library(gfx_audio STATIC
    ${AUDIO_DIR}/audio_settings.cpp
    ${AUDIO_DIR}/audio_dsp.cpp
    ${AUDIO_DIR}/audio_mixer.cpp
    ${AUDIO_DIR}/audio_stream.cpp
    ${AUDIO_DIR}/audio_synth.cpp
    ${AUDIO_DIR}/audio_system.cpp
    ${AUDIO_DIR}/audio_demo.cpp
    ${AUDIO_DIR}/miniaudio_impl.cpp)
target_include_directories(gfx_audio PUBLIC ${AUDIO_DIR})
find_package(Threads REQUIRED)
target_link_libraries(gfx_audio PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(NOT MSVC)
    target_link_libraries(gfx_audio PUBLIC m)
    target_compile_options(gfx_audio PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(audio_bench audio_bench.cpp)
target_link_libraries(audio_bench PRIVATE gfx_audio)
