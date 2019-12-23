cmake_minimum_required(VERSION 3.5)

project(AudioCaptureWin
    LANGUAGES CXX
    VERSION 0.1)
include(Findliblsl.cmake)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt5 REQUIRED COMPONENTS Widgets Multimedia)

add_executable(${PROJECT_NAME} MACOSX_BUNDLE WIN32
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    reader.h
    reader.cpp
)
target_link_libraries(${PROJECT_NAME}
    PRIVATE
    Qt5::Widgets
    Qt5::Multimedia
    LSL::lsl
)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 14)
# target_compile_features(${PROJECT_NAME} PRIVATE cxx_auto_type cxx_lambda_init_captures)


set_target_properties(${PROJECT_NAME} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/MacOSXBundleInfo.plist"
    MACOSX_BUNDLE_GUI_IDENTIFIER "LabStreamingLayer.AudioCapture")

# Copy the required dll's into the build folder --> useful for debugging from IDE
# create a list of files to copy
set(THIRD_PARTY_DLLS
   LSL::lsl
   Qt5::Core
   Qt5::Gui
   Qt5::Widgets
   Qt5::Multimedia
   Qt5::Network
)
foreach(_lib ${THIRD_PARTY_DLLS})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different  
            $<TARGET_FILE:${_lib}>
            $<TARGET_FILE_DIR:${PROJECT_NAME}>)
endforeach()

installLSLApp(${PROJECT_NAME})
LSLGenerateCPackConfig()
