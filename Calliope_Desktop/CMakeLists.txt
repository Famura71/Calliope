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
