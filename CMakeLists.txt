cmake_minimum_required(VERSION 3.10)

project(thread_pool_server)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

file(GLOB SOURCES "*.cpp")

add_executable(thread_pool_server.exe ${SOURCES})

target_link_libraries(thread_pool_server.exe PRIVATE Threads::Threads)



