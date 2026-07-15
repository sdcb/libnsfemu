cmake_minimum_required(VERSION 3.22)

project(libnsfemu VERSION 1.0.0 LANGUAGES C)

option(NSFEMU_BUILD_TOOLS "Build command-line smoke/diagnostic tools" ON)

include(CTest)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

add_library(nsfemu
    src/blip_buffer.c
    src/nes_apu.c
    src/nes_cpu.c
    src/nsfemu.c
)

target_include_directories(nsfemu
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(libnsfemu::nsfemu ALIAS nsfemu)

target_compile_features(nsfemu PUBLIC c_std_11)

set_property(TARGET nsfemu PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(nsfemu
        PRIVATE
            NSFEMU_SHARED
            NSFEMU_BUILDING
        INTERFACE
            NSFEMU_SHARED
    )
endif()

install(
    TARGETS nsfemu
    EXPORT libnsfemu-targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/libnsfemu-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libnsfemu-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/libnsfemu-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libnsfemu
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/libnsfemu-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/libnsfemu-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libnsfemu
)

install(
    EXPORT libnsfemu-targets
    NAMESPACE libnsfemu::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libnsfemu
)

if(MSVC)
    target_compile_options(nsfemu PRIVATE /W4 /wd4204 /wd4244 /wd4706)
else()
    target_compile_options(nsfemu PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

if(NSFEMU_BUILD_TOOLS)
    add_executable(nsfemu_smoke tools/nsfemu_smoke.c)
    target_link_libraries(nsfemu_smoke PRIVATE nsfemu)

    add_executable(nsfemu_api_check tools/nsfemu_api_check.c)
    target_link_libraries(nsfemu_api_check PRIVATE nsfemu)

    if(WIN32)
        add_executable(nsfemu_compare_gme tools/nsfemu_compare_gme.c)
        target_link_libraries(nsfemu_compare_gme PRIVATE nsfemu)
        set_property(TARGET nsfemu_compare_gme PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

        if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/local/nsf")
            add_test(
                NAME nsfemu_api_check
                COMMAND nsfemu_api_check
                    "${CMAKE_CURRENT_SOURCE_DIR}/local/nsf/Contra JP [Probotector] (1988-02-09)(Konami).nsf"
                    "${CMAKE_CURRENT_SOURCE_DIR}/local/nsf/赤色要塞_Akai Yousai - Final Commando (FDS)(1988-05-02)(Konami).nsf"
            )

            add_test(
                NAME nsfemu_local_compare
                COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_local_compare.ps1"
                    "$<TARGET_FILE:nsfemu_compare_gme>"
                    "${CMAKE_CURRENT_SOURCE_DIR}/local/nsf"
                    2
            )
        endif()
    endif()
endif()
