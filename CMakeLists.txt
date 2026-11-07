cmake_minimum_required(VERSION 3.20)
project(ffx-vk-linux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    message(FATAL_ERROR "ffx-vk-linux loads x86-64 Windows DLLs: Linux on x86-64 only")
endif()

find_package(Vulkan REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm libdrm_amdgpu)
find_library(SPIRV_TOOLS_LIBRARY NAMES SPIRV-Tools REQUIRED)

# vk.xml: the Vulkan SDK's or the system's registry
find_file(VK_XML vk.xml PATHS ${Vulkan_INCLUDE_DIR}/../share/vulkan/registry /usr/share/vulkan/registry NO_DEFAULT_PATH)
if(NOT VK_XML)
    message(FATAL_ERROR "vk.xml (the Vulkan registry) not found - install the Vulkan headers' registry")
endif()
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vk_thunks.inc
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_vk_thunks.py ${VK_XML} ${CMAKE_CURRENT_BINARY_DIR}/vk_thunks.inc
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_vk_thunks.py ${VK_XML}
    COMMENT "Vulkan thunks from vk.xml")

add_library(ffx_vk_linux SHARED
    src/pe.cpp src/teb.cpp src/win32.cpp src/win32_runtime.cpp src/win32_security.cpp
    src/adapter.cpp src/spirv_images.cpp src/vk_thunks.cpp src/ffxvk.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/vk_thunks.inc)
target_include_directories(ffx_vk_linux PUBLIC include third_party/ffx/api/include third_party/ffx/upscalers/include PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(ffx_vk_linux PUBLIC Vulkan::Headers PRIVATE ${CMAKE_DL_LIBS} pthread OpenSSL::Crypto ${SPIRV_TOOLS_LIBRARY} PkgConfig::DRM)
set_target_properties(ffx_vk_linux PROPERTIES CXX_VISIBILITY_PRESET hidden)
target_compile_options(ffx_vk_linux PRIVATE -Wall -Wno-attributes -Wno-deprecated-declarations)

add_executable(peinfo tools/peinfo.cpp)
add_executable(ffxquery tools/ffxquery.cpp tools/upscale_smoke.cpp)
target_link_libraries(ffxquery PRIVATE ffx_vk_linux Vulkan::Vulkan)

include(CTest)
if(BUILD_TESTING)
    add_executable(runtime_checks tests/win32_runtime.cpp src/win32_runtime.cpp src/teb.cpp)
    target_include_directories(runtime_checks PRIVATE src)
    target_link_libraries(runtime_checks PRIVATE pthread)
    target_compile_options(runtime_checks PRIVATE -UNDEBUG)
    add_test(NAME windows_runtime COMMAND runtime_checks)

    add_executable(security_checks tests/win32_security.cpp src/win32_security.cpp)
    target_include_directories(security_checks PRIVATE src)
    target_link_libraries(security_checks PRIVATE OpenSSL::Crypto)
    add_test(NAME windows_security COMMAND security_checks)

    add_executable(spirv_rewrite_checks tests/spirv_rewrite.cpp src/spirv_images.cpp)
    target_include_directories(spirv_rewrite_checks PRIVATE src)
    target_link_libraries(spirv_rewrite_checks PRIVATE ${SPIRV_TOOLS_LIBRARY})
    find_program(SPIRV_AS spirv-as)
    find_program(SPIRV_VAL spirv-val)
    if(SPIRV_AS AND SPIRV_VAL)
        add_test(NAME spirv_images COMMAND Python3::Interpreter
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spirv_images.py $<TARGET_FILE:spirv_rewrite_checks>)
    endif()
endif()
