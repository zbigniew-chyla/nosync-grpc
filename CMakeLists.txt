cmake_minimum_required(VERSION 3.12)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NOSYNC_GRPC_MODULES REQUIRED protobuf grpc++)

project(
    nosync_grpc
    VERSION 0.0.1
    LANGUAGES CXX)

add_library(${PROJECT_NAME} INTERFACE)

target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_17)

target_include_directories(
    ${PROJECT_NAME}
    INTERFACE
    $<BUILD_INTERFACE:${${PROJECT_NAME}_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    ${NOSYNC_GRPC_MODULES_INCLUDE_DIRS})

target_link_libraries(
    ${PROJECT_NAME}
    INTERFACE
    ${NOSYNC_GRPC_MODULES_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT})

target_compile_options(
    ${PROJECT_NAME}
    INTERFACE
    ${NOSYNC_GRPC_MODULES_CFLAGS})
