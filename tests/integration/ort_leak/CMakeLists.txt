cmake_minimum_required(VERSION 3.16)
project(cpuinfo_ownership_probe LANGUAGES C CXX)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

set(CPUINFO_SOURCE_DIR "" CACHE PATH "Exact cpuinfo source used by ORT")
option(PROBE_CPUINFO_LTO "Enable IPO for the cpuinfo dependency" ON)
if(NOT EXISTS "${CPUINFO_SOURCE_DIR}/src/arm/linux/init.c")
    message(FATAL_ERROR "Provide the verified CPUINFO_SOURCE_DIR")
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ${PROBE_CPUINFO_LTO})
set(CPUINFO_LIBRARY_TYPE static CACHE STRING "" FORCE)
set(CPUINFO_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(CPUINFO_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(CPUINFO_BUILD_MOCK_TESTS OFF CACHE BOOL "" FORCE)
set(CPUINFO_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_compile_options(-ffunction-sections -fdata-sections)
add_subdirectory("${CPUINFO_SOURCE_DIR}" cpuinfo EXCLUDE_FROM_ALL)

# A non-IPO caller preserves the same public processor structure consumption
# as ORT, while the shared-library export boundary keeps cpuinfo private.
add_library(cpuinfo_probe SHARED cpuinfo_probe.c)
set_target_properties(cpuinfo_probe PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION FALSE INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
target_include_directories(cpuinfo_probe PRIVATE "${CPUINFO_SOURCE_DIR}/include")
target_link_libraries(cpuinfo_probe PRIVATE cpuinfo)
target_link_options(cpuinfo_probe PRIVATE
    "LINKER:--version-script,${CMAKE_CURRENT_SOURCE_DIR}/exports.map"
    "LINKER:--gc-sections")

add_executable(cpuinfo_ownership_probe cpuinfo_probe_main.cpp)
set_target_properties(cpuinfo_ownership_probe PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION FALSE INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
target_include_directories(cpuinfo_ownership_probe PRIVATE "${CPUINFO_SOURCE_DIR}/include")
target_compile_options(cpuinfo_ownership_probe PRIVATE
    -fsanitize=address,undefined -fno-omit-frame-pointer -g)
target_link_options(cpuinfo_ownership_probe PRIVATE -fsanitize=address,undefined)
target_link_libraries(cpuinfo_ownership_probe PRIVATE cpuinfo_probe)
