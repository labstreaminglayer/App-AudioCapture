# =============================================================================
# AudioCapture - CMakeLists.txt
# =============================================================================
# Capture audio and stream it over Lab Streaming Layer.
# =============================================================================

cmake_minimum_required(VERSION 3.28)

# CMP0177: install() DESTINATION paths are normalized (CMake 3.31+)
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

project(AudioCapture
    VERSION 0.2.0
    DESCRIPTION "Capture audio and stream it over LabStreamingLayer"
    HOMEPAGE_URL "https://github.com/labstreaminglayer/App-AudioCapture/"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# liblsl Dependency
# =============================================================================
# By default, liblsl is fetched automatically from GitHub.
# To use a pre-installed liblsl, set LSL_INSTALL_ROOT.
set(LSL_INSTALL_ROOT "" CACHE PATH "Path to installed liblsl (optional)")
set(LSL_FETCH_REF "v1.18.0.b6" CACHE STRING "liblsl version to fetch from GitHub")

if(LSL_INSTALL_ROOT)
    # Use pre-installed liblsl
    find_package(LSL REQUIRED
        HINTS "${LSL_INSTALL_ROOT}"
        PATH_SUFFIXES share/LSL lib/cmake/LSL Frameworks/lsl.framework/Resources/CMake
    )
    message(STATUS "Using installed liblsl: ${LSL_DIR}")
    include("${LSL_DIR}/LSLCMake.cmake")
else()
    # Fetch liblsl from GitHub
    message(STATUS "Fetching liblsl ${LSL_FETCH_REF} from GitHub...")
    include(FetchContent)
    set(LSL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(liblsl
        GIT_REPOSITORY https://github.com/sccn/liblsl.git
        GIT_TAG ${LSL_FETCH_REF}
        GIT_SHALLOW ON
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(liblsl)
    if(NOT TARGET LSL::lsl)
        add_library(LSL::lsl ALIAS lsl)
    endif()
    include("${liblsl_SOURCE_DIR}/cmake/LSLCMake.cmake")
endif()

# =============================================================================
# Qt6
# =============================================================================
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Multimedia)

# macOS: Work around deprecated AGL framework issue in some Qt6 builds
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_wrap_gl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if(_wrap_gl_libs)
        list(FILTER _wrap_gl_libs EXCLUDE REGEX ".*AGL.*")
        set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${_wrap_gl_libs}")
    endif()
endif()

# =============================================================================
# Common Dependencies
# =============================================================================
find_package(Threads REQUIRED)

# =============================================================================
# RPATH Configuration (must be set before targets are created)
# =============================================================================
LSL_configure_rpath()

# =============================================================================
# Application Target
# =============================================================================
qt_add_executable(${PROJECT_NAME} MACOSX_BUNDLE)

target_sources(${PROJECT_NAME} PRIVATE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    reader.h
    reader.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt::Widgets
    Qt::Multimedia
    Threads::Threads
    LSL::lsl
)

# macOS bundle configuration
if(APPLE)
    set(MACOSX_BUNDLE_GUI_IDENTIFIER "org.labstreaminglayer.${PROJECT_NAME}")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MacOSXBundleInfo.plist.in"
    )
endif()

# =============================================================================
# Windows: Copy DLLs to build directory for debugging
# =============================================================================
if(WIN32)
    get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    execute_process(
        COMMAND ${_qmake_executable} -query QT_INSTALL_PLUGINS
        OUTPUT_VARIABLE QT_PLUGINS_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Copy runtime DLLs
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMAND_EXPAND_LISTS
        COMMENT "Copying runtime DLLs for ${PROJECT_NAME}"
    )

    # Copy Qt plugins
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${QT_PLUGINS_DIR}/platforms"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/platforms"
    )
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${QT_PLUGINS_DIR}/styles"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/styles"
    )
endif()

# =============================================================================
# Installation
# =============================================================================
include(GNUInstallDirs)

# Platform-specific install directories
if(WIN32)
    set(INSTALL_BINDIR ".")
    set(INSTALL_LIBDIR ".")
elseif(APPLE)
    set(INSTALL_BINDIR ".")
    set(INSTALL_LIBDIR ".")
else()
    set(INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
    set(INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
endif()

install(TARGETS ${PROJECT_NAME}
    BUNDLE DESTINATION "${INSTALL_BINDIR}"
    RUNTIME DESTINATION "${INSTALL_BINDIR}"
)

# =============================================================================
# Bundle liblsl with the application
# =============================================================================
LSL_install_liblsl(
    DESTINATION "${INSTALL_LIBDIR}"
    FRAMEWORK_DESTINATION "${INSTALL_BINDIR}/${PROJECT_NAME}.app/Contents/Frameworks"
)

# =============================================================================
# Qt Deployment
# =============================================================================
LSL_deploy_qt(TARGET "${PROJECT_NAME}" DESTINATION "${INSTALL_BINDIR}")

# =============================================================================
# MinGW Runtime Deployment
# =============================================================================
LSL_install_mingw_runtime(DESTINATION "${INSTALL_BINDIR}")

# =============================================================================
# macOS Code Signing
# =============================================================================
LSL_codesign(
    TARGET "${PROJECT_NAME}"
    DESTINATION "${INSTALL_BINDIR}"
    ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/app.entitlements"
    BUNDLE
)

# =============================================================================
# CPack Configuration
# =============================================================================
LSL_get_target_arch()
LSL_get_os_name()

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Labstreaminglayer")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}-${LSL_OS}_${LSL_ARCH}")
set(CPACK_STRIP_FILES ON)

if(WIN32)
    set(CPACK_GENERATOR ZIP)
elseif(APPLE)
    set(CPACK_GENERATOR TGZ)
else()
    set(CPACK_GENERATOR DEB TGZ)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "LabStreamingLayer Developers")
    set(CPACK_DEBIAN_PACKAGE_SECTION "science")
    set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.deb")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6widgets6, libqt6multimedia6")
endif()

include(CPack)
