cmake_minimum_required(VERSION 3.18)

project(MasterAgentConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(MasterAgent 2 CONFIG REQUIRED)

add_executable(master_agent_package_consumer main.cpp)
target_compile_features(master_agent_package_consumer PRIVATE cxx_std_17)
target_link_libraries(master_agent_package_consumer
    PRIVATE MasterAgent::Core)
