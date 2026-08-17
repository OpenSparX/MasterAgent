cmake_minimum_required(VERSION 3.22.1)
project(sparx_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

# ─── All inference backends use dlopen at runtime ────────────────────────────
# GenieX, QNN, NeuroPilot — none are linked statically.
# This keeps our binary free of any Qualcomm/MTK proprietary code.
# The actual SDK libs are loaded from the device's system partitions.

# ─── Agent Core Library ──────────────────────────────────────────────────────
# Contains: JNI bridge, proactive engine, legacy QNN dlopen path
add_library(sparx_agent SHARED
    agent_jni.cpp
    qnn_inference.cpp
    proactive_engine_android.cpp
)

# ─── GenieX Backend Library ──────────────────────────────────────────────────
# Separate .so — loaded on demand only on Qualcomm devices.
# This avoids UnsatisfiedLinkError on non-Qualcomm devices.
add_library(sparx_geniex SHARED
    geniex_backend_jni.cpp
)

# ─── Include paths ───────────────────────────────────────────────────────────
target_include_directories(sparx_agent PRIVATE
    ${CMAKE_SOURCE_DIR}/../../../../cli/include
    ${CMAKE_SOURCE_DIR}
)

target_include_directories(sparx_geniex PRIVATE
    ${CMAKE_SOURCE_DIR}
)

# ─── Link dependencies ──────────────────────────────────────────────────────
find_library(log-lib log)

target_link_libraries(sparx_agent ${log-lib} dl)
target_link_libraries(sparx_geniex ${log-lib} dl)

# ─── Future: llama.cpp CPU backend ───────────────────────────────────────────
# add_library(sparx_llamacpp SHARED llamacpp_backend_jni.cpp)
# target_link_libraries(sparx_llamacpp ${log-lib} ggml llama)
# ─── Future: MTK NeuroPilot backend ──────────────────────────────────────────
# add_library(sparx_neuropilot SHARED neuropilot_backend_jni.cpp)
# target_link_libraries(sparx_neuropilot ${log-lib} dl)
