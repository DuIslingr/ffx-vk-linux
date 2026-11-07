cmake_minimum_required(VERSION 3.20)
project(ffx-vk-linux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    message(FATAL_ERROR "ffx-vk-linux loads x86-64 Windows DLLs: Linux on x86-64 only")
endif()

find_package(Vulkan REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# vk.xml: the Vulkan SDK's or the system's registry
find_file(VK_XML vk.xml PATHS ${Vulkan_INCLUDE_DIR}/../share/vulkan/registry /usr/share/vulkan/registry NO_DEFAULT_PATH)
if(NOT VK_XML)
    message(FATAL_ERROR "vk.xml (the Vulkan registry) not found - install the Vulkan headers' registry")
endif()
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vk_thunks.inc
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_vk_thunks.py ${VK_XML} ${CMAKE_CURRENT_BINARY_DIR}/vk_thunks.inc
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_vk_thunks.py ${VK_XML}
    COMMENT "Vulkan thunks from vk.xml")

add_library(ffx_vk_linux SHARED src/pe.cpp src/teb.cpp src/win32.cpp src/vk_thunks.cpp src/ffxvk.cpp ${CMAKE_CURRENT_BINARY_DIR}/vk_thunks.inc)
target_include_directories(ffx_vk_linux PUBLIC include third_party/ffx/api/include third_party/ffx/upscalers/include PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(ffx_vk_linux PUBLIC Vulkan::Headers PRIVATE ${CMAKE_DL_LIBS} pthread)
set_target_properties(ffx_vk_linux PROPERTIES CXX_VISIBILITY_PRESET hidden)
target_compile_options(ffx_vk_linux PRIVATE -Wall -Wno-attributes -Wno-deprecated-declarations)

add_executable(peinfo tools/peinfo.cpp)
add_executable(ffxquery tools/ffxquery.cpp)
target_link_libraries(ffxquery PRIVATE ffx_vk_linux Vulkan::Vulkan)
