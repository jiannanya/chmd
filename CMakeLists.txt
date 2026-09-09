cmake_minimum_required(VERSION 3.20)
project(chmd VERSION 1.2.0 LANGUAGES CXX)
include(GNUInstallDirs)

option(CHMD_BUILD_TESTS "Build the test suite" ON)
option(CHMD_BUILD_BENCHMARKS "Build the micro-benchmark" OFF)
option(CHMD_BUILD_FUZZER "Build the libFuzzer target (Clang only)" OFF)
option(CHMD_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if((CHMD_ENABLE_SANITIZERS OR CHMD_BUILD_FUZZER) AND WIN32)
    # LLVM's Windows sanitizer runtimes are built against the static CRT.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()


add_library(chmd
    src/document.cpp
    src/parser.cpp
    src/inline.cpp
    src/render.cpp
    src/entities.cpp
)
add_library(chmd::chmd ALIAS chmd)

target_compile_features(chmd PUBLIC cxx_std_20)
target_include_directories(chmd
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE src
)
set_target_properties(chmd PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

if(MSVC)
    target_compile_options(chmd PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(chmd PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(CHMD_ENABLE_SANITIZERS)
    if(MSVC)
        target_compile_options(chmd PUBLIC /fsanitize=address /Zi)
        target_link_options(chmd PUBLIC /DEBUG)
    elseif(WIN32)
        # LLVM 17's MSVC-ABI ASan runtime cannot intercept the current Windows
        # CRT reliably; UBSan remains fully supported with the dynamic CRT.
        target_compile_options(chmd PUBLIC -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(chmd PUBLIC -fsanitize=undefined)
    else()
        target_compile_options(chmd PUBLIC -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(chmd PUBLIC -fsanitize=address,undefined)
    endif()
endif()

add_executable(chmd_cli app/chmd_cli.cpp)
target_link_libraries(chmd_cli PRIVATE chmd::chmd)
set_target_properties(chmd_cli PROPERTIES
    OUTPUT_NAME chmd
    # Windows sanitizer runtimes can export symbols from the executable.
    # Its import library must not overwrite the actual chmd.lib library.
    ARCHIVE_OUTPUT_NAME chmd_cli
    PDB_NAME chmd_cli
    COMPILE_PDB_NAME chmd_cli
)

install(TARGETS chmd chmd_cli EXPORT chmdTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT chmdTargets NAMESPACE chmd:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chmd)

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/chmdConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/chmdConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chmd
)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/chmdConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/chmdConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/chmdConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chmd)

if(CHMD_BUILD_TESTS)
    enable_testing()
    add_executable(chmd_tests tests/test_main.cpp)
    target_link_libraries(chmd_tests PRIVATE chmd::chmd)
    add_test(NAME chmd_unit COMMAND chmd_tests)
    add_executable(chmd_boundary_tests tests/test_boundaries.cpp)
    target_link_libraries(chmd_boundary_tests PRIVATE chmd::chmd)
    add_test(NAME chmd_boundaries COMMAND chmd_boundary_tests)
    add_executable(chmd_extension_tests tests/test_extensions.cpp)
    target_link_libraries(chmd_extension_tests PRIVATE chmd::chmd)
    add_test(NAME chmd_extensions COMMAND chmd_extension_tests)
    add_executable(chmd_regression_tests tests/test_regressions.cpp)
    target_link_libraries(chmd_regression_tests PRIVATE chmd::chmd)
    add_test(NAME chmd_regressions COMMAND chmd_regression_tests)
    set_tests_properties(chmd_unit chmd_boundaries chmd_extensions chmd_regressions PROPERTIES TIMEOUT 60)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME chmd_cli_options
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py
                    $<TARGET_FILE:chmd_cli>)
        set_tests_properties(chmd_cli_options PROPERTIES TIMEOUT 60)
    endif()
    if(Python3_Interpreter_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/spec.json")
        add_test(NAME chmd_commonmark_0_31_2
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_spec.py
                    --program $<TARGET_FILE:chmd_cli>
                    --program-arg=--commonmark
                    --spec ${CMAKE_CURRENT_SOURCE_DIR}/tests/spec.json)
        set_tests_properties(chmd_commonmark_0_31_2 PROPERTIES TIMEOUT 120)
    endif()
    if(CHMD_ENABLE_SANITIZERS AND MSVC)
        # Some recent Windows CRT builds expose non-interceptable thunks. ASan
        # can safely continue after reporting those startup-only misses.
        set_tests_properties(chmd_unit chmd_boundaries chmd_extensions chmd_regressions PROPERTIES
            ENVIRONMENT "ASAN_WIN_CONTINUE_ON_INTERCEPTION_FAILURE=1")
        if(TEST chmd_cli_options)
            set_tests_properties(chmd_cli_options PROPERTIES
                ENVIRONMENT "ASAN_WIN_CONTINUE_ON_INTERCEPTION_FAILURE=1")
        endif()
        if(TEST chmd_commonmark_0_31_2)
            set_tests_properties(chmd_commonmark_0_31_2 PROPERTIES
                ENVIRONMENT "ASAN_WIN_CONTINUE_ON_INTERCEPTION_FAILURE=1")
        endif()
    endif()
endif()

if(CHMD_BUILD_FUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "CHMD_BUILD_FUZZER requires Clang")
    endif()
    if(WIN32)
        target_compile_options(chmd PRIVATE -fsanitize=fuzzer-no-link,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(chmd INTERFACE -fsanitize=fuzzer-no-link,undefined)
        set(CHMD_FUZZ_SANITIZERS fuzzer,undefined)
    else()
        target_compile_options(chmd PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(chmd INTERFACE -fsanitize=fuzzer-no-link,address,undefined)
        set(CHMD_FUZZ_SANITIZERS fuzzer,address,undefined)
    endif()
    add_executable(chmd_fuzzer tests/fuzz_parser.cpp)
    target_link_libraries(chmd_fuzzer PRIVATE chmd::chmd)
    target_compile_options(chmd_fuzzer PRIVATE -fsanitize=${CHMD_FUZZ_SANITIZERS} -fno-omit-frame-pointer)
    target_link_options(chmd_fuzzer PRIVATE -fsanitize=${CHMD_FUZZ_SANITIZERS})
endif()

if(CHMD_BUILD_BENCHMARKS)
    add_executable(chmd_benchmark benchmarks/benchmark.cpp)
    target_link_libraries(chmd_benchmark PRIVATE chmd::chmd)
endif()
