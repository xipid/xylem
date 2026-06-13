cmake_minimum_required(VERSION 3.14)

# Force Clang if not specified
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "clang")
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "clang++")
endif()

project(Xylem VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable CCache
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache enabled")
endif()

# Enable Mold Linker
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=mold")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=mold")
set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -fuse-ld=mold")
message(STATUS "mold linker enabled")

# Include the local 'xic' library from the sibling directory
get_filename_component(XIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xic" ABSOLUTE)
if(NOT EXISTS "${XIC_DIR}/CMakeLists.txt")
    message(STATUS "xic not found at ${XIC_DIR}, downloading automatically...")
    execute_process(
        COMMAND git clone https://github.com/xipid/xic.git "${XIC_DIR}"
        RESULT_VARIABLE CLONE_RESULT
    )
    if(NOT CLONE_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to automatically download xic from https://github.com/xipid/xic.git")
    endif()
endif()

if(EXISTS "${XIC_DIR}/CMakeLists.txt")
    message(STATUS "Found xic at ${XIC_DIR}")
    add_subdirectory(${XIC_DIR} ${CMAKE_CURRENT_BINARY_DIR}/xic EXCLUDE_FROM_ALL)
else()
    message(FATAL_ERROR "xic not found at ${XIC_DIR}")
endif()

# Include the local 'rho' library from the sibling directory
get_filename_component(RHO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../rho" ABSOLUTE)
if(NOT EXISTS "${RHO_DIR}/CMakeLists.txt")
    message(STATUS "rho not found at ${RHO_DIR}, downloading automatically...")
    execute_process(
        COMMAND git clone https://github.com/xipid/rho.git "${RHO_DIR}"
        RESULT_VARIABLE CLONE_RESULT
    )
    if(NOT CLONE_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to automatically download rho from https://github.com/xipid/rho.git")
    endif()
endif()

if(EXISTS "${RHO_DIR}/CMakeLists.txt")
    message(STATUS "Found rho at ${RHO_DIR}")
    add_subdirectory(${RHO_DIR} ${CMAKE_CURRENT_BINARY_DIR}/rho EXCLUDE_FROM_ALL)
else()
    message(FATAL_ERROR "rho not found at ${RHO_DIR}")
endif()

# --- Automatic Source Discovery ---
file(GLOB_RECURSE XYLEM_SOURCES 
    "src/Xylem/*.cpp"
)

# --- Main Target Definition ---
add_library(Xylem STATIC ${XYLEM_SOURCES})

# Basic Include Directories
target_include_directories(Xylem PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Link against Xi and Rho
target_link_libraries(Xylem PUBLIC Xi::Xi Rho)

# --- Test Discovery ---
file(GLOB TEST_SOURCES "tests/*.cpp")
foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(xylem_${TEST_NAME} ${TEST_SRC})
    target_link_libraries(xylem_${TEST_NAME} PRIVATE Xylem)
    set_target_properties(xylem_${TEST_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
endforeach()

# --- CLI Tool ---
add_executable(xy src/xy.cpp)
target_link_libraries(xy PRIVATE Xylem)
set_target_properties(xy PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
