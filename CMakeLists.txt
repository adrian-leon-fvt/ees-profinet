cmake_minimum_required(VERSION 3.16)
project(profinet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(profinet src/frame.cpp src/stack.cpp)
add_library(profinet::profinet ALIAS profinet)
target_include_directories(profinet PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_options(profinet PRIVATE -Wall -Wextra)

include(GNUInstallDirs)
install(TARGETS profinet EXPORT profinetTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT profinetTargets NAMESPACE profinet::
    FILE profinetConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/profinet)

if(PROJECT_IS_TOP_LEVEL)
    add_executable(pn_sniff examples/pn_sniff.cpp)
    target_link_libraries(pn_sniff PRIVATE profinet)

    find_package(GTest)
    if(GTest_FOUND)
        enable_testing()
        add_executable(profinet_test test/test_frame.cpp)
        target_link_libraries(profinet_test PRIVATE profinet GTest::gtest_main)
        add_test(NAME profinet_test COMMAND profinet_test)
    endif()
endif()
