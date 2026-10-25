cmake_minimum_required(VERSION 3.22)

project(PedalBoardToolchainTest LANGUAGES CXX)

# Same static-runtime choice as the real project, so this probe proves the
# no-redistributable configuration too.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(hello hello.cpp)
target_compile_features(hello PRIVATE cxx_std_17)
target_compile_options(hello PRIVATE /utf-8 /W4)
