cmake_minimum_required(VERSION 3.18)

project(MasterAgent VERSION 2.2.1 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MASTER_AGENT_BUILD_APP
       "Build the single-process Master Agent application" ON)
option(MASTER_AGENT_BUILD_TESTS
       "Build Master Agent tests" ON)
option(MASTER_AGENT_BUILD_CLI
       "Build the sparx developer CLI and model runtime adapters" ON)
option(MASTER_AGENT_ENABLE_IPC
       "Build the optional IPC transport adapters" OFF)
option(MASTER_AGENT_ENABLE_QNN
       "Enable the platform-provided QNN/Genie backend" OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

if(MINGW)
    get_filename_component(MINGW_RUNTIME_DIR
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(MINGW_RUNTIME_FILES
        "${MINGW_RUNTIME_DIR}/libgcc_s_seh-1.dll"
        "${MINGW_RUNTIME_DIR}/libstdc++-6.dll"
        "${MINGW_RUNTIME_DIR}/libwinpthread-1.dll")

    function(copy_mingw_runtime TARGET_NAME)
        foreach(RUNTIME_FILE IN LISTS MINGW_RUNTIME_FILES)
            if(EXISTS "${RUNTIME_FILE}")
                add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${RUNTIME_FILE}"
                        "$<TARGET_FILE_DIR:${TARGET_NAME}>")
            endif()
        endforeach()
    endfunction()
endif()

set(MEMORY_SHORT_TERM_SOURCE_ROOT
    "${CMAKE_SOURCE_DIR}/third_party/memory_short_term"
    CACHE PATH "Short-term memory source root")

if(NOT EXISTS
   "${MEMORY_SHORT_TERM_SOURCE_ROOT}/include/vehicle_memory/conversation_journal.h")
    message(FATAL_ERROR
        "Short-term memory source is incomplete: "
        "${MEMORY_SHORT_TERM_SOURCE_ROOT}")
endif()

add_library(memory_short_term_core STATIC
    "${MEMORY_SHORT_TERM_SOURCE_ROOT}/src/atomic_file_writer.cpp"
    "${MEMORY_SHORT_TERM_SOURCE_ROOT}/src/checksum.cpp"
    "${MEMORY_SHORT_TERM_SOURCE_ROOT}/src/json_conversation_journal.cpp"
    "${MEMORY_SHORT_TERM_SOURCE_ROOT}/src/short_term_api_controller.cpp")
target_compile_features(memory_short_term_core PUBLIC cxx_std_17)
target_include_directories(memory_short_term_core PUBLIC
    "$<BUILD_INTERFACE:${MEMORY_SHORT_TERM_SOURCE_ROOT}/include>"
    "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set_target_properties(memory_short_term_core PROPERTIES
    EXPORT_NAME MemoryShortTermCore)
if(WIN32)
    target_link_libraries(memory_short_term_core PRIVATE bcrypt)
endif()

# Check if the core source tree is present.
# The full kernel source (src/) is proprietary and not included in the OSS
# distribution. When absent, we build a stub library so that downstream
# targets (cli, tests, eval) can still link.
set(_core_source_present OFF)
if(EXISTS "${CMAKE_SOURCE_DIR}/src/agent_dispatch/atomic_lineage.cpp")
    set(_core_source_present ON)
endif()

if(_core_source_present)

set(MASTER_AGENT_CORE_SOURCES
    # Agent dispatch
    src/agent_dispatch/atomic_lineage.cpp
    src/agent_dispatch/control_plane.cpp
    src/agent_dispatch/dispatch_contracts.cpp
    src/agent_dispatch/dispatch_scheduler.cpp
    src/agent_dispatch/dispatch_state.cpp
    src/agent_dispatch/inference_lineage.cpp
    src/agent_dispatch/preemption_controller.cpp
    src/agent_dispatch/persistence_store.cpp
    src/agent_dispatch/query_reconcile.cpp
    src/agent_dispatch/registry_admission.cpp
    src/agent_dispatch/route_selector.cpp

    # Agent service
    src/agent_service/agent_service_impl.cpp
    src/agent_service/failure_mapping.cpp
    src/agent_service/intent_query_gateway.cpp
    src/agent_service/observability.cpp
    src/agent_service/turn_pipeline.cpp

    # Atomic service and MCP
    src/atomic_service/admission_validator.cpp
    src/atomic_service/atomic_service_impl.cpp
    src/atomic_service/deterministic_provider.cpp
    src/atomic_service/execution_scheduler.cpp
    src/atomic_service/mcp_wire.cpp
    src/atomic_service/persistence_store.cpp
    src/atomic_service/read_only_query.cpp
    src/atomic_service/tool_registry.cpp

    # Shared contracts
    src/common/types.cpp

    # Data and audit logging
    src/data_log/audit_writer.cpp
    src/data_log/event_writer.cpp
    src/data_log/payload_guard.cpp
    src/data_log/service_control.cpp
    src/data_log/service_lifecycle.cpp
    src/data_log/trace_query.cpp

    # Exception management
    src/exception/exception_digest.cpp
    src/exception/exception_query.cpp
    src/exception/exception_reporter.cpp
    src/exception/observation_drain.cpp
    src/exception/service_lifecycle.cpp
    src/exception/state_mutation.cpp

    # Inference and global arbitration
    src/inference/admission_controller.cpp
    src/inference/global_arbiter.cpp
    src/inference/inference_events.cpp
    src/inference/inference_framework_impl.cpp
    src/inference/kv_coordinator.cpp
    src/inference/model_executor.cpp
    src/inference/model_runtime.cpp

    # Intent recognition and two-stage model flow
    src/intent/bge_classifier.cpp
    src/intent/intent_engine_impl.cpp
    src/intent/mock_inference_path.cpp
    src/intent/model_decision_parser.cpp
    src/intent/query_batch_executor.cpp
    src/intent/skill_path.cpp
    src/intent_support/deterministic_skill_adapter.cpp
    src/intent_support/intent_prompt_adapter.cpp
    src/intent_support/module_factory_bridge.cpp

    # Interaction ingress
    src/interaction/interaction_layer.cpp

    # KV-cache lifecycle
    src/kv_cache/cache_invalidation.cpp
    src/kv_cache/cache_publication.cpp
    src/kv_cache/capacity_manager.cpp
    src/kv_cache/lease_acquisition.cpp
    src/kv_cache/lease_completion.cpp

    # Memory integration
    src/memory/memory_service.cpp

    # Durable task orchestration
    src/orchestrator/blocker_index.cpp
    src/orchestrator/control_plane.cpp
    src/orchestrator/dag_validator.cpp
    src/orchestrator/orchestrator_impl.cpp
    src/orchestrator/persistence_store.cpp
    src/orchestrator/result_reconciler.cpp
    src/orchestrator/scheduler.cpp

    # Input preprocessing
    src/preprocess/cleaner.cpp
    src/preprocess/host_test_state_provider.cpp
    src/preprocess/normalizer.cpp
    src/preprocess/preprocess_engine_impl.cpp
    src/preprocess/preprocess_observer.cpp
    src/preprocess/request_validation.cpp
    src/preprocess/state_query_service_impl.cpp
    src/preprocess/time_aligner.cpp
    src/preprocess/vehicle_state_collector.cpp

    # Prompt, runtime composition, skills, and SubAgent boundary
    src/prompt/prompt_engine_impl.cpp
    src/prompt/prompt_renderer.cpp
    src/prompt/prompt_template_analyzer.cpp
    src/prompt/template_manager.cpp
    src/runtime/master_agent_runtime.cpp
    src/skill/skill_content_loader.cpp
    src/skill/skill_engine_impl.cpp
    src/skill/skill_keyword_matcher.cpp
    src/skill/skill_library_store.cpp
    src/skill/skill_repository.cpp
    src/skill/skill_utils.cpp
    src/sub_agents/sub_agent.cpp)

add_library(master_agent_core STATIC ${MASTER_AGENT_CORE_SOURCES})
target_compile_features(master_agent_core PUBLIC cxx_std_17)
target_include_directories(master_agent_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(master_agent_core PUBLIC memory_short_term_core)
if(WIN32)
    target_link_libraries(master_agent_core PUBLIC bcrypt)
endif()
set_target_properties(master_agent_core PROPERTIES EXPORT_NAME Core)
add_library(MasterAgent::Core ALIAS master_agent_core)

else()
# ─── OSS stub: core source not present ───────────────────────────────────
# Build an INTERFACE library so cli/ and tests/ can still compile and link.
# The cli implements all Agent OS functionality directly; it only needs
# headers from include/ for type definitions.
message(STATUS "Core source not present (OSS build) — using stub library")
add_library(master_agent_core INTERFACE)
target_compile_features(master_agent_core INTERFACE cxx_std_17)
target_include_directories(master_agent_core INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(master_agent_core INTERFACE memory_short_term_core)
add_library(MasterAgent::Core ALIAS master_agent_core)
endif()

# Prompt and Skill use runtime-managed JSON/text configuration. Mirror the
# source configuration into the build tree so local executables and CTest share
# the same relative paths as a source-tree launch.
file(COPY "${CMAKE_SOURCE_DIR}/config"
     DESTINATION "${CMAKE_BINARY_DIR}")

if(MASTER_AGENT_ENABLE_IPC AND _core_source_present)
    target_sources(master_agent_core PRIVATE
        src/transport/ipc/ipc.cpp
        src/transport/ipc/remote_services.cpp
        src/transport/ipc/wire_codec.cpp)
    target_compile_definitions(master_agent_core
        PUBLIC MASTER_AGENT_ENABLE_IPC=1)
endif()

if(MASTER_AGENT_ENABLE_QNN)
    message(FATAL_ERROR
        "The QNN/Genie adapter is platform-provided and is not included "
        "in this source delivery.")
endif()

if(MASTER_AGENT_BUILD_APP AND _core_source_present)
    add_executable(master_agent_app app/main.cpp)
    set_target_properties(master_agent_app PROPERTIES
        OUTPUT_NAME master_agent)
    target_link_libraries(master_agent_app PRIVATE MasterAgent::Core)
    if(MINGW)
        copy_mingw_runtime(master_agent_app)
    endif()
endif()

if(MASTER_AGENT_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(MASTER_AGENT_BUILD_TESTS AND _core_source_present)
    enable_testing()
    add_subdirectory(tests)
elseif(MASTER_AGENT_BUILD_TESTS)
    # OSS build: only build strategic-feature tests that have source files.
    # Like eval/, each test compiles the relevant cli sources directly.
    enable_testing()
    set(_cli_src "${CMAKE_SOURCE_DIR}/cli/src")

    # test_integration_speculation
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_integration_speculation.cpp")
        add_executable(test_integration_speculation
            tests/test_integration_speculation.cpp
            "${_cli_src}/sparx_speculative.cpp"
            "${_cli_src}/sparx_transformer_predictor.cpp")
        target_compile_features(test_integration_speculation PRIVATE cxx_std_17)
        target_include_directories(test_integration_speculation PRIVATE
            "${CMAKE_SOURCE_DIR}/cli/include" "${CMAKE_SOURCE_DIR}/third_party")
        target_link_libraries(test_integration_speculation PRIVATE MasterAgent::Core)
        add_test(NAME test_integration_speculation COMMAND test_integration_speculation)
    endif()

    # test_orset
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_orset.cpp")
        add_executable(test_orset
            tests/test_orset.cpp
            "${_cli_src}/sparx_mesh.cpp"
            "${_cli_src}/sparx_delta_crdt.cpp")
        target_compile_features(test_orset PRIVATE cxx_std_17)
        target_include_directories(test_orset PRIVATE
            "${CMAKE_SOURCE_DIR}/cli/include" "${CMAKE_SOURCE_DIR}/third_party")
        target_link_libraries(test_orset PRIVATE MasterAgent::Core)
        add_test(NAME test_orset COMMAND test_orset)
    endif()

    # test_merkle
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_merkle.cpp")
        add_executable(test_merkle
            tests/test_merkle.cpp
            "${_cli_src}/sparx_mesh.cpp"
            "${_cli_src}/sparx_delta_crdt.cpp")
        target_compile_features(test_merkle PRIVATE cxx_std_17)
        target_include_directories(test_merkle PRIVATE
            "${CMAKE_SOURCE_DIR}/cli/include" "${CMAKE_SOURCE_DIR}/third_party")
        target_link_libraries(test_merkle PRIVATE MasterAgent::Core)
        add_test(NAME test_merkle COMMAND test_merkle)
    endif()

    # test_embedding
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_embedding.cpp")
        add_executable(test_embedding
            tests/test_embedding.cpp
            "${_cli_src}/sparx_speculative.cpp")
        target_compile_features(test_embedding PRIVATE cxx_std_17)
        target_include_directories(test_embedding PRIVATE
            "${CMAKE_SOURCE_DIR}/cli/include" "${CMAKE_SOURCE_DIR}/third_party")
        target_link_libraries(test_embedding PRIVATE MasterAgent::Core)
        add_test(NAME test_embedding COMMAND test_embedding)
    endif()

    # bench_strategic
    if(EXISTS "${CMAKE_SOURCE_DIR}/tests/bench_strategic.cpp")
        add_executable(bench_strategic
            tests/bench_strategic.cpp
            "${_cli_src}/sparx_speculative.cpp"
            "${_cli_src}/sparx_mesh.cpp"
            "${_cli_src}/sparx_delta_crdt.cpp"
            "${_cli_src}/sparx_formal_verify.cpp"
            "${_cli_src}/sparx_smt_backend.cpp")
        target_compile_features(bench_strategic PRIVATE cxx_std_17)
        target_include_directories(bench_strategic PRIVATE
            "${CMAKE_SOURCE_DIR}/cli/include" "${CMAKE_SOURCE_DIR}/third_party")
        target_link_libraries(bench_strategic PRIVATE MasterAgent::Core)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            target_link_libraries(bench_strategic PRIVATE pthread)
        endif()
        add_test(NAME bench_strategic COMMAND bench_strategic)
    endif()
endif()

add_subdirectory(eval)

if(_core_source_present)
install(TARGETS master_agent_core memory_short_term_core
    EXPORT MasterAgentTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
else()
install(TARGETS memory_short_term_core
    EXPORT MasterAgentTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(MASTER_AGENT_BUILD_APP AND _core_source_present)
    install(TARGETS master_agent_app RUNTIME DESTINATION bin)
endif()

if(MINGW)
    install(FILES ${MINGW_RUNTIME_FILES} DESTINATION bin OPTIONAL)
endif()

install(DIRECTORY include/master_agent/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/master_agent
    PATTERN "transport" EXCLUDE)
if(MASTER_AGENT_ENABLE_IPC)
    install(DIRECTORY include/master_agent/transport/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/master_agent/transport)
endif()
install(DIRECTORY third_party/nlohmann
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY "${MEMORY_SHORT_TERM_SOURCE_ROOT}/include/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md VERSION.json THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/MasterAgent)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/MasterAgent/docs)
install(DIRECTORY config/
    DESTINATION config)

configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/MasterAgentConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/MasterAgentConfig.cmake"
    INSTALL_DESTINATION
        "${CMAKE_INSTALL_LIBDIR}/cmake/MasterAgent")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/MasterAgentConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(EXPORT MasterAgentTargets
    FILE MasterAgentTargets.cmake
    NAMESPACE MasterAgent::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MasterAgent")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/MasterAgentConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/MasterAgentConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/MasterAgent")
