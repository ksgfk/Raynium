cmake_minimum_required(VERSION 3.25)
project(RayniumRuntime VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Raynium native currently requires a 64-bit Windows MSVC-compatible toolchain. Use an x64 developer terminal and the supplied presets.")
endif()
if(NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded" OR
   NOT RADRAY_DEPENDENCY_TRIPLET STREQUAL "windows-x64-msvc-staticcrt")
    message(FATAL_ERROR "Use the Raynium presets: bridge and RadRay dependencies must use /MT, including Debug.")
endif()
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release)$" OR CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Use the single-configuration Ninja Debug/Release presets.")
endif()

get_filename_component(RAYNIUM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(RAYNIUM_MANAGED_RADRAY_DIR "${RAYNIUM_ROOT}/external/RadRay")
set(RADRAY_SOURCE_DIR "${RAYNIUM_MANAGED_RADRAY_DIR}" CACHE PATH "RadRay source checkout; defaults to the script-managed dependency")
get_filename_component(RADRAY_SOURCE_DIR "${RADRAY_SOURCE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

if(NOT EXISTS "${RADRAY_SOURCE_DIR}/CMakeLists.txt" OR
   NOT EXISTS "${RADRAY_SOURCE_DIR}/modules/render/include/radray/render/rhi.h")
    message(FATAL_ERROR "RadRay source is missing at ${RADRAY_SOURCE_DIR}. Run: python ${RAYNIUM_ROOT}/tools/fetch_radray.py restore. CMake never downloads dependencies.")
endif()

if(RADRAY_SOURCE_DIR STREQUAL RAYNIUM_MANAGED_RADRAY_DIR)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/radray.lock.json" RAYNIUM_RADRAY_LOCK)
    string(JSON RAYNIUM_RADRAY_COMMIT GET "${RAYNIUM_RADRAY_LOCK}" commit)
    find_package(Git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${RADRAY_SOURCE_DIR}" rev-parse --verify HEAD
        RESULT_VARIABLE RAYNIUM_RADRAY_GIT_RESULT
        OUTPUT_VARIABLE RAYNIUM_RADRAY_HEAD
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT RAYNIUM_RADRAY_GIT_RESULT EQUAL 0 OR
       NOT RAYNIUM_RADRAY_HEAD STREQUAL RAYNIUM_RADRAY_COMMIT)
        message(FATAL_ERROR "The managed RadRay revision differs from native/radray.lock.json. Run tools/fetch_radray.py restore explicitly.")
    endif()
else()
    message(STATUS "Using local RadRay source override: ${RADRAY_SOURCE_DIR}; the dependency lock is not enforced for this override.")
endif()

find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${RADRAY_SOURCE_DIR}" rev-parse HEAD
    RESULT_VARIABLE revision_result OUTPUT_VARIABLE RAYNIUM_RADRAY_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${RADRAY_SOURCE_DIR}" status --porcelain --untracked-files=normal
    RESULT_VARIABLE status_result OUTPUT_VARIABLE radray_status OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT revision_result EQUAL 0 OR NOT status_result EQUAL 0)
    message(FATAL_ERROR "Cannot record RadRay source identity.")
endif()
set(RAYNIUM_RADRAY_DIRTY 0)
set(RAYNIUM_RADRAY_DIRTY_JSON false)
if(radray_status)
    set(RAYNIUM_RADRAY_DIRTY 1)
    set(RAYNIUM_RADRAY_DIRTY_JSON true)
endif()
set(RAYNIUM_TOOLCHAIN "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
file(GLOB_RECURSE identity_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*" "${CMAKE_CURRENT_SOURCE_DIR}/src/*" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/*" "${RADRAY_SOURCE_DIR}/modules/*"
    "${RADRAY_SOURCE_DIR}/cmake/*")
list(APPEND identity_files "${CMAKE_CURRENT_LIST_FILE}" "${CMAKE_CURRENT_SOURCE_DIR}/CMakePresets.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/radray.lock.json" "${RADRAY_SOURCE_DIR}/CMakeLists.txt"
    "${RADRAY_SOURCE_DIR}/project_manifest.json")
if(EXISTS "${RADRAY_SOURCE_DIR}/project_manifest.lock.json")
    list(APPEND identity_files "${RADRAY_SOURCE_DIR}/project_manifest.lock.json")
endif()
list(SORT identity_files)
set(identity "${RAYNIUM_RADRAY_REVISION};${radray_status};${CMAKE_BUILD_TYPE};${RAYNIUM_TOOLCHAIN};MT;${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_DEBUG};${CMAKE_CXX_FLAGS_RELEASE}")
foreach(source IN LISTS identity_files)
    file(SHA256 "${source}" source_hash)
    string(APPEND identity ";${source_hash}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${identity_files})
string(SHA256 RAYNIUM_BUILD_ID "${identity}")
configure_file(cmake/build_info.h.in generated/raynium/build_info.h @ONLY)
configure_file(cmake/native-build.json.in bin/native-build.json @ONLY)

# RadRay finds prepared packages; this host never fetches or builds third-party dependencies.
set(RADRAY_BUILD_TESTS OFF)
set(RADRAY_BUILD_BENCHMARKS OFF)
set(RADRAY_BUILD_COMPILE_COMMANDS OFF)
set(RADRAY_BUILD_WINDOW OFF)
set(RADRAY_BUILD_RENDER ON)
set(RADRAY_ENABLE_D3D12 ON)
set(RADRAY_ENABLE_VULKAN ON)
set(RADRAY_BUILD_SHADER OFF)
set(RADRAY_BUILD_SHADER_COMPILER OFF)
set(RADRAY_BUILD_SHADER_TOOLS OFF)
set(RADRAY_ENABLE_MIMALLOC ON)
set(RADRAY_ENABLE_PROFILER OFF)
set(RADRAY_ENABLE_ZLIB OFF)
set(RADRAY_ENABLE_LIBPNG OFF)
set(RADRAY_ENABLE_LIBJPEG OFF)
set(RADRAY_ENABLE_AUTO_OPTIMIZATION OFF)
set(RADRAY_INSTALL OFF)

add_subdirectory("${RADRAY_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/radray")

add_library(rayniumruntime SHARED
    src/environment.cpp include/raynium/environment.h
    src/bridge.cpp include/raynium/bridge.h
    src/runtime.cpp include/raynium/runtime.h
    src/preview.cpp include/raynium/preview.h
    src/terrain.cpp include/raynium/terrain.h)
target_compile_features(rayniumruntime PRIVATE cxx_std_20)
target_include_directories(rayniumruntime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(rayniumruntime PRIVATE RAYNIUM_RUNTIME_EXPORTS)
target_compile_options(rayniumruntime PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8)
target_link_libraries(rayniumruntime PRIVATE RadRay::render)
set_target_properties(rayniumruntime PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS rayniumruntime RUNTIME DESTINATION . COMPONENT runtime)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/bin/native-build.json" DESTINATION . COMPONENT runtime)
