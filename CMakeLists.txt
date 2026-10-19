# CMake 3.20 added support for requesting C++23.
cmake_minimum_required(VERSION 3.20)

# For clangd / Zed (Ninja generator). VS generators ignore this.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Silence unused-variable warning when a C compiler is set by a toolchain/IDE.
if(CMAKE_C_COMPILER)
endif()

set(LIBNAME "trmesh")

# Target Godot 4.7+ APIs. Must be set before add_subdirectory(godot-cpp).
set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API version to target")
# Required for editor hot-reload (pairs with reloadable=true in the .gdextension).
set(GODOTCPP_USE_HOT_RELOAD ON CACHE BOOL "Enable GDExtension hot reload accounting")

find_package(Python3 3.4 REQUIRED)

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/godot-cpp/src")
	message(NOTICE "godot-cpp bindings source not found; initializing submodule...")
	execute_process(
		COMMAND git submodule update --init --recursive godot-cpp
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		COMMAND_ERROR_IS_FATAL ANY
	)
endif()

# godot-cpp defines options before its own project(); keep that order.
add_subdirectory(godot-cpp SYSTEM)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp/gen/include")

get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)
get_target_property(GODOTCPP_PLATFORM godot-cpp GODOTCPP_PLATFORM)

project(TRMeshGDExt
	VERSION 0.1.0
	DESCRIPTION "Minimal GDExtension for TRMesh"
	LANGUAGES CXX
)

add_library(${LIBNAME} SHARED)

target_sources(${LIBNAME}
	PRIVATE
		src/register_types.cpp
		src/register_types.h
		src/mesh/half_edge_mesh.cpp
		src/mesh/half_edge_mesh.h
		src/mesh/mesh_recording.cpp
		src/mesh/mesh_recording.h
		src/mesh/topology_iterators.hpp
		src/mesh/topology_data.hpp
		src/mesh/utils/face_builder.cpp
		src/mesh/utils/face_builder.h
		src/trmesh_instance_3d.cpp
		src/trmesh_instance_3d.h
		src/utils/bump_allocator.hpp
		src/utils/slot_map.hpp
)

target_include_directories(${LIBNAME} PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(${LIBNAME} PRIVATE godot-cpp)

set_target_properties(${LIBNAME}
	PROPERTIES
		CXX_STANDARD 23
		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF
		# Generator expression avoids MSVC Debug/Release subdirectories.
		LIBRARY_OUTPUT_DIRECTORY "$<1:${PROJECT_SOURCE_DIR}/bin/${GODOTCPP_PLATFORM}>"
		RUNTIME_OUTPUT_DIRECTORY "$<1:${PROJECT_SOURCE_DIR}/bin/${GODOTCPP_PLATFORM}>"
		PDB_OUTPUT_DIRECTORY "$<1:${PROJECT_SOURCE_DIR}/bin/${GODOTCPP_PLATFORM}>"
		PREFIX "lib"
		OUTPUT_NAME "${LIBNAME}${GODOTCPP_SUFFIX}"
)

include(CTest)
if(BUILD_TESTING)
	add_subdirectory(tests)
endif()
