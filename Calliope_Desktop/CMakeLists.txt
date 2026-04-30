cmake_minimum_required(VERSION 4.2)
project(Calliope_Desktop)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(
    Calliope_Desktop
    WIN32
    main.cpp
    audio_capture.cpp
    transfer.cpp)

if (WIN32)
    set(CALLIOPE_LOGO_SOURCE "${CMAKE_SOURCE_DIR}/../Calliope_Logo.png")
    add_custom_command(
        TARGET Calliope_Desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CALLIOPE_LOGO_SOURCE}"
            "$<TARGET_FILE_DIR:Calliope_Desktop>/Calliope_Logo.png")

    target_link_libraries(Calliope_Desktop PRIVATE ole32 uuid ws2_32 shell32 gdiplus)
endif()
