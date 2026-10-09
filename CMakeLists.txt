cmake_minimum_required(VERSION 3.30)
project(ApricotFields)

set(CMAKE_CXX_STANDARD 26)

add_library(ApricotFields STATIC
        src/apricotfields.cpp
        src/apricore.cpp
        src/render/aprend_internal.hpp
        src/render/aprendcontext.cpp
        src/render/aprendbuffers.cpp
        src/render/aprendimages_internal.hpp
        src/render/aprendimages.cpp
        src/render/aprendframes.cpp
        src/render/aprendcommands.cpp
        src/render/aprendpipeline.cpp
        src/render/aprendswapchain.cpp
        src/archive/aparchive_internal.hpp
        src/archive/aparchive.cpp
        src/audio/apaudio.cpp
        src/audio/apaudiocues.cpp
        src/math/apmathbvh.cpp
        src/sync/apsync_internal.hpp
        src/sync/apsyncnet.cpp
        include/apricore.h
        include/render/aprendcontext.h
        include/render/aprendbuffers.h
        include/render/aprendimages.h
        include/render/aprendframes.h
        include/render/aprendcommands.h
        include/render/aprendpipeline.h
        include/render/aprendswapchain.h
        include/archive/aparchive.h
        include/audio/apaudio.h
        include/audio/apaudiocues.h
        include/math/apmathbvh.h
        include/sync/apsyncnet.h
        )

target_include_directories(ApricotFields PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)


# Add the directory for SpudLib library
if (NOT TARGET spudlib)
    add_subdirectory(../spudlib ${CMAKE_CURRENT_BINARY_DIR}/spudlib)
endif ()
# Link SpudLib library
target_link_libraries(ApricotFields PUBLIC spudlib)

# ApSync runs each session on a thread of its own (src/sync/apsyncnet.cpp).
find_package(Threads REQUIRED)
target_link_libraries(ApricotFields PRIVATE Threads::Threads)

# Vulkan stuff -- only needed on Windows/Linux, where SpudGPU's Vulkan
# backend is selected and shaders are compiled to SPIR-V via glslc. ApricotFields
# never includes Vulkan headers directly (it only calls spudgpu.h's
# backend-agnostic C API), so this must not run on Apple, where SpudGPU
# uses the Metal backend instead.
if (WIN32 OR (UNIX AND NOT APPLE))
    # Manually hint where Vulkan is located on Windows
    # On Linux, it is automatically /usr/include and /usr/lib64
    if (WIN32)
        set(Vulkan_INCLUDE_DIR "C:/VulkanSDK/1.4.341.1/Include")
        set(Vulkan_LIBRARY "C:/VulkanSDK/1.4.341.1/Lib/vulkan-1.lib")
        set(Vulkan_GLSLC_EXECUTABLE "C:/VulkanSDK/1.4.341.1/Bin/glslc.exe")
    endif ()

    find_package(Vulkan REQUIRED)
    target_include_directories(ApricotFields PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(ApricotFields PRIVATE Vulkan::Vulkan)

    if (NOT Vulkan_GLSLC_EXECUTABLE)
        message(FATAL_ERROR "glslc not found. Install the Vulkan SDK.")
    endif ()

    # Shaders are compiled to the parent project's runtime output directory
    # (set by whatever project adds this directory).  Fall back to the local
    # build dir when ApricotFields is built standalone.
    if (CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(APRICOT_SHADER_OUTDIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")
    else ()
        set(APRICOT_SHADER_OUTDIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    endif ()

    function(apricot_compile_shader TARGET SHADER)
        get_filename_component(NAME ${SHADER} NAME)
        set(SPV "${APRICOT_SHADER_OUTDIR}/${NAME}.spv")
        add_custom_command(
                OUTPUT ${SPV}
                COMMAND ${CMAKE_COMMAND} -E make_directory "${APRICOT_SHADER_OUTDIR}"
                COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER} -o ${SPV}
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}
        )
        target_sources(${TARGET} PRIVATE ${SPV})
    endfunction()

    apricot_compile_shader(ApricotFields shaders/apricot_default.vert)
    apricot_compile_shader(ApricotFields shaders/apricot_default.frag)
endif ()

# GLM — header-only math library used internally by the C++ backends
set(GLM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.deps/glm-src")
include(FetchContent)
FetchContent_Declare(
        glm
        SOURCE_DIR ${GLM_DIR}
)
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_MakeAvailable(glm)
target_link_libraries(ApricotFields PUBLIC glm::glm)

# libdeflate - DEFLATE and CRC-32 for ApArchive. Vendored under
# eqdev/.deps/libdeflate (git checkout of tag v1.26), like glm and
# SPIRV-Cross. Pinned on purpose: DEFLATE output is part of what makes an
# archive byte-identical, and a libdeflate upgrade can change it. Only the
# static library is built. Gzip support stays on because libdeflate only
# compiles its CRC-32 with it; the zlib wrapper is off.
set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_ZLIB_SUPPORT OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_GZIP_SUPPORT ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        libdeflate
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../.deps/libdeflate
)
FetchContent_MakeAvailable(libdeflate)
# ApricotFields may be linked into a shared library too.
set_target_properties(libdeflate_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ApricotFields PRIVATE libdeflate::libdeflate_static)

# glaze - JSON, for ApAudio's sound bank manifests (src/audio/apaudiocues.cpp).
# Header only, vendored under eqdev/.deps/glaze like libdeflate. Private: no
# glaze type crosses ApricotFields' C API.
FetchContent_Declare(
        glaze
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../.deps/glaze
)
FetchContent_MakeAvailable(glaze)
target_link_libraries(ApricotFields PRIVATE glaze::glaze)

# Headless tests - no GPU, no window. Run with ctest, or the executable.
option(APRICOTFIELDS_BUILD_TESTS "Build ApricotFields' headless tests" ON)
if (APRICOTFIELDS_BUILD_TESTS)
    enable_testing()
    add_executable(aparchive_tests tests/aparchive_tests.cpp)
    target_link_libraries(aparchive_tests PRIVATE ApricotFields)
    add_test(NAME aparchive_tests COMMAND aparchive_tests)
    add_executable(apaudio_tests tests/apaudio_tests.c)
    target_link_libraries(apaudio_tests PRIVATE ApricotFields)
    add_test(NAME apaudio_tests COMMAND apaudio_tests)
    # Two sessions talking over the loopback interface: no GPU, no window,
    # but it does open a listening socket on 127.0.0.1.
    add_executable(apsync_tests tests/apsync_tests.c)
    target_link_libraries(apsync_tests PRIVATE ApricotFields)
    add_test(NAME apsync_tests COMMAND apsync_tests)
    add_executable(apmathbvh_tests tests/apmathbvh_tests.c)
    target_link_libraries(apmathbvh_tests PRIVATE ApricotFields)
    add_test(NAME apmathbvh_tests COMMAND apmathbvh_tests)
endif ()
