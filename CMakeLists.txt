cmake_minimum_required(VERSION 3.24)

# macOS: one universal app for Apple silicon and Intel Macs. Set before project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS FastChat runs on")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architectures to build for")

project(FastChat LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /MP)
    # Global on purpose: every dependency must agree. WIN32_LEAN_AND_MEAN is
    # deliberately not set - it hides shellapi.h, which wxWidgets needs.
    add_compile_definitions(NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    # Statically link the CRT so FastChat.exe needs no redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(FASTCHAT_NO_WARNINGS /w)
else()
    set(FASTCHAT_NO_WARNINGS -w)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Floor for old dependencies")
set(CMAKE_SKIP_INSTALL_RULES ON)
# Every fetched library is linked into FastChat itself.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

option(FASTCHAT_PRISM "Speech through the Prism library (screen readers, SAPI, OneCore, VoiceOver...)" ON)

# ---------------------------------------------------------------------------
# Dependencies fetched by CMake
# ---------------------------------------------------------------------------

# --- wxWidgets ---
# The same 3.3-line master commit FastPlay uses, for its screen reader fixes.
# To reuse a checkout already on disk, configure with
# -DFETCHCONTENT_SOURCE_DIR_WXWIDGETS=<path to wxWidgets source>.
FetchContent_Declare(wxWidgets
    GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
    GIT_TAG 776a9a278791fe0d1c79be337e55ab8e53f89587
)
set(wxBUILD_SHARED OFF CACHE INTERNAL "")
set(wxBUILD_USE_STATIC_RUNTIME ON CACHE INTERNAL "")
set(wxBUILD_TESTS OFF CACHE INTERNAL "")
set(wxBUILD_SAMPLES OFF CACHE INTERNAL "")
set(wxBUILD_DEMOS OFF CACHE INTERNAL "")
if(APPLE)
    set(wxBUILD_PRECOMP OFF CACHE INTERNAL "")
else()
    set(wxBUILD_PRECOMP ON CACHE INTERNAL "")
endif()
set(wxUSE_STL ON CACHE INTERNAL "")
# FastChat links core, base and adv; the rest of wxWidgets is build time for nothing.
set(wxUSE_STC OFF CACHE INTERNAL "")
set(wxUSE_WEBVIEW OFF CACHE INTERNAL "")
set(wxUSE_MEDIACTRL OFF CACHE INTERNAL "")
set(wxUSE_RICHTEXT OFF CACHE INTERNAL "")
set(wxUSE_PROPGRID OFF CACHE INTERNAL "")
set(wxUSE_RIBBON OFF CACHE INTERNAL "")
set(wxUSE_AUI OFF CACHE INTERNAL "")
set(wxUSE_XRC OFF CACHE INTERNAL "")
set(wxUSE_LIBJPEG OFF CACHE INTERNAL "")
set(wxUSE_LIBTIFF OFF CACHE INTERNAL "")
set(wxUSE_LIBWEBP OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(wxWidgets)

# --- nlohmann/json ---
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# --- SQLite (the amalgamation, compiled straight in) ---
FetchContent_Declare(sqlite
    URL https://sqlite.org/2026/sqlite-amalgamation-3510200.zip
)
FetchContent_GetProperties(sqlite)
if(NOT sqlite_POPULATED)
    FetchContent_Populate(sqlite)
endif()
add_library(fc_sqlite STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(fc_sqlite PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(fc_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_ENABLE_FTS5)
target_compile_options(fc_sqlite PRIVATE ${FASTCHAT_NO_WARNINGS})

# --- miniaudio (sound events) ---
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)  # the header only; compiled by src/core/miniaudio_impl.c
endif()

# --- Prism (speech) ---
# https://github.com/ethindp/prism: screen readers, SAPI, OneCore, VoiceOver,
# Speech Dispatcher and more behind one C API. Built static (BUILD_SHARED_LIBS
# above), which makes its target define PRISM_STATIC for us.
if(FASTCHAT_PRISM)
    set(PRISM_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(PRISM_ENABLE_DEMOS OFF CACHE BOOL "" FORCE)
    set(PRISM_ENABLE_GDEXTENSION OFF CACHE BOOL "" FORCE)
    set(PRISM_ENABLE_SHIMS OFF CACHE BOOL "" FORCE)
    set(PRISM_ENABLE_LINTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(prism
        GIT_REPOSITORY https://github.com/ethindp/prism.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(prism)
endif()

option(FASTCHAT_DEPS_ONLY "Configure the fetched dependencies without FastChat itself" OFF)
if(FASTCHAT_DEPS_ONLY)
    return()
endif()

# ---------------------------------------------------------------------------
# FastChat
# ---------------------------------------------------------------------------

# The core: everything but the windows (providers, storage, speech, sounds).
set(FASTCHAT_CORE_SOURCES
    src/core/util.cpp
    src/core/sse.cpp
    src/core/settings.cpp
    src/core/database.cpp
    src/core/conversation.cpp
    src/core/chat_engine.cpp
    src/core/code_tools.cpp
    src/core/computer_tools.cpp
    src/core/events.cpp
    src/core/log.cpp
    src/core/notify.cpp
    src/core/speech.cpp
    src/core/sound.cpp
    src/core/soundpack.cpp
    src/core/miniaudio_impl.c
    src/core/providers/provider.cpp
    src/core/providers/openai_compat.cpp
    src/core/providers/anthropic.cpp
    src/core/providers/gemini.cpp
    src/core/providers/ollama.cpp
    src/core/providers/claude_code.cpp
    src/core/providers/codex.cpp
)
if(WIN32)
    list(APPEND FASTCHAT_CORE_SOURCES
        src/core/platform/http_windows.cpp
        src/core/platform/paths_windows.cpp
        src/core/platform/secrets_windows.cpp
        src/core/platform/process_windows.cpp
        src/core/platform/speech_sapi.cpp
        src/core/platform/child_windows.cpp
        src/core/platform/desktop_windows.cpp
    )
else()
    list(APPEND FASTCHAT_CORE_SOURCES
        src/core/platform/http_curl.cpp
        src/core/platform/paths_posix.cpp
        src/core/platform/secrets_posix.cpp
        src/core/platform/process_posix.cpp
        src/core/platform/child_posix.cpp
    )
endif()
if(APPLE)
    list(APPEND FASTCHAT_CORE_SOURCES src/core/platform/speech_avspeech.mm src/core/platform/desktop_mac.mm)
    set_source_files_properties(src/core/platform/speech_avspeech.mm src/core/platform/desktop_mac.mm
        PROPERTIES COMPILE_OPTIONS -fobjc-arc)
elseif(NOT WIN32)
    list(APPEND FASTCHAT_CORE_SOURCES src/core/platform/desktop_none.cpp)
endif()

add_library(fastchat_core STATIC ${FASTCHAT_CORE_SOURCES})
target_include_directories(fastchat_core PUBLIC src PRIVATE ${miniaudio_SOURCE_DIR})
target_link_libraries(fastchat_core PUBLIC nlohmann_json::nlohmann_json fc_sqlite)
target_compile_definitions(fastchat_core PRIVATE
    MA_NO_ENCODING
    $<$<BOOL:${FASTCHAT_PRISM}>:FASTCHAT_HAVE_PRISM>)
set_source_files_properties(src/core/miniaudio_impl.c PROPERTIES COMPILE_OPTIONS "${FASTCHAT_NO_WARNINGS}")
if(FASTCHAT_PRISM)
    target_link_libraries(fastchat_core PUBLIC prism::prism)
endif()
if(MSVC)
    target_compile_options(fastchat_core PRIVATE /W3)
endif()
if(WIN32)
    target_link_libraries(fastchat_core PUBLIC winhttp crypt32 ole32 oleaut32 uuid sapi shlwapi shell32 user32 advapi32
        gdi32 dwmapi windowscodecs uiautomationcore)
else()
    find_package(CURL REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(fastchat_core PUBLIC CURL::libcurl Threads::Threads ${CMAKE_DL_LIBS})
endif()
if(APPLE)
    target_link_libraries(fastchat_core PUBLIC "-framework Security" "-framework CoreFoundation"
        "-framework CoreAudio" "-framework AudioToolbox" "-framework AVFoundation" "-framework Foundation"
        "-framework AppKit" "-framework ApplicationServices" "-framework ImageIO" "-framework CoreGraphics")
    # ScreenCaptureKit is macOS 14 and later; FastChat still runs on 11.
    target_link_options(fastchat_core PUBLIC "-Wl,-weak_framework,ScreenCaptureKit")
endif()

# The application: the windows.
set(FASTCHAT_SOURCES
    src/app.cpp
    src/ui/ui_util.cpp
    src/ui/main_frame.cpp
    src/ui/conversation_frame.cpp
    src/ui/new_conversation_dialog.cpp
    src/ui/settings_dialog.cpp
    src/ui/provider_dialog.cpp
    src/ui/chat_settings_dialog.cpp
    src/ui/event_settings_panel.cpp
    src/ui/model_picker.cpp
    src/ui/text_view_dialog.cpp
    src/ui/activity_window.cpp
)
if(WIN32)
    list(APPEND FASTCHAT_SOURCES res/fastchat.rc)
endif()

add_executable(FastChat WIN32 MACOSX_BUNDLE ${FASTCHAT_SOURCES})
target_link_libraries(FastChat PRIVATE fastchat_core wx::core wx::base wx::adv)
if(MSVC)
    target_compile_options(FastChat PRIVATE /W3)
    # A map file beside the build, to read crash addresses against.
    target_link_options(FastChat PRIVATE /MAP:${CMAKE_CURRENT_BINARY_DIR}/FastChat.map)
endif()
if(WIN32)
    # FastChat.exe goes to the repository root, beside its data when run portably.
    set(FASTCHAT_OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE PATH "Where FastChat.exe is written")
    set_target_properties(FastChat PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${FASTCHAT_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${FASTCHAT_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${FASTCHAT_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${FASTCHAT_OUTPUT_DIR})
endif()

if(APPLE)
    # The app bundle, with the version from version.h and the readme in
    # Contents/Resources. FastChat.app is written to the repository root.
    file(STRINGS src/version.h FASTCHAT_VERSION_LINE REGEX "#define APP_VERSION ")
    string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" FASTCHAT_VERSION "${FASTCHAT_VERSION_LINE}")
    configure_file(res/mac/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
    set(FASTCHAT_OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE PATH "Where FastChat.app is written")
    set_target_properties(FastChat PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
        RUNTIME_OUTPUT_DIRECTORY ${FASTCHAT_OUTPUT_DIR})
    add_custom_command(TARGET FastChat POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/README.md
            $<TARGET_BUNDLE_CONTENT_DIR:FastChat>/Resources/README.md
        # Ad-hoc signed, so it runs on Apple silicon and keeps its permissions.
        COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:FastChat>)
endif()

# Tests of the core (no windows): build the fastchat_tests target, then run it,
# with tests/mock_server.py running and FASTCHAT_MOCK set to its address for the
# provider tests.
add_executable(fastchat_tests EXCLUDE_FROM_ALL tests/core_tests.cpp)
target_link_libraries(fastchat_tests PRIVATE fastchat_core)
