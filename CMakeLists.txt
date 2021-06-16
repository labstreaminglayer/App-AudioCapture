cmake_minimum_required(VERSION 3.12)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum MacOS deployment version")

project(AudioCapture
    DESCRIPTION "Capture audio and stream it over LabStreamingLayer"
    HOMEPAGE_URL "https://github.com/labstreaminglayer/App-AudioCapture/"
    LANGUAGES CXX C
    VERSION 0.1)

# Needed for customized MacOSXBundleInfo.plist.in
SET(CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake" ${CMAKE_MODULE_PATH})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


## Qt
set(CMAKE_AUTOMOC ON)  # The later version of this in LSLCMake is somehow not enough.
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
find_package(Qt6 COMPONENTS Core Widgets Network DBus Multimedia)  # Multimedia requires Qt6.2
if(NOT Qt6_FOUND)
	if(APPLE)
		list(APPEND CMAKE_PREFIX_PATH "/usr/local/opt/qt@5")
	endif()
	# If we require 5.15 then we can use version-agnostic linking, but 5.15 not easily available on Ubuntu.
	find_package(Qt5 COMPONENTS Core Widgets Network DBus Multimedia REQUIRED)
	add_executable(${PROJECT_NAME} MACOSX_BUNDLE)
	set(LSLAPP_QT_VER Qt5)
else()
	qt_add_executable(${PROJECT_NAME} MACOSX_BUNDLE MANUAL_FINALIZATION)
	set(LSLAPP_QT_VER Qt)
endif()

# LSL
find_package(LSL REQUIRED
    HINTS ${LSL_INSTALL_ROOT}
    "${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/build/"
    "${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/build/install"
    "${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/out/build/x64-Release"
    "${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/out/install/x64-Release"
    PATH_SUFFIXES share/LSL)
get_filename_component(LSL_PATH ${LSL_CONFIG} DIRECTORY)

find_package(Threads REQUIRED)

target_sources(${PROJECT_NAME} PRIVATE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    reader.h
    reader.cpp
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
    ${LSLAPP_QT_VER}::Widgets
    ${LSLAPP_QT_VER}::Multimedia
    Threads::Threads
    LSL::lsl
)

set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
set_target_properties(${PROJECT_NAME} PROPERTIES
	XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
)

# Copy the required dll's into the build folder --> useful for debugging from IDE
# create a list of files to copy
set(THIRD_PARTY_DLLS
   LSL::lsl
   ${LSLAPP_QT_VER}::Core
   ${LSLAPP_QT_VER}::Gui
   ${LSLAPP_QT_VER}::Widgets
   ${LSLAPP_QT_VER}::Multimedia
   ${LSLAPP_QT_VER}::Network
)

foreach(_lib ${THIRD_PARTY_DLLS})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different  
            $<TARGET_FILE:${_lib}>
            $<TARGET_FILE_DIR:${PROJECT_NAME}>)
endforeach()

if(Qt6_FOUND)
	qt_finalize_executable(${PROJECT_NAME})
endif()

installLSLApp(${PROJECT_NAME})
LSLGenerateCPackConfig()
