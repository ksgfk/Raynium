cmake_minimum_required(VERSION 3.25)
project(RayniumProfilerDependency LANGUAGES C CXX)

if(NOT EXISTS "${TRACY_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Prepare the pinned Tracy source and pass TRACY_SOURCE_DIR explicitly.")
endif()

add_subdirectory("${TRACY_SOURCE_DIR}" tracy)
# Export this definition with the client: the bridge starts and stops it outside loader lock.
target_compile_definitions(TracyClient PUBLIC TRACY_DELAYED_INIT)
