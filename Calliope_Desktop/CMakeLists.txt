cmake_minimum_required(VERSION 4.2)
project(Calliope_Desktop)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(
    Calliope_Desktop
    WIN32
    main.cpp
    audio_capture.cpp
    transfer.cpp
    app.rc)

if (WIN32)
    target_link_libraries(Calliope_Desktop PRIVATE ole32 uuid ws2_32 shell32)
endif()

set(ADB_PLATFORM_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/platform-tools")

add_custom_command(
    TARGET Calliope_Desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Calliope_Desktop>/tools"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ADB_PLATFORM_TOOLS_DIR}"
            "$<TARGET_FILE_DIR:Calliope_Desktop>/tools/platform-tools"
    COMMENT "Copying adb platform-tools bundle next to the executable")
