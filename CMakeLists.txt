cmake_minimum_required(VERSION 3.30)
project(ApricotFields)

set(CMAKE_CXX_STANDARD 26)

add_library(ApricotFields STATIC
        src/apricotfields.cpp
        src/render/apricotrender.cpp)

target_include_directories(ApricotFields PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)


# Add the directory for SpudLib library
if (NOT TARGET spudlib)
    add_subdirectory(../spudlib ${CMAKE_CURRENT_BINARY_DIR}/spudlib)
endif ()
# Link SpudLib library
target_link_libraries(ApricotFields PUBLIC spudlib)

# Vulkan stuff
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

if (WIN32 OR (UNIX AND NOT APPLE))
    if (NOT Vulkan_GLSLC_EXECUTABLE)
        message(FATAL_ERROR "glslc not found. Install the Vulkan SDK.")
    endif ()

    # Shaders are compiled to the parent project's runtime output directory
    # (set by the host application, e.g. Erethal).  Fall back to the local
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
#find_package(glm REQUIRED)
#target_link_libraries(ApricotFields PRIVATE glm::glm)
include(FetchContent)
FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        0.9.9.8
)
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_MakeAvailable(glm)
target_link_libraries(ApricotFields PRIVATE glm::glm)
