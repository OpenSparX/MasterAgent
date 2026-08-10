function(add_master_agent_test TARGET_NAME SOURCE_FILE)
    add_executable(${TARGET_NAME} ${SOURCE_FILE})
    target_link_libraries(${TARGET_NAME} PRIVATE MasterAgent::Core)
    target_include_directories(${TARGET_NAME}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
    set_tests_properties(${TARGET_NAME} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

add_master_agent_test(test_e2e test_e2e.cpp)
add_master_agent_test(test_atomic test_atomic.cpp)
add_master_agent_test(test_atomic_durability test_atomic_durability.cpp)
add_master_agent_test(test_orchestrator_durability
    test_orchestrator_durability.cpp)
add_master_agent_test(test_orchestrator_controls
    test_orchestrator_controls.cpp)
add_master_agent_test(test_inference test_inference.cpp)
add_master_agent_test(test_observability test_observability.cpp)
add_master_agent_test(test_resilience test_resilience.cpp)
add_master_agent_test(test_contracts test_contracts.cpp)
add_master_agent_test(test_mcp_wire_contracts
    test_mcp_wire_contracts.cpp)
add_master_agent_test(test_prompt_skill test_prompt_skill.cpp)
add_master_agent_test(test_preprocess test_preprocess.cpp)
add_master_agent_test(test_agent_dispatch_control
    test_agent_dispatch_control.cpp)

# Compile and execute the colleague-owned L1 short-term memory test without
# changing its source. Only the target wiring belongs to MasterAgent.
add_executable(test_memory_short_term_l1
    "${MEMORY_SHORT_TERM_SOURCE_ROOT}/tests/test_short_term_api.cpp")
target_link_libraries(test_memory_short_term_l1
    PRIVATE memory_short_term_core)
target_include_directories(test_memory_short_term_l1 PRIVATE
    "${MEMORY_SHORT_TERM_SOURCE_ROOT}/tests")
add_test(NAME test_memory_short_term_l1
    COMMAND test_memory_short_term_l1)
set_tests_properties(test_memory_short_term_l1 PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

if(MASTER_AGENT_ENABLE_IPC)
    add_master_agent_test(test_ipc test_ipc.cpp)
endif()

# Contract tests for the concrete model runtimes. These need no llama-server
# and no libGenie.so: they verify seal validation, work-unit estimation, and
# safe construction on a host with no NPU.
if(MASTER_AGENT_BUILD_CLI)
    add_executable(test_sparx_runtimes test_sparx_runtimes.cpp)
    target_link_libraries(test_sparx_runtimes PRIVATE
        MasterAgent::Core
        MasterAgent::SparxRuntimes)
    target_include_directories(test_sparx_runtimes
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME test_sparx_runtimes COMMAND test_sparx_runtimes)
    set_tests_properties(test_sparx_runtimes PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
