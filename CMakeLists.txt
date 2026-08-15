cmake_minimum_required(VERSION 3.15)
project(schwung-tablor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# No -ffast-math: it lets the compiler assume no NaN/Inf, which removes the
# safety of clamps on the realtime path (the ER-99 rule).
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# Device runtime is glibc 2.35 / libstdc++ 6.0.29 — link the C++ runtime
# statically so the .so carries no libstdc++ version requirement at all.
set(TB_STATIC_LINK -static-libstdc++ -static-libgcc)

# The Move module: dlopen'd by Schwung's chain host as dsp.so
add_library(tablor SHARED src/dsp/tablor_plugin.cpp)
target_include_directories(tablor PRIVATE src src/dsp src/host)
target_link_libraries(tablor PRIVATE m)
target_link_options(tablor PRIVATE ${TB_STATIC_LINK})
set_target_properties(tablor PROPERTIES OUTPUT_NAME "dsp" PREFIX "" SUFFIX ".so")

# Loader test: dlopens the real dsp.so exactly like the chain host does.
# Cross-compiled and run ON the Move — the phase 0 gate.
add_executable(tablor_loadtest tools/loadtest.cpp)
target_include_directories(tablor_loadtest PRIVATE src src/dsp src/host)
target_link_libraries(tablor_loadtest PRIVATE dl m)
target_link_options(tablor_loadtest PRIVATE ${TB_STATIC_LINK})

# Phase 1 benchmark: mip build validation + realtime factor ON the Move.
# The go/no-go for the voice/unison budget.
add_executable(tablor_bench tools/bench.cpp src/ported/wavetable.cpp)
target_include_directories(tablor_bench PRIVATE src src/ported)
target_link_libraries(tablor_bench PRIVATE m)
target_link_options(tablor_bench PRIVATE ${TB_STATIC_LINK})
