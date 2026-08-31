if(NOT BUILD_TESTING)
	return()
endif()

add_executable(cpp_rpx_oracle_trace rpx_oracle_trace.cpp)
target_include_directories(cpp_rpx_oracle_trace PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(cpp_rpx_oracle_trace PRIVATE
	"$<LINK_GROUP:RESCAN,CemuCafe>"
)
set_target_properties(cpp_rpx_oracle_trace PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<1:>"
)

add_executable(cpp_rpl_link_oracle_trace rpl_link_oracle_trace.cpp)
target_include_directories(cpp_rpl_link_oracle_trace PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(cpp_rpl_link_oracle_trace PRIVATE
	"$<LINK_GROUP:RESCAN,CemuCafe>"
)
set_target_properties(cpp_rpl_link_oracle_trace PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<1:>"
)

add_executable(cpp_rpl_call_oracle_trace rpl_call_oracle_trace.cpp)
target_include_directories(cpp_rpl_call_oracle_trace PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(cpp_rpl_call_oracle_trace PRIVATE
	"$<LINK_GROUP:RESCAN,CemuCafe>"
)
set_target_properties(cpp_rpl_call_oracle_trace PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<1:>"
)

add_executable(cpp_rpl_data_oracle_trace rpl_data_oracle_trace.cpp)
target_include_directories(cpp_rpl_data_oracle_trace PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(cpp_rpl_data_oracle_trace PRIVATE
	"$<LINK_GROUP:RESCAN,CemuCafe>"
)
set_target_properties(cpp_rpl_data_oracle_trace PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<1:>"
)
