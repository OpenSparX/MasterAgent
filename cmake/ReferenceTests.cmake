add_executable(test_reference_runtime tests/test_reference_runtime.cpp)
target_link_libraries(test_reference_runtime PRIVATE MasterAgent::Core)
add_test(NAME test_reference_runtime COMMAND test_reference_runtime)
if(MASTER_AGENT_BUILD_CLI)
    add_test(NAME reference_cli_demo COMMAND sparx demo automotive)
    set_tests_properties(reference_cli_demo PROPERTIES PASS_REGULAR_EXPRESSION "\"temperature\":22")
    add_test(NAME reference_cli_unknown COMMAND sparx unsupported-command)
    set_tests_properties(reference_cli_unknown PROPERTIES WILL_FAIL TRUE)
endif()
add_test(NAME installed_consumer
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_CONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestConsumer.cmake)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(MASTER_AGENT_ENABLE_HTTP AND MASTER_AGENT_BUILD_CLI)
    add_executable(http_model_probe tests/http_model_probe.cpp)
    target_link_libraries(http_model_probe PRIVATE MasterAgent::Http)
    add_test(NAME http_model_contract
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_http_model.py $<TARGET_FILE:sparx> $<TARGET_FILE:http_model_probe>)
endif()
add_executable(test_check_failure tests/check_failure.cpp)
add_test(NAME release_assertions_are_active COMMAND test_check_failure)
set_tests_properties(release_assertions_are_active PROPERTIES WILL_FAIL TRUE)
add_test(NAME evaluation_script_contract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_eval_runner.py ${CMAKE_CURRENT_SOURCE_DIR}/eval/run_all.sh)
if(MASTER_AGENT_ENABLE_STORAGE)
    add_executable(test_durable_runtime tests/test_durable_runtime.cpp)
    target_link_libraries(test_durable_runtime PRIVATE MasterAgent::Core)
    add_test(NAME test_durable_runtime COMMAND test_durable_runtime)
    if(MASTER_AGENT_BUILD_CLI)
        add_executable(durable_probe tests/durable_probe.cpp)
        target_link_libraries(durable_probe PRIVATE MasterAgent::Core)
        add_test(NAME crash_recovery_contract
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_crash_recovery.py
                $<TARGET_FILE:durable_probe> $<TARGET_FILE:sparx>)
        set_tests_properties(crash_recovery_contract PROPERTIES TIMEOUT 60)
    endif()
endif()

add_executable(test_execution_context tests/test_execution_context.cpp)
target_link_libraries(test_execution_context PRIVATE MasterAgent::Core)
add_test(NAME test_execution_context COMMAND test_execution_context)
set_tests_properties(test_execution_context PROPERTIES TIMEOUT 15)
if(MASTER_AGENT_ENABLE_HTTP AND MASTER_AGENT_ENABLE_STORAGE AND MASTER_AGENT_BUILD_CLI)
    add_subdirectory(examples/inventory_agent)
    add_test(NAME inventory_service_contract
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_inventory_contract.py
            $<TARGET_FILE:inventory_agent> $<TARGET_FILE:sparx>
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/inventory_agent/service.py)
    set_tests_properties(inventory_service_contract PROPERTIES TIMEOUT 45)
endif()
