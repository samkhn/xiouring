cmake_minimum_required(VERSION 3.5)

project(xiouring LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# NOTE: anything that depends on liburing needs threads
find_package(Threads REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)

find_library(IOURING_LIB liburing.so
    PATHS /usr/lib /usr/local/lib)

find_path(IOURING_HEADER liburing.h
    PATHS /usr/include /usr/local/include)

if(NOT IOURING_LIB OR NOT IOURING_HEADER)
    message(FATAL_ERROR "Cannot find liburing in system headers. Specify -DIOURING_HEADER and -DIOURING_LIB to point to header and library respectively")
else()
    message("iouring header at ${IOURING_HEADER} library at ${IOURING_LIB}")
endif()

add_executable(iouringcat src/cat.cpp)
target_link_libraries(iouringcat ${IOURING_LIB})
target_include_directories(iouringcat PUBLIC ${IOURING_HEADER} Threads::Threads)
