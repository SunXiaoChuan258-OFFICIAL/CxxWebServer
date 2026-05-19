cmake_minimum_required(VERSION 3.10)

project(thread_pool_server)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
#find_library(MYSQL_LIB_PATH mysqlcppconn REQUIRED)
#find_path(MYSQL_INCLUDE_DIR mysql_driver.h PATH_SUFFIXES cppconn)


file(GLOB SOURCES "*.cpp")

add_executable(CxxWebServer_release.exe ${SOURCES})

#target_include_directories(CxxWebServer_release.exe PRIVATE ${MYSQL_INCLUDE_DIR})
target_link_libraries(CxxWebServer_release.exe PRIVATE 
    Threads::Threads 
    #${MYSQL_LIB_PATH}
)





