cmake_minimum_required(VERSION 3.16)
project(fanotify_monitor_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(autostart_monitor
    src/event_queue/event_queue.cpp
    src/sources/threaded_source.cpp

        src/sources/software_source.cpp
        src/sources/monitors/software_monitor/software_monitor.cpp
    src/examples/autostart_monitor_main.cpp
)

target_include_directories(autostart_monitor PRIVATE include)
target_compile_options(autostart_monitor PRIVATE -Wall -Wextra -Wpedantic)
