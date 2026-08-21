project(cemuMain)

include("${CMAKE_SOURCE_DIR}/cmake/TargetArchitecture.cmake")

option(CEMU_CXX_FLAGS "Additional flags used for compiling Cemu source code")
if(CEMU_CXX_FLAGS)
	add_compile_options(${CEMU_CXX_FLAGS})
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message( FATAL_ERROR "Pointers are not 64bit" )
endif()

if(MSVC)
	add_compile_definitions(WIN32_LEAN_AND_MEAN CURL_STATICLIB)
elseif(UNIX)
	if(APPLE)
		if (ENABLE_VULKAN)
			add_compile_definitions(
				_XOPEN_SOURCE
				VK_USE_PLATFORM_MACOS_MVK
				VK_USE_PLATFORM_METAL_EXT
			)
		else()
			add_compile_definitions(_XOPEN_SOURCE)
		endif()
	else()
		if (ENABLE_VULKAN)
			add_compile_definitions(
				VK_USE_PLATFORM_XLIB_KHR # legacy. Do we need to support XLIB surfaces?
				VK_USE_PLATFORM_XCB_KHR
			)
			if (ENABLE_WAYLAND)
				add_compile_definitions(VK_USE_PLATFORM_WAYLAND_KHR)
			endif()
		endif()
	endif()
	# warnings
	if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
		add_compile_options(-Wno-ambiguous-reversed-operator)
	endif()

    add_compile_options(-Wno-multichar -Wno-invalid-offsetof -Wno-switch -Wno-ignored-attributes -Wno-deprecated-enum-enum-conversion)
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

# on msvc we can reuse the PCH from CemuCommon, on other platforms it's not straight forward to reuse the PCH so we opt out of it
function(cemu_use_precompiled_header target_name)
	if(MSVC)
		target_precompile_headers(${target_name} REUSE_FROM CemuCommon)
	else()
		target_precompile_headers(${target_name} PRIVATE "${CMAKE_SOURCE_DIR}/src/Common/precompiled.h")
	endif()
endfunction()

add_subdirectory(Common)
add_subdirectory(frontend)
add_subdirectory(gui)
if(CEMU_FRONTEND STREQUAL "webview")
	add_subdirectory(webview)
endif()
add_subdirectory(Cafe)
add_subdirectory(application)
add_subdirectory(Cemu)
add_subdirectory(config)
add_subdirectory(input)
add_subdirectory(audio)
add_subdirectory(util)
add_subdirectory(imgui)
add_subdirectory(resource)

function(cemu_collect_target_link_closure root_target out_var)
	set(queue "${root_target}")
	set(visited "")
	set(closure "")
	while(queue)
		list(POP_FRONT queue current)
		if(current IN_LIST visited OR NOT TARGET "${current}")
			continue()
		endif()
		list(APPEND visited "${current}")
		foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
			get_target_property(edges "${current}" "${property}")
			if(NOT edges)
				continue()
			endif()
			foreach(edge IN LISTS edges)
				if(TARGET "${edge}")
					list(APPEND closure "${edge}")
					list(APPEND queue "${edge}")
				endif()
			endforeach()
		endforeach()
	endwhile()
	list(REMOVE_DUPLICATES closure)
	set(${out_var} "${closure}" PARENT_SCOPE)
endfunction()

function(cemu_assert_target_has_no_edge target)
	cemu_collect_target_link_closure("${target}" link_edges)
	foreach(forbidden IN LISTS ARGN)
		if(forbidden IN_LIST link_edges)
			message(FATAL_ERROR
				"Forbidden transitive architecture edge: ${target} -> ... -> ${forbidden}")
		endif()
	endforeach()
endfunction()

function(cemu_assert_target_has_direct_edges target)
	if(NOT TARGET "${target}")
		return()
	endif()
	set(direct_edges "")
	foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
		get_target_property(property_edges "${target}" "${property}")
		if(property_edges)
			list(APPEND direct_edges ${property_edges})
		endif()
	endforeach()
	foreach(required IN LISTS ARGN)
		if(NOT required IN_LIST direct_edges)
			message(FATAL_ERROR
				"Missing explicit architecture edge: ${target} -> ${required}")
		endif()
	endforeach()
endfunction()

function(cemu_assert_target_has_no_sources target)
	if(NOT TARGET "${target}")
		return()
	endif()
	get_target_property(target_sources "${target}" SOURCES)
	foreach(forbidden IN LISTS ARGN)
		get_filename_component(forbidden_name "${forbidden}" NAME)
		foreach(source IN LISTS target_sources)
			get_filename_component(source_name "${source}" NAME)
			if(source_name STREQUAL forbidden_name)
				message(FATAL_ERROR
					"Forbidden architecture source ownership: ${target} contains ${source}")
			endif()
		endforeach()
	endforeach()
endfunction()

# Configure-time target DAG guards. Forward edges are intentionally allowed;
# these assertions reject the reverse edges that would form architecture cycles.
cemu_assert_target_has_no_edge(CemuHostContracts CemuCafe CemuApplication CemuWxGui)
cemu_assert_target_has_no_edge(CemuInput CemuCafe CemuApplication CemuWxGui)
cemu_assert_target_has_no_edge(CemuComponents CemuCafe CemuConfig CemuInput CemuApplication CemuWxGui)
cemu_assert_target_has_no_edge(CemuCafe CemuApplication CemuWxGui)
cemu_assert_target_has_no_edge(CemuApplication CemuWxGui)
cemu_assert_target_has_no_edge(CemuWebRpc CemuCafe CemuApplication CemuWxGui)
cemu_assert_target_has_no_edge(CemuWebFrontend CemuWxGui CemuWxFrontend
	CemuWxRendererAdapters CemuWxDebuggerAdapters CemuWxDeviceAdapters)
cemu_assert_target_has_no_edge(CemuWebRendererAdapters CemuApplication CemuWxGui)
# The normal GUI sees declarations only. The composition target is the sole
# owner of implementation archive wiring.
cemu_assert_target_has_direct_edges(CemuWxGui CemuWxAdapterContracts)
cemu_assert_target_has_no_edge(CemuWxGui
	CemuWxRendererAdapters CemuWxDebuggerAdapters CemuWxDeviceAdapters)
cemu_assert_target_has_direct_edges(CemuWxFrontend
	CemuWxGui CemuWxRendererAdapters CemuWxDebuggerAdapters CemuWxDeviceAdapters)
cemu_assert_target_has_no_edge(CemuWxAdapterContracts
	CemuCafe CemuConfig CemuInput CemuApplication CemuWxGui
	CemuWxRendererAdapters CemuWxDebuggerAdapters CemuWxDeviceAdapters)
cemu_assert_target_has_no_edge(CemuWxGuiCommon CemuCafe CemuApplication CemuWxGui CemuWxDebuggerAdapters)
cemu_assert_target_has_no_edge(CemuWxRendererAdapters CemuApplication CemuWxGui)
cemu_assert_target_has_no_edge(CemuWxDebuggerAdapters CemuApplication CemuWxGui)
cemu_assert_target_has_no_edge(CemuWxDeviceAdapters CemuApplication CemuWxGui CemuWxDebuggerAdapters CemuWxRendererAdapters)
cemu_assert_target_has_direct_edges(CemuWxRendererAdapters CemuWxAdapterContracts)
cemu_assert_target_has_direct_edges(CemuWxDebuggerAdapters CemuWxAdapterContracts)
cemu_assert_target_has_direct_edges(CemuWxDeviceAdapters CemuWxAdapterContracts)
cemu_assert_target_has_direct_edges(CemuWxGui
	CemuAudio CemuConfig CemuDownloadManager CemuInput CemuUtil)
cemu_assert_target_has_direct_edges(CemuCafe CemuComponents CemuConfig CemuInput CemuResource CemuUtil)
cemu_assert_target_has_direct_edges(CemuConfig CemuComponents)
cemu_assert_target_has_direct_edges(CemuDownloadManager
	CemuCafe CemuComponents CemuConfig CemuUtil)
cemu_assert_target_has_direct_edges(CemuInput CemuUtil)

# Exact internal-target allow matrix. Third-party targets are intentionally
# outside this matrix; every in-tree Cemu* edge must be named here.
cemu_assert_target_internal_edges_allowed(CemuHostContracts)
cemu_assert_target_internal_edges_allowed(CemuComponents
	CemuCommon CemuHostContracts CemuUtil)
cemu_assert_target_internal_edges_allowed(CemuInput
	CemuCommon CemuHostContracts CemuUtil)
cemu_assert_target_internal_edges_allowed(CemuConfig
	CemuCommon CemuComponents)
cemu_assert_target_internal_edges_allowed(CemuCafe
	CemuCommon CemuComponents CemuConfig CemuHostContracts CemuInput CemuExtendDomain CemuResource CemuUtil CemuWaylandProtocols)
cemu_assert_target_internal_edges_allowed(CemuDownloadManager
	CemuCafe CemuCommon CemuComponents CemuConfig CemuHostContracts CemuUtil)
cemu_assert_target_internal_edges_allowed(CemuApplication
	CemuAudio CemuCafe CemuCommon CemuConfig CemuInput CemuComponents CemuDownloadManager CemuUtil)
cemu_assert_target_internal_edges_allowed(CemuWebRpc)
cemu_assert_target_internal_edges_allowed(CemuWebFrontend
	CemuApplication CemuCommon CemuFrontendModel CemuHostContracts CemuInput CemuUtil CemuWebRpc
	CemuWebRendererAdapters)
cemu_assert_target_internal_edges_allowed(CemuWebRendererAdapters
	CemuCafe CemuCommon CemuConfig CemuHostContracts)
cemu_assert_target_internal_edges_allowed(CemuWxAdapterContracts CemuHostContracts)
cemu_assert_target_internal_edges_allowed(CemuWxGuiCommon CemuCommon)
cemu_assert_target_internal_edges_allowed(CemuWxRendererAdapters
	CemuCafe CemuCommon CemuHostContracts CemuConfig CemuWxAdapterContracts CemuWaylandProtocols)
cemu_assert_target_internal_edges_allowed(CemuWxDebuggerAdapters
	CemuCafe CemuCommon CemuComponents CemuConfig CemuResource CemuUtil CemuWxAdapterContracts CemuWxGuiCommon)
cemu_assert_target_internal_edges_allowed(CemuWxDeviceAdapters
	CemuCafe CemuCommon CemuComponents CemuConfig CemuResource CemuWxAdapterContracts)
cemu_assert_target_internal_edges_allowed(CemuWxGui
	CemuAudio CemuCommon CemuConfig CemuWxGuiCommon CemuApplication CemuExtendDomain
	CemuFrontendModel CemuHostContracts CemuInput CemuResource CemuUtil
	CemuWxAdapterContracts CemuComponents CemuDownloadManager CemuWaylandProtocols)
cemu_assert_target_internal_edges_allowed(CemuWxFrontend
	CemuWxGui CemuWxRendererAdapters CemuWxDebuggerAdapters CemuWxDeviceAdapters)
cemu_assert_target_has_no_sources(CemuComponents
	napi.cpp napi_act.cpp napi_ec.cpp napi_helper.cpp napi_idbe.cpp napi_version.cpp
	ncrypto.cpp nex.cpp nexFriends.cpp nexThread.cpp prudp.cpp ppcAssembler.cpp
	DownloadManager.cpp DownloadManager.h)
cemu_assert_target_has_no_sources(CemuConfig ActiveSettings.cpp LaunchSettings.cpp)
cemu_assert_target_has_no_sources(CemuWxGui
	AudioDebuggerWindow.cpp
	AudioDebuggerWindow.h
	EmulatedUSBDevices/EmulatedUSBDeviceAdapter.cpp
	EmulatedUSBDevices/EmulatedUSBDeviceAdapter.h
	EmulatedUSBDevices/EmulatedUSBDeviceFrame.cpp
	EmulatedUSBDevices/EmulatedUSBDeviceFrame.h
	debugger/BreakpointWindow.cpp
	debugger/BreakpointWindow.h
	components/TextList.cpp
	components/TextList.h
	helpers/wxCustomEvents.cpp
	helpers/wxCustomEvents.h
	debugger/DumpCtrl.cpp
	debugger/DumpCtrl.h
	debugger/DumpWindow.cpp
	debugger/DumpWindow.h
	debugger/DebuggerWindow2.cpp
	debugger/DebuggerWindow2.h
	debugger/DebuggerWindowAdapter.cpp
	debugger/DebuggerWindowAdapter.h
	debugger/DisasmCtrl.cpp
	debugger/DisasmCtrl.h
	debugger/ModuleWindow.cpp
	debugger/ModuleWindow.h
	debugger/MemorySearcherAdapter.cpp
	debugger/MemorySearcherAdapter.h
	debugger/RegisterWindow.cpp
	debugger/RegisterWindow.h
	debugger/SymbolCtrl.cpp
	debugger/SymbolCtrl.h
	debugger/SymbolWindow.cpp
	debugger/SymbolWindow.h
	MemorySearcherTool.cpp
	MemorySearcherTool.h
	windows/PPCThreadsViewer/DebugPPCThreadsWindow.cpp
	windows/PPCThreadsViewer/DebugPPCThreadsWindow.h
	windows/TextureRelationViewer/TextureRelationWindow.cpp
	windows/TextureRelationViewer/TextureRelationWindow.h
)

# Source-level layer checks are mandatory for every configuration, including
# production builds with BUILD_TESTING=OFF. The ctest registration below keeps
# the same guard visible in test reports as well.
set(SOURCE_ROOT "${CMAKE_SOURCE_DIR}")
include("${CMAKE_SOURCE_DIR}/cmake/CheckArchitecture.cmake")

if(BUILD_TESTING)
	add_test(NAME architecture_guards
		COMMAND ${CMAKE_COMMAND}
			-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
			-P ${CMAKE_SOURCE_DIR}/cmake/CheckArchitecture.cmake)
	add_test(NAME architecture_guards_negative_input
		COMMAND ${CMAKE_COMMAND}
			-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}/cmake/tests/architecture_negative
			-P ${CMAKE_SOURCE_DIR}/cmake/CheckArchitecture.cmake)
	set_tests_properties(architecture_guards_negative_input PROPERTIES WILL_FAIL TRUE)
	add_test(NAME architecture_targets_negative_input
		COMMAND ${CMAKE_COMMAND}
			-S "${CMAKE_SOURCE_DIR}/cmake/tests/target_architecture_negative"
			-B "${CMAKE_BINARY_DIR}/architecture-target-negative"
			-DCEMU_SOURCE_ROOT=${CMAKE_SOURCE_DIR})
	set_tests_properties(architecture_targets_negative_input PROPERTIES WILL_FAIL TRUE)
	add_executable(cemu_update_worker_tests
		"${CMAKE_SOURCE_DIR}/cmake/tests/CemuUpdateWorkerTests.cpp")
	target_include_directories(cemu_update_worker_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
	add_test(NAME cemu_update_worker_tests COMMAND cemu_update_worker_tests)
	set_tests_properties(cemu_update_worker_tests PROPERTIES TIMEOUT 5)

	function(cemu_add_static_self_containment_test test_name library)
		if(NOT TARGET "${library}")
			return()
		endif()
		add_executable("${test_name}"
			"${CMAKE_SOURCE_DIR}/cmake/tests/StaticLibrarySelfContainment.cpp")
		target_link_libraries("${test_name}" PRIVATE
			"$<LINK_LIBRARY:WHOLE_ARCHIVE,${library}>" ${ARGN})
	endfunction()

	# Force every object from each architectural archive into a consumer. This
	# catches missing direct/transitive dependencies that an ordinary static link
	# can hide by never extracting the affected object.
	cemu_add_static_self_containment_test(components_self_containment CemuComponents)
	cemu_add_static_self_containment_test(input_self_containment CemuInput)
	cemu_add_static_self_containment_test(config_self_containment CemuConfig)
	cemu_add_static_self_containment_test(cafe_self_containment CemuCafe)
	cemu_add_static_self_containment_test(download_manager_self_containment CemuDownloadManager)
	cemu_add_static_self_containment_test(application_self_containment CemuApplication)
	if(ENABLE_WXWIDGETS)
		cemu_add_static_self_containment_test(renderer_adapter_self_containment CemuWxRendererAdapters)
		cemu_add_static_self_containment_test(debugger_adapter_self_containment CemuWxDebuggerAdapters)
		cemu_add_static_self_containment_test(device_adapter_self_containment CemuWxDeviceAdapters)
		cemu_add_static_self_containment_test(wxgui_self_containment CemuWxGui
			CemuWxRendererAdapters CemuWxDebuggerAdapters CemuWxDeviceAdapters)
	endif()
endif()

add_executable(CemuBin
	main.cpp
	mainLLE.cpp
)

if(CEMU_FRONTEND STREQUAL "headless")
	target_sources(CemuBin PRIVATE headless/HeadlessWindowSystem.cpp)
endif()

cemu_use_precompiled_header(CemuBin)

if(MSVC AND MSVC_VERSION EQUAL 1940)
	# workaround for an msvc issue on VS 17.10 where generated ILK files are too large
	# see https://developercommunity.visualstudio.com/t/After-updating-to-VS-1710-the-size-of-/10665511
	set_target_properties(CemuBin PROPERTIES LINK_FLAGS "/INCREMENTAL:NO")
endif()

if(WIN32)
	target_sources(CemuBin PRIVATE
	resource/cemu.rc
	../dist/windows/cemu.manifest
	)
endif()

if(APPLE AND CMAKE_GENERATOR STREQUAL "Xcode")
    set_target_properties(CemuBin PROPERTIES
        XCODE_ATTRIBUTE_ENABLE_DEBUG_DYLIB[variant=Debug] "YES"
    )
endif()

string(TIMESTAMP CURRENT_YEAR "%Y" UTC)

set_property(TARGET CemuBin PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(TARGET CemuBin PROPERTY WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
set(OUTPUT_NAME "Cemu_$<LOWER_CASE:$<CONFIG>>")

if (MACOS_BUNDLE)
	set_property(TARGET CemuBin PROPERTY MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resource/MacOSXBundleInfo.plist.in")

	set(RESOURCE_FILES "${CMAKE_SOURCE_DIR}/src/resource/cemu.icns")
	target_sources(CemuBin PRIVATE "${RESOURCE_FILES}")

	set(MACOSX_BUNDLE_ICON_FILE "cemu.icns")
	set(MACOSX_BUNDLE_GUI_IDENTIFIER "info.cemu.Cemu")
	set(MACOSX_BUNDLE_BUNDLE_NAME "Cemu")
	set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${EMULATOR_VERSION_MAJOR}.${EMULATOR_VERSION_MINOR}.${EMULATOR_VERSION_PATCH}")
	set(MACOSX_BUNDLE_BUNDLE_VERSION "${EMULATOR_VERSION_MAJOR}.${EMULATOR_VERSION_MINOR}.${EMULATOR_VERSION_PATCH}")
	set(MACOSX_BUNDLE_COPYRIGHT "Copyright © ${CURRENT_YEAR} Cemu Project")

	set(MACOSX_BUNDLE_CATEGORY "public.app-category.games")
 	set(MACOSX_MINIMUM_SYSTEM_VERSION "13.4")
  	set(MACOSX_BUNDLE_TYPE_EXTENSION "wua")

	set_target_properties(CemuBin PROPERTIES
		MACOSX_BUNDLE true
		RESOURCE "${RESOURCE_FILES}"
	)

	set(FOLDERS gameProfiles resources)
	foreach(folder ${FOLDERS})
		add_custom_command (TARGET CemuBin POST_BUILD
			COMMAND ${CMAKE_COMMAND} ARGS -E copy_directory "${CMAKE_SOURCE_DIR}/bin/${folder}" "$<TARGET_BUNDLE_DIR:CemuBin>/Contents/SharedSupport/${folder}")
	endforeach(folder)

	if (ENABLE_VULKAN)
		if (EXISTS "/usr/local/lib/libMoltenVK.dylib")
			set(MOLTENVK_PATH "/usr/local/lib/libMoltenVK.dylib")
		elseif (EXISTS "/opt/homebrew/lib/libMoltenVK.dylib")
			set(MOLTENVK_PATH "/opt/homebrew/lib/libMoltenVK.dylib")
		else()
			message(FATAL_ERROR "failed to find libMoltenVK.dylib")
		endif ()
  endif()
  
	if (ENABLE_LIBUSB)
		if(CMAKE_BUILD_TYPE STREQUAL "Debug")
		    set(LIBUSB_PATH "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/lib/libusb-1.0.0.dylib")
		else()
		    set(LIBUSB_PATH "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib/libusb-1.0.0.dylib")
		endif()
	endif()

	set(UPDATE_SH_PATH "${CMAKE_SOURCE_DIR}/src/resource/update.sh")
	set(APP_BUNDLE_DIR "$<TARGET_BUNDLE_DIR:CemuBin>")
	set(FRAMEWORKS_DIR "${APP_BUNDLE_DIR}/Contents/Frameworks")
	set(RESOURCES_DIR  "${APP_BUNDLE_DIR}/Contents/Resources")

	if (ENABLE_VULKAN)
		add_custom_command(TARGET CemuBin POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy 
				"${MOLTENVK_PATH}"
				"${FRAMEWORKS_DIR}/libMoltenVK.dylib"
		)
	endif()

	add_custom_command(TARGET CemuBin POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy
			"${UPDATE_SH_PATH}"
			"${RESOURCES_DIR}/update.sh"
		COMMAND chmod 755
			"${RESOURCES_DIR}/update.sh"
	)

	if (ENABLE_LIBUSB)
		add_custom_command(TARGET CemuBin POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy
				"${LIBUSB_PATH}"
				"${FRAMEWORKS_DIR}/libusb-1.0.0.dylib"
		)
	endif()

	set_target_properties(CemuBin PROPERTIES
		BUILD_WITH_INSTALL_RPATH TRUE
		INSTALL_RPATH "@executable_path/../Frameworks"
	)

	if (ENABLE_LIBUSB)
		add_custom_command(TARGET CemuBin POST_BUILD
		COMMAND install_name_tool
			-change @rpath/libusb-1.0.0.dylib
				@executable_path/../Frameworks/libusb-1.0.0.dylib
				"$<TARGET_FILE:CemuBin>"
		)
	endif()
else()
    if(APPLE)
		if (ENABLE_VULKAN)
			find_library(MOLTENVK_LIBRARY
				NAMES MoltenVK moltenvk libMoltenVK.dylib
				PATHS /usr/local/lib /opt/homebrew/lib
			)
			if(MOLTENVK_LIBRARY)
				message(STATUS "Found MoltenVK: ${MOLTENVK_LIBRARY}")
				target_link_libraries(CemuBin PRIVATE ${MOLTENVK_LIBRARY})
			else()
				message(WARNING "libMoltenVK.dylib not found")
			endif()
			set_target_properties(CemuBin PROPERTIES
				BUILD_WITH_INSTALL_RPATH TRUE
				INSTALL_RPATH "/usr/local/lib;/opt/homebrew/lib"
			)
		endif()
    endif()
endif()

set_target_properties(CemuBin PROPERTIES
	# multi-configuration generators will add a config subdirectory to RUNTIME_OUTPUT_DIRECTORY if no generator expression is used
	# to get the same behavior everywhere we append an empty generator expression
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../bin/$<1:>"
	OUTPUT_NAME "${OUTPUT_NAME}"
)

if(ENABLE_SDL)
	target_link_libraries(CemuBin PRIVATE SDL3::SDL3)
endif()

target_link_libraries(CemuBin PRIVATE
	CemuApplication
	CemuAudio
	CemuCafe
	CemuCommon
	CemuComponents
	CemuConfig
	CemuDownloadManager
	CemuInput
	CemuResource
	CemuUtil
)

if(ENABLE_WXWIDGETS)
	target_link_libraries(CemuBin PRIVATE CemuWxFrontend)
elseif(CEMU_FRONTEND STREQUAL "webview")
	target_link_libraries(CemuBin PRIVATE CemuWebFrontend)
endif()

if(UNIX AND NOT APPLE)
	# due to nasm output some linkers will make stack executable
	# cemu does not require this so we explicity disable it
	target_link_options(CemuBin PRIVATE -z noexecstack)
	# some residual debug info from boost/discord-rpc is normally included
	# most likely not helpful in debugging problems with cemu code
	target_link_options(CemuBin PRIVATE "$<$<CONFIG:Release>:-Xlinker;--strip-debug>")
endif()

if (BSD)
	target_link_libraries(CemuBin PRIVATE execinfo SPIRV-Tools SPIRV-Tools-opt)
endif()
