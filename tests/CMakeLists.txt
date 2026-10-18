include(FetchContent)

FetchContent_Declare(
	Catch2
	GIT_REPOSITORY https://github.com/catchorg/Catch2.git
	GIT_TAG v3.15.3
	GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(trmesh_slot_map_tests bump_allocator_test.cpp slot_map_test.cpp)
target_include_directories(trmesh_slot_map_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(trmesh_slot_map_tests PRIVATE Catch2::Catch2WithMain)

set_target_properties(trmesh_slot_map_tests
	PROPERTIES
		CXX_STANDARD 23
		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF
)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(trmesh_slot_map_tests)
