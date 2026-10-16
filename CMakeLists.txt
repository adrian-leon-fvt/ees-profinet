cmake_minimum_required(VERSION 3.16)
project(profinet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(profinet src/frame.cpp src/lldp.cpp src/rpc.cpp src/stack.cpp)
add_library(profinet::profinet ALIAS profinet)
target_include_directories(profinet PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(profinet PUBLIC cxx_std_20)
target_compile_options(profinet PRIVATE -Wall -Wextra)

include(GNUInstallDirs)
install(TARGETS profinet EXPORT profinetTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT profinetTargets NAMESPACE profinet::
    FILE profinetConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/profinet)

option(PROFINET_BUILD_EXAMPLES "Build example programs" ${PROJECT_IS_TOP_LEVEL})
option(PROFINET_BUILD_TESTS "Build unit tests and loopback test app" ${PROJECT_IS_TOP_LEVEL})

if(PROFINET_BUILD_EXAMPLES)
    add_executable(pn_sniff examples/pn_sniff.cpp)
    target_link_libraries(pn_sniff PRIVATE profinet)
endif()

if(PROFINET_BUILD_TESTS)
    enable_testing()

    add_executable(profinet_loopback test/integration/profinet_loopback.cpp)
    target_link_libraries(profinet_loopback PRIVATE profinet)
    add_test(NAME profinet_loopback
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test/integration/loopback_netns.sh
                $<TARGET_FILE:profinet_loopback>)
    set_tests_properties(profinet_loopback PROPERTIES
        LABELS integration SKIP_RETURN_CODE 77 TIMEOUT 10)

    find_package(GTest REQUIRED)
    add_executable(profinet_test test/unit/test_frame.cpp test/unit/test_lldp.cpp test/unit/test_rpc.cpp test/unit/test_stack.cpp)
    target_link_libraries(profinet_test PRIVATE profinet GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(profinet_test PROPERTIES LABELS unit TIMEOUT 5)
endif()
