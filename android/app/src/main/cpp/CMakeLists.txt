cmake_minimum_required(VERSION 3.22.1)
project(sparx_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

# ─── QNN Runtime (loaded dynamically from device) ─────────────────────────────
# We don't link QNN statically — it's dlopen'd at runtime from the device's
# /vendor/lib64/ or /system/lib64/. This avoids redistributing Qualcomm binaries.

# ─── Agent Core Sources ───────────────────────────────────────────────────────
add_library(sparx_agent SHARED
    agent_jni.cpp
    qnn_inference.cpp
    proactive_engine_android.cpp
)

target_include_directories(sparx_agent PRIVATE
    ${CMAKE_SOURCE_DIR}/../../../../cli/include    # Reuse existing headers
    ${CMAKE_SOURCE_DIR}
)

# Link Android log + dl (for dlopen of QNN)
find_library(log-lib log)
target_link_libraries(sparx_agent ${log-lib} dl)
