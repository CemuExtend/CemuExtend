add_library(CemuWebRpc STATIC
  MainWindowState.cpp
  MainWindowState.h
  RpcDispatcher.cpp
  RpcDispatcher.h
)

add_library(CemuWebRendererAdapters STATIC
	OpenGLHost.h
  RendererHost.cpp
  RendererHost.h
)
if(ENABLE_OPENGL)
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		target_sources(CemuWebRendererAdapters PRIVATE OpenGLHostLinux.cpp)
	elseif(WIN32)
		target_sources(CemuWebRendererAdapters PRIVATE OpenGLHostWin.cpp)
	elseif(APPLE)
		target_sources(CemuWebRendererAdapters PRIVATE OpenGLHostCocoa.mm)
		target_link_libraries(CemuWebRendererAdapters PRIVATE "-framework Cocoa" "-framework OpenGL")
	endif()
endif()
target_include_directories(CemuWebRendererAdapters PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(CemuWebRendererAdapters PRIVATE
  CemuCafe
  CemuCommon
  CemuConfig
  CemuHostContracts
)

option(CEMU_REGENERATE_WEB_UI "Regenerate embedded React assets during the C++ build" OFF)
set(CEMU_WEB_ASSET_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/generated/WebAssets.h")
if(CEMU_REGENERATE_WEB_UI)
  find_program(BUN_EXECUTABLE bun REQUIRED)
  add_custom_target(cemu_web_assets
    COMMAND "${BUN_EXECUTABLE}" install --frozen-lockfile
    COMMAND "${BUN_EXECUTABLE}" run generate
    COMMAND "${BUN_EXECUTABLE}" run build:embedded
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/ui"
    BYPRODUCTS "${CEMU_WEB_ASSET_HEADER}"
    VERBATIM
  )
elseif(NOT EXISTS "${CEMU_WEB_ASSET_HEADER}")
  message(FATAL_ERROR "Embedded web assets are missing; run 'cd ui && bun run build:embedded'")
endif()
target_include_directories(CemuWebRpc PUBLIC
  "${CMAKE_SOURCE_DIR}/src"
  ${RAPIDJSON_INCLUDE_DIRS}
)

add_library(CemuWebFrontend STATIC
	NativeWindowHost.h
	RendererHost.h
	WebFrontend.cpp
	WebHostServices.cpp
	WebHostServices.h
	WebHostState.cpp
	WebHostState.h
  generated/RpcMethods.h
  generated/WebAssets.h
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(CemuWebFrontend PRIVATE NativeWindowHostGtk.cpp)
elseif(WIN32)
  target_sources(CemuWebFrontend PRIVATE NativeWindowHostWin.cpp)
  target_link_libraries(CemuWebFrontend PRIVATE imm32 shell32)
elseif(APPLE)
  enable_language(OBJCXX)
  target_sources(CemuWebFrontend PRIVATE NativeWindowHostCocoa.mm)
  target_link_libraries(CemuWebFrontend PRIVATE "-framework Cocoa" "-framework CoreGraphics" "-framework QuartzCore")
endif()
if(TARGET cemu_web_assets)
  add_dependencies(CemuWebFrontend cemu_web_assets)
endif()
target_include_directories(CemuWebFrontend PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  ${RAPIDJSON_INCLUDE_DIRS}
)
target_link_libraries(CemuWebFrontend PRIVATE
  CemuApplication
  CemuCommon
  CemuFrontendModel
  CemuHostContracts
  CemuInput
  CemuWebRpc
  CemuUtil
  CemuWebRendererAdapters
  webview::core_static
)

if(BUILD_TESTING)
  add_executable(web_main_window_state_tests tests/MainWindowStateTests.cpp)
  target_link_libraries(web_main_window_state_tests PRIVATE CemuWebRpc)
  add_test(NAME web_main_window_state_tests COMMAND web_main_window_state_tests)

  add_executable(web_rpc_dispatcher_tests tests/RpcDispatcherTests.cpp)
  target_link_libraries(web_rpc_dispatcher_tests PRIVATE CemuWebRpc)
  add_test(NAME web_rpc_dispatcher_tests COMMAND web_rpc_dispatcher_tests)

	add_executable(web_host_state_tests WebHostState.cpp tests/WebHostStateTests.cpp)
	target_include_directories(web_host_state_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
	target_link_libraries(web_host_state_tests PRIVATE CemuHostContracts)
	add_test(NAME web_host_state_tests COMMAND web_host_state_tests)
endif()
