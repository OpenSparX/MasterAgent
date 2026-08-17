# eval/ — Comprehensive evaluation binaries for OpenSparX subsystems.
#
# Each eval binary exercises one subsystem through synthetic but realistic
# workloads, measuring system-level metrics (latency, correctness, scaling,
# overhead). They link against MasterAgent::Core and the CLI source files that
# implement the subsystem logic.

cmake_minimum_required(VERSION 3.18)

# All eval targets share C++17 and common include paths.
set(EVAL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/cli/include")
set(EVAL_CLI_SRC_DIR "${CMAKE_SOURCE_DIR}/cli/src")

# Helper: define an eval binary that links against Core and compiles
# the relevant CLI source alongside the eval driver.
function(add_eval_target TARGET_NAME EVAL_SOURCE CLI_SOURCES)
    add_executable(${TARGET_NAME} ${EVAL_SOURCE} ${CLI_SOURCES})
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_17)
    target_include_directories(${TARGET_NAME} PRIVATE ${EVAL_INCLUDE_DIR})
    target_link_libraries(${TARGET_NAME} PRIVATE MasterAgent::Core)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        target_link_libraries(${TARGET_NAME} PRIVATE pthread)
    endif()
endfunction()

# --- eval_speculation ---
add_eval_target(eval_speculation
    "${CMAKE_CURRENT_SOURCE_DIR}/speculation/eval_speculation.cpp"
    "${EVAL_CLI_SRC_DIR}/sparx_speculative.cpp"
)

# --- eval_mesh ---
add_eval_target(eval_mesh
    "${CMAKE_CURRENT_SOURCE_DIR}/mesh/eval_mesh.cpp"
    "${EVAL_CLI_SRC_DIR}/sparx_mesh.cpp"
)

# --- eval_formal ---
add_eval_target(eval_formal
    "${CMAKE_CURRENT_SOURCE_DIR}/formal/eval_formal.cpp"
    "${EVAL_CLI_SRC_DIR}/sparx_formal_verify.cpp"
)

# --- eval_learning ---
add_eval_target(eval_learning
    "${CMAKE_CURRENT_SOURCE_DIR}/learning/eval_learning.cpp"
    "${EVAL_CLI_SRC_DIR}/sparx_learning.cpp"
)

# --- eval_constrained ---
add_eval_target(eval_constrained
    "${CMAKE_CURRENT_SOURCE_DIR}/constrained/eval_constrained.cpp"
    "${EVAL_CLI_SRC_DIR}/sparx_constrained_decode.cpp"
)
