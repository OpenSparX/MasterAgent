#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "MasterAgent::Core" for configuration "Release"
set_property(TARGET MasterAgent::Core APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(MasterAgent::Core PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libmaster_agent_core.a"
  )

list(APPEND _cmake_import_check_targets MasterAgent::Core )
list(APPEND _cmake_import_check_files_for_MasterAgent::Core "${_IMPORT_PREFIX}/lib/libmaster_agent_core.a" )

# Import target "MasterAgent::MemoryShortTermCore" for configuration "Release"
set_property(TARGET MasterAgent::MemoryShortTermCore APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(MasterAgent::MemoryShortTermCore PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libmemory_short_term_core.a"
  )

list(APPEND _cmake_import_check_targets MasterAgent::MemoryShortTermCore )
list(APPEND _cmake_import_check_files_for_MasterAgent::MemoryShortTermCore "${_IMPORT_PREFIX}/lib/libmemory_short_term_core.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
