include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/CefVersion.cmake")

set(CEF_ROOT "" CACHE PATH "Path to the extracted official CEF binary distribution")
if(NOT CEF_ROOT AND DEFINED ENV{CEF_ROOT})
	set(CEF_ROOT "$ENV{CEF_ROOT}" CACHE PATH "Path to the extracted official CEF binary distribution" FORCE)
endif()
if(NOT CEF_ROOT)
	set(CEF_ROOT "${CMAKE_SOURCE_DIR}/dependencies/cef" CACHE PATH "Path to the extracted official CEF binary distribution" FORCE)
endif()
get_filename_component(CEF_ROOT "${CEF_ROOT}" ABSOLUTE)

string(TOLOWER "${CMAKE_BUILD_TYPE}" _cemu_cef_build_type)
set(_cef_config_dir Release)
if(_cemu_cef_build_type STREQUAL "debug")
	set(_cef_config_dir Debug)
endif()

set(_cef_required
	include/cef_app.h
	cmake/FindCEF.cmake
	libcef_dll/CMakeLists.txt)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	list(APPEND _cef_required
		${_cef_config_dir}/libcef.so
		${_cef_config_dir}/libEGL.so
		${_cef_config_dir}/libGLESv2.so
		${_cef_config_dir}/chrome-sandbox
		${_cef_config_dir}/v8_context_snapshot.bin
		${_cef_config_dir}/libvk_swiftshader.so
		${_cef_config_dir}/libvulkan.so.1
		${_cef_config_dir}/vk_swiftshader_icd.json
		Resources/icudtl.dat
		Resources/resources.pak
		Resources/chrome_100_percent.pak
		Resources/chrome_200_percent.pak
		Resources/locales)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	list(APPEND _cef_required
		${_cef_config_dir}/libcef.dll
		${_cef_config_dir}/libcef.lib
		${_cef_config_dir}/chrome_elf.dll
		${_cef_config_dir}/d3dcompiler_47.dll
		${_cef_config_dir}/dxcompiler.dll
		${_cef_config_dir}/dxil.dll
		${_cef_config_dir}/libEGL.dll
		${_cef_config_dir}/libGLESv2.dll
		${_cef_config_dir}/v8_context_snapshot.bin
		${_cef_config_dir}/vk_swiftshader.dll
		${_cef_config_dir}/vulkan-1.dll
		${_cef_config_dir}/vk_swiftshader_icd.json
		Resources/icudtl.dat
		Resources/resources.pak
		Resources/chrome_100_percent.pak
		Resources/chrome_200_percent.pak
		Resources/locales)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	set(_cef_framework "${_cef_config_dir}/Chromium Embedded Framework.framework")
	list(APPEND _cef_required
		"${_cef_framework}/Chromium Embedded Framework"
		"${_cef_framework}/Libraries/libEGL.dylib"
		"${_cef_framework}/Libraries/libGLESv2.dylib"
		"${_cef_framework}/Libraries/libvk_swiftshader.dylib"
		"${_cef_framework}/Libraries/libvulkan.dylib"
		"${_cef_framework}/Libraries/vk_swiftshader_icd.json"
		"${_cef_framework}/Resources/icudtl.dat"
		"${_cef_framework}/Resources/resources.pak")
endif()

foreach(_relative IN LISTS _cef_required)
	if(NOT EXISTS "${CEF_ROOT}/${_relative}")
		message(FATAL_ERROR
			"CEF_ROOT='${CEF_ROOT}' is not a valid CEF ${CEMU_CEF_VERSION} "
			"${CEMU_CEF_PLATFORM} distribution: missing ${_relative}. "
			"Run scripts/fetch-cef.sh ${CEMU_CEF_PLATFORM} or pass "
			"-DCEF_ROOT=/path/to/extracted/cef.")
	endif()
endforeach()

# Cemu is the subprocess executable on Windows; it does not use CEF's
# bootstrap-DLL packaging mode.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	set(USE_SANDBOX OFF CACHE BOOL "Enable or disable use of the CEF sandbox" FORCE)
endif()

# cef_variables.cmake otherwise prefers the host architecture on macOS, which
# is wrong for a cross-architecture CMAKE_OSX_ARCHITECTURES build.
if(CEMU_CEF_PLATFORM STREQUAL "linux64" OR
	CEMU_CEF_PLATFORM STREQUAL "windows64" OR
	CEMU_CEF_PLATFORM STREQUAL "macosx64")
	set(PROJECT_ARCH "x86_64")
elseif(CEMU_CEF_PLATFORM STREQUAL "linuxarm64" OR
	CEMU_CEF_PLATFORM STREQUAL "macosarm64")
	set(PROJECT_ARCH "arm64")
endif()

list(PREPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	# The wrapper contains the scoped framework loader in Objective-C++ and is
	# added before Cemu's platform source lists enable these languages.
	enable_language(OBJC OBJCXX)
endif()
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "${CMAKE_BINARY_DIR}/cef/libcef_dll_wrapper")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT TARGET libcef_lib)
	ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
endif()

add_library(CemuCefRuntime INTERFACE)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	# macOS loads the versioned framework through CefScopedLibraryLoader.
	target_link_libraries(CemuCefRuntime INTERFACE libcef_dll_wrapper ${CEF_STANDARD_LIBS})
else()
	target_link_libraries(CemuCefRuntime INTERFACE libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
endif()
target_include_directories(CemuCefRuntime INTERFACE "${CEF_ROOT}")

set(CEMU_CEF_RUNTIME_DIR
	"${CEF_ROOT}/$<IF:$<CONFIG:Debug>,Debug,Release>"
	CACHE INTERNAL "CEF binary runtime directory")
set(CEMU_CEF_RESOURCE_DIR "${CEF_ROOT}/Resources" CACHE INTERNAL "CEF resource directory")
set(CEMU_CEF_MACOS_HELPER_SUFFIXES "${CEF_HELPER_APP_SUFFIXES}"
	CACHE INTERNAL "CEF-required macOS helper app suffix definitions")

function(_cemu_copy_cef_licenses target destination)
	foreach(_license LICENSE.txt README.txt CREDITS.html)
		if(EXISTS "${CEF_ROOT}/${_license}")
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND "${CMAKE_COMMAND}" -E copy_if_different
					"${CEF_ROOT}/${_license}" "${destination}/CEF-${_license}")
		endif()
	endforeach()
endfunction()

function(cemu_configure_cef_runtime_target target)
	if(NOT TARGET ${target})
		message(FATAL_ERROR
			"cemu_configure_cef_runtime_target: unknown target '${target}'")
	endif()
	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		set_target_properties(${target} PROPERTIES
			BUILD_WITH_INSTALL_RPATH TRUE
			BUILD_RPATH "$ORIGIN"
			INSTALL_RPATH "$ORIGIN")
	endif()
endfunction()

function(cemu_stage_cef_runtime target)
	if(NOT TARGET ${target})
		message(FATAL_ERROR "cemu_stage_cef_runtime: unknown target '${target}'")
	endif()

	if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		foreach(_file libcef.so libEGL.so libGLESv2.so chrome-sandbox
			v8_context_snapshot.bin libvk_swiftshader.so vk_swiftshader_icd.json)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND "${CMAKE_COMMAND}" -E copy_if_different
					"${CEMU_CEF_RUNTIME_DIR}/${_file}" "$<TARGET_FILE_DIR:${target}>/${_file}")
		endforeach()
		if(EXISTS "${CEF_ROOT}/Release/libminigbm.so" OR
			EXISTS "${CEF_ROOT}/Debug/libminigbm.so")
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND "${CMAKE_COMMAND}" -E copy_if_different
					"${CEMU_CEF_RUNTIME_DIR}/libminigbm.so" "$<TARGET_FILE_DIR:${target}>/libminigbm.so")
		endif()

		# Keep CEF's SwiftShader loader away from VulkanAPI.cpp's host-loader
		# search path. The CEF runtime can opt into this private directory later.
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/cef-swiftshader"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${CEMU_CEF_RUNTIME_DIR}/libvulkan.so.1"
				"$<TARGET_FILE_DIR:${target}>/cef-swiftshader/libvulkan.so.1"
			COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE_DIR:${target}>/libvulkan.so.1")

		foreach(_file icudtl.dat resources.pak chrome_100_percent.pak chrome_200_percent.pak)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND "${CMAKE_COMMAND}" -E copy_if_different
					"${CEMU_CEF_RESOURCE_DIR}/${_file}" "$<TARGET_FILE_DIR:${target}>/${_file}")
		endforeach()
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CEMU_CEF_RESOURCE_DIR}/locales" "$<TARGET_FILE_DIR:${target}>/locales")
		_cemu_copy_cef_licenses(${target} "$<TARGET_FILE_DIR:${target}>")
	elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
		foreach(_file chrome_elf.dll d3dcompiler_47.dll dxcompiler.dll dxil.dll
			libcef.dll libEGL.dll libGLESv2.dll v8_context_snapshot.bin)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND "${CMAKE_COMMAND}" -E copy_if_different
					"${CEMU_CEF_RUNTIME_DIR}/${_file}" "$<TARGET_FILE_DIR:${target}>/${_file}")
		endforeach()
		# vulkan-1.dll must not shadow the system Vulkan loader used by Cemu.
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/cef-swiftshader"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${CEMU_CEF_RUNTIME_DIR}/vulkan-1.dll"
				"$<TARGET_FILE_DIR:${target}>/cef-swiftshader/vulkan-1.dll"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${CEMU_CEF_RUNTIME_DIR}/vk_swiftshader.dll"
				"$<TARGET_FILE_DIR:${target}>/cef-swiftshader/vk_swiftshader.dll"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${CEMU_CEF_RUNTIME_DIR}/vk_swiftshader_icd.json"
				"$<TARGET_FILE_DIR:${target}>/cef-swiftshader/vk_swiftshader_icd.json"
			COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE_DIR:${target}>/vulkan-1.dll")
		foreach(_file icudtl.dat resources.pak chrome_100_percent.pak chrome_200_percent.pak)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND "${CMAKE_COMMAND}" -E copy_if_different
					"${CEMU_CEF_RESOURCE_DIR}/${_file}" "$<TARGET_FILE_DIR:${target}>/${_file}")
		endforeach()
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CEMU_CEF_RESOURCE_DIR}/locales" "$<TARGET_FILE_DIR:${target}>/locales")
		_cemu_copy_cef_licenses(${target} "$<TARGET_FILE_DIR:${target}>")

	elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
		get_target_property(_is_bundle ${target} MACOSX_BUNDLE)
		if(_is_bundle)
			COPY_MAC_FRAMEWORK("${target}" "${CEMU_CEF_RUNTIME_DIR}" "$<TARGET_BUNDLE_DIR:${target}>")
			_cemu_copy_cef_licenses(${target} "$<TARGET_BUNDLE_DIR:${target}>/Contents/Resources")
		else()
			# Used by command-line smoke tests. Production targets should be app bundles.
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND "${CMAKE_COMMAND}" -E copy_directory
					"${CEMU_CEF_RUNTIME_DIR}/Chromium Embedded Framework.framework"
					"$<TARGET_FILE_DIR:${target}>/Chromium Embedded Framework.framework")
			_cemu_copy_cef_licenses(${target} "$<TARGET_FILE_DIR:${target}>")
		endif()
	endif()
endfunction()

# Register helper targets created by the application layer. CEF requires the
# five helper app variants listed in CEMU_CEF_MACOS_HELPER_SUFFIXES; creation of
# those targets stays with the application because it owns the subprocess entry
# source and Info.plist bundle identifiers.
function(cemu_stage_cef_macos_helper main_target helper_target helper_output_name)
	if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
		message(FATAL_ERROR "cemu_stage_cef_macos_helper is only available on macOS")
	endif()
	if(NOT TARGET ${main_target} OR NOT TARGET ${helper_target})
		message(FATAL_ERROR "cemu_stage_cef_macos_helper received an unknown target")
	endif()
	add_dependencies(${main_target} ${helper_target})
	add_custom_command(TARGET ${main_target} POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"$<TARGET_BUNDLE_DIR:${helper_target}>"
			"$<TARGET_BUNDLE_DIR:${main_target}>/Contents/Frameworks/${helper_output_name}.app")
endfunction()

unset(_cemu_cef_build_type)
unset(_cef_config_dir)
unset(_cef_framework)
unset(_cef_required)
