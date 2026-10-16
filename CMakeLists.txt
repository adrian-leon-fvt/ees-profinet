cmake_minimum_required(VERSION 3.16)
project(profinet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PROFINET_POSIX "Build Linux/POSIX I/O (Stack, RpcEndpoint); OFF = portable codecs + Dispatcher only" ON)

add_library(profinet src/dispatcher.cpp src/frame.cpp src/lldp.cpp src/rpc.cpp)
if(PROFINET_POSIX)
    target_sources(profinet PRIVATE src/stack.cpp src/rpc_endpoint.cpp)
endif()
add_library(profinet::profinet ALIAS profinet)
target_include_directories(profinet PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(profinet PUBLIC cxx_std_17)
target_compile_options(profinet PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)

include(GNUInstallDirs)
install(TARGETS profinet EXPORT profinetTargets)
if(PROFINET_POSIX)
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
else()
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN stack.h EXCLUDE)
endif()
install(EXPORT profinetTargets NAMESPACE profinet::
    FILE profinetConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/profinet)

option(PROFINET_BUILD_EXAMPLES "Build example programs" ${PROJECT_IS_TOP_LEVEL})
option(PROFINET_BUILD_TESTS "Build unit tests and loopback test app" ${PROJECT_IS_TOP_LEVEL})

if(PROFINET_BUILD_EXAMPLES AND PROFINET_POSIX)
    add_executable(pn_sniff examples/pn_sniff.cpp)
    target_link_libraries(pn_sniff PRIVATE profinet)
endif()

if(PROFINET_BUILD_TESTS)
    enable_testing()

    find_package(GTest REQUIRED)
    add_executable(profinet_test test/unit/test_dispatcher.cpp test/unit/test_frame.cpp test/unit/test_lldp.cpp test/unit/test_rpc.cpp)
    target_link_libraries(profinet_test PRIVATE profinet GTest::gtest_main)

    if(PROFINET_POSIX)
        target_sources(profinet_test PRIVATE test/unit/test_rpc_endpoint.cpp test/unit/test_stack.cpp)

        add_executable(profinet_loopback test/integration/profinet_loopback.cpp)
        target_link_libraries(profinet_loopback PRIVATE profinet)
        add_test(NAME profinet_loopback
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test/integration/loopback_netns.sh
                    $<TARGET_FILE:profinet_loopback>)
        set_tests_properties(profinet_loopback PROPERTIES
            LABELS integration SKIP_RETURN_CODE 77 TIMEOUT 10)
    endif()

    include(GoogleTest)
    gtest_discover_tests(profinet_test PROPERTIES LABELS unit TIMEOUT 5)
endif()

option(PROFINET_BUILD_DOCS "Build Doxygen API docs (target: docs)" ${PROJECT_IS_TOP_LEVEL})
if(PROFINET_BUILD_DOCS)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        set(DOXYGEN_PROJECT_NAME "ees-profinet")
        set(DOXYGEN_USE_MDFILE_AS_MAINPAGE ${CMAKE_CURRENT_SOURCE_DIR}/docs/README.md)
        set(DOXYGEN_EXTRACT_ALL NO)
        set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
        set(DOXYGEN_WARN_NO_PARAMDOC NO)
        set(DOXYGEN_WARN_AS_ERROR YES)
        set(DOXYGEN_EXCLUDE_SYMBOLS "detail")
        set(DOXYGEN_GENERATE_TREEVIEW YES)
        doxygen_add_docs(docs
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/docs
            COMMENT "Generating API docs")
    endif()
endif()
