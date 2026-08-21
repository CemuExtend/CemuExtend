cmake_minimum_required(VERSION 3.21.1)
project(frontend_retirement_negative LANGUAGES CXX)

if(NOT DEFINED CEMU_SOURCE_ROOT)
	message(FATAL_ERROR "CEMU_SOURCE_ROOT is required")
endif()

include("${CEMU_SOURCE_ROOT}/cmake/FrontendRetirement.cmake")

add_library(retired_frontend INTERFACE)
target_sources(retired_frontend INTERFACE
	"$<$<BOOL:1>:${CMAKE_CURRENT_SOURCE_DIR}/gui/wxgui/Legacy.cpp>")
add_library(intermediary INTERFACE)
target_link_libraries(intermediary INTERFACE "$<LINK_ONLY:retired_frontend>")
add_executable(frontend_consumer main.cpp)
target_link_libraries(frontend_consumer PRIVATE intermediary)

cemu_assert_target_closure_has_no_source_path(frontend_consumer "/gui/wxgui/")
