add_library(CemuHostContracts INTERFACE)
target_sources(CemuHostContracts INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/../host/contracts/HostContracts.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/FrontendRuntime.h"
)
target_include_directories(CemuHostContracts INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(CemuFrontendModel STATIC
  ArchiveInstallPolicy.cpp
  ArchiveInstallPolicy.h
  CemuExtendFrontendBridge.cpp
  CemuExtendFrontendBridge.h
)

target_include_directories(CemuFrontendModel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(CemuRuntimeOverlay STATIC
	RuntimeOverlay.cpp
	RuntimeOverlay.h
)
target_include_directories(CemuRuntimeOverlay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

if(BUILD_TESTING)
  add_executable(cemuextend_frontend_bridge_tests
    tests/CemuExtendFrontendBridgeTests.cpp
  )
  target_link_libraries(cemuextend_frontend_bridge_tests PRIVATE CemuFrontendModel)
  add_test(NAME cemuextend_frontend_bridge_tests COMMAND cemuextend_frontend_bridge_tests)

  add_executable(archive_install_policy_tests
    tests/ArchiveInstallPolicyTests.cpp
  )
  target_link_libraries(archive_install_policy_tests PRIVATE CemuFrontendModel)
  add_test(NAME archive_install_policy_tests COMMAND archive_install_policy_tests)

	add_executable(runtime_overlay_tests
		RuntimeOverlay.cpp
		tests/RuntimeOverlayTests.cpp
	)
	target_include_directories(runtime_overlay_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
	add_test(NAME runtime_overlay_tests COMMAND runtime_overlay_tests)
endif()
