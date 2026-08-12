# sparx — the developer-facing CLI.
#
# Deliberately a separate target from master_agent_core: the kernel must stay
# usable as an embedded library with no CLI, process-spawning, or socket code
# linked in. Everything here is host tooling plus the two concrete
# IModelRuntime adapters.

set(SPARX_SOURCES
    src/sparx_main.cpp
    src/cmd_init.cpp
    src/cmd_add.cpp
    src/cmd_pull.cpp
    src/cmd_run.cpp
    src/cmd_devices.cpp
    src/cmd_deploy.cpp
    src/cmd_doctor.cpp
    src/cmd_demo.cpp
    src/cmd_shell.cpp
    src/cmd_plan.cpp
    src/cmd_trace.cpp
    src/sparx_trace.cpp
)

# The runtime adapters are a library so tests can link them without pulling in
# main(). GenieModelRuntime uses dlopen, so it builds everywhere and reports
# "NPU unavailable" at runtime on a host without the Qualcomm stack. That is
# what lets one binary serve both a dev laptop and a device.
add_library(sparx_runtimes STATIC
    src/llama_cpp_model_runtime.cpp
    src/genie_model_runtime.cpp
)
target_compile_features(sparx_runtimes PUBLIC cxx_std_17)
target_include_directories(sparx_runtimes PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(sparx_runtimes PUBLIC MasterAgent::Core)
if(NOT WIN32)
    target_link_libraries(sparx_runtimes PUBLIC ${CMAKE_DL_LIBS})
endif()
add_library(MasterAgent::SparxRuntimes ALIAS sparx_runtimes)

add_executable(sparx ${SPARX_SOURCES})
target_include_directories(sparx PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(sparx PRIVATE
    MasterAgent::Core
    MasterAgent::SparxRuntimes)

# Version stamping. SPARX_VERSION is passed by the release workflow from the
# git tag. For local development builds, auto-detect from git describe so that
# `sparx version` shows something useful without manual -D flags.
if(NOT DEFINED SPARX_VERSION)
    execute_process(
        COMMAND git describe --tags --always
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_rc)
    if(_git_rc EQUAL 0 AND _git_version)
        string(REGEX REPLACE "^v" "" SPARX_VERSION "${_git_version}")
    else()
        set(SPARX_VERSION "0.0.0-dev")
    endif()
endif()
if(NOT DEFINED SPARX_GIT_SHA)
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _sha_rc)
    if(_sha_rc EQUAL 0 AND _git_sha)
        set(SPARX_GIT_SHA "${_git_sha}")
    else()
        set(SPARX_GIT_SHA "unknown")
    endif()
endif()
if(NOT DEFINED SPARX_BUILD_TARGET)
    set(SPARX_BUILD_TARGET "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()
target_compile_definitions(sparx PRIVATE
    SPARX_VERSION="${SPARX_VERSION}"
    SPARX_GIT_SHA="${SPARX_GIT_SHA}"
    SPARX_BUILD_TARGET="${SPARX_BUILD_TARGET}")

# Templates are embedded as string literals in cmd_init.cpp rather than
# installed as data files: `sparx init` must work from a single copied binary
# with no sibling share/ directory. The files under templates/ are the
# editable source of truth for those literals.

if(MINGW)
    copy_mingw_runtime(sparx)
endif()

install(TARGETS sparx RUNTIME DESTINATION bin)
