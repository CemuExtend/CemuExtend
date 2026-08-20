add_library(CemuHostContracts INTERFACE)
target_sources(CemuHostContracts INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/../host/contracts/HostContracts.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/FrontendRuntime.h"
)
target_include_directories(CemuHostContracts INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(CemuFrontendModel STATIC
  CemuExtendFrontendBridge.cpp
  CemuExtendFrontendBridge.h
)

target_include_directories(CemuFrontendModel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

if(BUILD_TESTING)
  add_executable(cemuextend_frontend_bridge_tests
    tests/CemuExtendFrontendBridgeTests.cpp
  )
  target_link_libraries(cemuextend_frontend_bridge_tests PRIVATE CemuFrontendModel)
  add_test(NAME cemuextend_frontend_bridge_tests COMMAND cemuextend_frontend_bridge_tests)
endif()
