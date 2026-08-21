cmake_minimum_required(VERSION 3.25)
project(cemu_target_architecture_negative LANGUAGES CXX)

include("${CEMU_SOURCE_ROOT}/cmake/TargetArchitecture.cmake")

add_library(CemuCafe INTERFACE)
add_library(CemuCommon INTERFACE)
add_library(CemuHostContracts INTERFACE)
add_library(CemuUtil INTERFACE)
add_library(CemuInput INTERFACE)
target_link_libraries(CemuInput INTERFACE CemuCommon CemuHostContracts CemuUtil CemuCafe)

cemu_assert_target_internal_edges_allowed(CemuInput
	CemuCommon CemuHostContracts CemuUtil)
