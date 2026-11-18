cmake_minimum_required(VERSION 3.18)
project(zodiacdroid_native C)

# The emulator core lives at the repository root.
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../..)
file(GLOB CORE_SRCS ${ROOT}/core/*.c)

add_library(zodiacdroid SHARED jni_bridge.c ${CORE_SRCS})
target_include_directories(zodiacdroid PRIVATE ${ROOT}/core)
target_compile_options(zodiacdroid PRIVATE -O3 -Wno-unused -fno-strict-aliasing)
target_link_libraries(zodiacdroid PRIVATE android log m z aaudio)
