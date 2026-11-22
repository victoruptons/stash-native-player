cmake_minimum_required(VERSION 3.21)

project(StashNativePlayer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Network Qml Quick OpenGL WebEngineQuick WebChannel)

# --- libmpv headers ------------------------------------------------------------
# libmpv itself is loaded at runtime (see src/mpvlib.cpp); only the headers are
# needed to build. Point MPV_INCLUDE_DIR at a folder containing mpv/client.h, or
# let CMake download the headers of a known mpv release.
set(MPV_HEADERS_VERSION "v0.39.0" CACHE STRING "mpv release to fetch headers from")
find_path(MPV_INCLUDE_DIR mpv/client.h)
if(NOT MPV_INCLUDE_DIR)
    set(_mpv_hdr_dir "${CMAKE_BINARY_DIR}/mpv-headers")
    foreach(_h client.h render.h render_gl.h)
        if(NOT EXISTS "${_mpv_hdr_dir}/mpv/${_h}")
            message(STATUS "Downloading mpv header ${_h} (${MPV_HEADERS_VERSION})")
            file(DOWNLOAD
                "https://raw.githubusercontent.com/mpv-player/mpv/${MPV_HEADERS_VERSION}/libmpv/${_h}"
                "${_mpv_hdr_dir}/mpv/${_h}"
                STATUS _st TLS_VERIFY ON)
            list(GET _st 0 _code)
            if(NOT _code EQUAL 0)
                message(FATAL_ERROR "Could not download mpv/${_h}: ${_st}. Set MPV_INCLUDE_DIR instead.")
            endif()
        endif()
    endforeach()
    set(MPV_INCLUDE_DIR "${_mpv_hdr_dir}" CACHE PATH "" FORCE)
endif()
message(STATUS "Using mpv headers from ${MPV_INCLUDE_DIR}")

qt_add_executable(StashNativePlayer WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/mpvlib.cpp src/mpvlib.h
    src/mpvitem.cpp src/mpvitem.h
    src/bridge.cpp src/bridge.h
    src/appcontroller.cpp src/appcontroller.h
    src/winframe.cpp src/winframe.h
    src/livesubs.cpp src/livesubs.h
    src/subtitlegen.cpp
    src/subtitlecore.cpp src/subtitlecore.h
    src/screenocr.cpp src/screenocr.h
    resources.qrc
)
if(APPLE)
    # native title bar with traffic lights over the toolbar (winframe_mac.mm)
    enable_language(OBJCXX)
    target_sources(StashNativePlayer PRIVATE src/winframe_mac.mm)
    target_link_libraries(StashNativePlayer PRIVATE "-framework AppKit")
endif()
if(WIN32)
    target_sources(StashNativePlayer PRIVATE app.rc)
    # Windows OCR (screenocr.cpp, C++/WinRT from the Windows SDK)
    target_link_libraries(StashNativePlayer PRIVATE windowsapp)
    set_source_files_properties(src/screenocr.cpp PROPERTIES COMPILE_OPTIONS "/permissive-;/bigobj")
endif()

target_include_directories(StashNativePlayer PRIVATE ${MPV_INCLUDE_DIR})
target_compile_definitions(StashNativePlayer PRIVATE SNP_VERSION="${PROJECT_VERSION}")

target_link_libraries(StashNativePlayer PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::OpenGL
    Qt6::WebEngineQuick Qt6::WebChannel
)

set_target_properties(StashNativePlayer PROPERTIES
    OUTPUT_NAME "StashNativePlayer"
    MACOSX_BUNDLE_BUNDLE_NAME "Stash Native Player"
    MACOSX_BUNDLE_GUI_IDENTIFIER "app.stashnativeplayer"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)

install(TARGETS StashNativePlayer BUNDLE DESTINATION . RUNTIME DESTINATION .)

# --- unit tests (-DSNP_BUILD_TESTS=ON, then ctest) --------------------------------
option(SNP_BUILD_TESTS "Build the unit tests" OFF)
if(SNP_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(tst_subtitlecore
        tests/tst_subtitlecore.cpp
        src/subtitlecore.cpp src/subtitlecore.h
    )
    set_target_properties(tst_subtitlecore PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)
    target_include_directories(tst_subtitlecore PRIVATE src)
    target_link_libraries(tst_subtitlecore PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME subtitlecore COMMAND tst_subtitlecore)
endif()
