cmake_minimum_required(VERSION 3.30)
project(ApricotFields)

set(CMAKE_CXX_STANDARD 26)

add_library(ApricotFields STATIC
        src/apricotfields.cpp
        include/render/apricotrender.hpp
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
