cmake_minimum_required(VERSION 3.24)
# FastChatSapi32: the 32-bit helper that speaks with 32-bit SAPI voices (see
# main.cpp). Its own project because it must be built for 32-bit x86 while
# FastChat is 64-bit; the main CMakeLists.txt builds it with ExternalProject.
project(FastChatSapi32 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "FastChatSapi32 is built for 32-bit Windows with MSVC: 32-bit voices only load into a 32-bit process")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The static C runtime: one file, nothing else to ship.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# A console program with no window: FastChat starts it hidden.
add_executable(FastChatSapi32 main.cpp)
target_compile_options(FastChatSapi32 PRIVATE /utf-8 /W3 /EHsc)
# Windows 7 and later, like FastChat.
target_compile_definitions(FastChatSapi32 PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0601)
target_link_libraries(FastChatSapi32 PRIVATE ole32 sapi)
