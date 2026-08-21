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

include("${CMAKE_SOURCE_DIR}/cmake/FrontendRetirement.cmake")

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
cemu_assert_target_has_no_edge(CemuHostContracts CemuCafe CemuApplication)
cemu_assert_target_has_no_edge(CemuInput CemuCafe CemuApplication)
cemu_assert_target_has_no_edge(CemuComponents CemuCafe CemuConfig CemuInput CemuApplication)
cemu_assert_target_has_no_edge(CemuCafe CemuApplication)
cemu_assert_target_has_no_edge(CemuWebRpc CemuCafe CemuApplication)
cemu_assert_target_has_no_edge(CemuWebRendererAdapters CemuApplication)
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
cemu_assert_target_has_no_sources(CemuComponents
	napi.cpp napi_act.cpp napi_ec.cpp napi_helper.cpp napi_idbe.cpp napi_version.cpp
	ncrypto.cpp nex.cpp nexFriends.cpp nexThread.cpp prudp.cpp ppcAssembler.cpp
	DownloadManager.cpp DownloadManager.h)
cemu_assert_target_has_no_sources(CemuConfig ActiveSettings.cpp LaunchSettings.cpp)
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
	add_test(NAME frontend_retirement_guards
		COMMAND ${CMAKE_COMMAND}
			-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
			-P ${CMAKE_SOURCE_DIR}/cmake/CheckFrontendRetirement.cmake)
	add_test(NAME wx_frontend_selection_rejected
		COMMAND ${CMAKE_COMMAND}
			-S ${CMAKE_SOURCE_DIR}
			-B ${CMAKE_BINARY_DIR}/wx-frontend-negative
			-DCEMU_FRONTEND=wx
			-DENABLE_VCPKG=OFF)
	set_tests_properties(wx_frontend_selection_rejected PROPERTIES WILL_FAIL TRUE)
	add_test(NAME frontend_source_guard_negative_input
		COMMAND ${CMAKE_COMMAND}
			-S ${CMAKE_SOURCE_DIR}/cmake/tests/frontend_retirement_negative
			-B ${CMAKE_BINARY_DIR}/frontend-retirement-negative
			-DCEMU_SOURCE_ROOT=${CMAKE_SOURCE_DIR})
	set_tests_properties(frontend_source_guard_negative_input PROPERTIES WILL_FAIL TRUE)
	add_executable(update_worker_mailbox_tests
		"${CMAKE_SOURCE_DIR}/cmake/tests/UpdateWorkerMailboxTests.cpp")
	target_include_directories(update_worker_mailbox_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
	add_test(NAME update_worker_mailbox_tests COMMAND update_worker_mailbox_tests)
	set_tests_properties(update_worker_mailbox_tests PROPERTIES TIMEOUT 5)

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

if(CEMU_FRONTEND STREQUAL "webview")
	target_link_libraries(CemuBin PRIVATE CemuWebFrontend)
endif()

cemu_collect_target_link_closure(CemuBin cemu_binary_link_closure)
foreach(linked_target IN LISTS cemu_binary_link_closure)
	if(linked_target MATCHES "(^|::)(wx|Wx)|Wx")
		message(FATAL_ERROR "Retired wxWidgets target reached CemuBin: ${linked_target}")
	endif()
endforeach()
cemu_assert_target_closure_has_no_source_path(CemuBin "/gui/wxgui/")

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
