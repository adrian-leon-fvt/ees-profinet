cmake_minimum_required(VERSION 3.16)
project(profinet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PROFINET_POSIX "Build Linux Stack and POSIX RpcEndpoint (Windows uses Winsock regardless)" ON)

add_library(profinet src/dispatcher.cpp src/frame.cpp src/lldp.cpp src/rpc.cpp)
if(PROFINET_POSIX)
    target_sources(profinet PRIVATE src/stack.cpp)
endif()
if(PROFINET_POSIX OR WIN32)
    target_sources(profinet PRIVATE src/rpc_endpoint.cpp)
endif()
if(WIN32)
    target_link_libraries(profinet PRIVATE ws2_32)
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
    add_executable(pn_borrowed_fd examples/pn_borrowed_fd.cpp)
    target_link_libraries(pn_borrowed_fd PRIVATE profinet)
endif()
if(PROFINET_BUILD_EXAMPLES AND (PROFINET_POSIX OR WIN32))
    add_executable(pn_turck_demo examples/pn_turck_demo.cpp)
    target_link_libraries(pn_turck_demo PRIVATE profinet)
endif()
# Uses only portable library parts; its own I/O is platform-specific.
if(PROFINET_BUILD_EXAMPLES AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32))
    add_executable(pn_dispatcher examples/pn_dispatcher.cpp)
    target_link_libraries(pn_dispatcher PRIVATE profinet)
endif()
if(PROFINET_BUILD_EXAMPLES AND WIN32)
    foreach(example pn_dispatcher pn_turck_demo)
        target_compile_definitions(${example} PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN)
    endforeach()
    find_path(PROFINET_NPCAP_INCLUDE_DIR pcap.h
        PATHS "$ENV{NPCAP_SDK}/Include")
    if(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
        set(npcap_lib_dir "Lib/ARM64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(npcap_lib_dir "Lib/x64")
    else()
        set(npcap_lib_dir "Lib")
    endif()
    find_library(PROFINET_NPCAP_LIBRARY wpcap
        PATHS "$ENV{NPCAP_SDK}/${npcap_lib_dir}")
    if(PROFINET_NPCAP_INCLUDE_DIR AND PROFINET_NPCAP_LIBRARY)
        foreach(example pn_dispatcher pn_turck_demo)
            target_include_directories(${example} PRIVATE
                ${PROFINET_NPCAP_INCLUDE_DIR})
            target_compile_definitions(${example} PRIVATE
                PROFINET_NPCAP)
            target_link_libraries(${example} PRIVATE
                ${PROFINET_NPCAP_LIBRARY} iphlpapi ws2_32)
            if(MSVC)
                target_link_options(${example} PRIVATE "/DELAYLOAD:wpcap.dll")
                target_link_libraries(${example} PRIVATE delayimp)
            endif()
        endforeach()
    else()
        message(STATUS
            "Npcap SDK not found; Windows Ethernet discovery is unavailable. "
            "Set NPCAP_SDK to the Npcap SDK directory.")
    endif()
    add_executable(pn_windows_udp examples/pn_windows_udp.cpp)
    target_link_libraries(pn_windows_udp PRIVATE profinet ws2_32)
endif()

if(PROFINET_BUILD_TESTS)
    enable_testing()
    if(TARGET pn_turck_demo)
        add_test(NAME turck_discovery_self_test COMMAND pn_turck_demo --self-test)
        set_tests_properties(turck_discovery_self_test PROPERTIES LABELS unit TIMEOUT 5)
    endif()

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

        set(PROFINET_E2E_PYTHON python3 CACHE STRING "Python with profinet-py + pytest (test/e2e/requirements.txt)")
        if(TARGET pn_turck_demo)
            add_test(NAME turck_status_loopback
                COMMAND ${PROFINET_E2E_PYTHON}
                    ${CMAKE_CURRENT_SOURCE_DIR}/test/integration/turck_status_loopback.py
                    $<TARGET_FILE:pn_turck_demo>)
            set_tests_properties(turck_status_loopback PROPERTIES
                LABELS integration SKIP_RETURN_CODE 77 TIMEOUT 20)
        endif()
        add_executable(profinet_e2e_device test/e2e/e2e_device.cpp)
        target_link_libraries(profinet_e2e_device PRIVATE profinet)
        add_test(NAME profinet_e2e
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e/run_e2e.sh
                    $<TARGET_FILE:profinet_e2e_device> ${PROFINET_E2E_PYTHON} -q)
        set_tests_properties(profinet_e2e PROPERTIES
            LABELS e2e SKIP_RETURN_CODE 77 TIMEOUT 120)
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
