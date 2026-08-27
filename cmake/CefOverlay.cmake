include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/CefVersion.cmake")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
	message(FATAL_ERROR "The CEF runtime overlay currently supports Linux x86_64 only")
endif()

set(CEF_ROOT "" CACHE PATH "Path to the extracted official CEF binary distribution")
if(NOT CEF_ROOT AND DEFINED ENV{CEF_ROOT})
	set(CEF_ROOT "$ENV{CEF_ROOT}" CACHE PATH "Path to the extracted official CEF binary distribution" FORCE)
endif()
if(NOT CEF_ROOT)
	set(CEF_ROOT "${CMAKE_SOURCE_DIR}/dependencies/cef" CACHE PATH "Path to the extracted official CEF binary distribution" FORCE)
endif()
get_filename_component(CEF_ROOT "${CEF_ROOT}" ABSOLUTE)

set(_cef_config_dir Release)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(_cef_config_dir Debug)
endif()
set(_cef_required include/cef_app.h cmake/FindCEF.cmake libcef_dll/CMakeLists.txt
	${_cef_config_dir}/libcef.so ${_cef_config_dir}/libEGL.so
	${_cef_config_dir}/libGLESv2.so ${_cef_config_dir}/chrome-sandbox
	${_cef_config_dir}/v8_context_snapshot.bin ${_cef_config_dir}/libvk_swiftshader.so
	${_cef_config_dir}/libvulkan.so.1 ${_cef_config_dir}/vk_swiftshader_icd.json
	Resources/icudtl.dat Resources/resources.pak
	Resources/chrome_100_percent.pak Resources/chrome_200_percent.pak Resources/locales)
foreach(_relative IN LISTS _cef_required)
	if(NOT EXISTS "${CEF_ROOT}/${_relative}")
		message(FATAL_ERROR
			"CEF_ROOT='${CEF_ROOT}' is not a valid CEF ${CEMU_CEF_VERSION} distribution: missing ${_relative}. "
			"Run scripts/fetch-cef-linux-x64.sh or pass -DCEF_ROOT=/path/to/extracted/cef.")
	endif()
endforeach()

list(PREPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "${CMAKE_BINARY_DIR}/cef/libcef_dll_wrapper")
if(NOT TARGET libcef_lib)
	ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
endif()

add_library(CemuCefRuntime INTERFACE)
target_link_libraries(CemuCefRuntime INTERFACE libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
target_include_directories(CemuCefRuntime INTERFACE "${CEF_ROOT}")

set(CEMU_CEF_RUNTIME_DIR
	"${CEF_ROOT}/$<IF:$<CONFIG:Debug>,Debug,Release>"
	CACHE INTERNAL "CEF binary runtime directory")
set(CEMU_CEF_RESOURCE_DIR "${CEF_ROOT}/Resources" CACHE INTERNAL "CEF resource directory")

function(cemu_stage_cef_runtime target)
	foreach(_file libcef.so libEGL.so libGLESv2.so chrome-sandbox
		v8_context_snapshot.bin libvk_swiftshader.so vk_swiftshader_icd.json)
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${CEMU_CEF_RUNTIME_DIR}/${_file}" "$<TARGET_FILE_DIR:${target}>/${_file}")
	endforeach()
	# CEF ships a private SwiftShader Vulkan loader named libvulkan.so.1. It must
	# never sit beside Cemu: VulkanAPI.cpp dlopen() would select it instead of the
	# host loader and lose the window-system surface extensions. Preserve it for
	# diagnostics without putting it on the application's library search path.
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"$<TARGET_FILE_DIR:${target}>/cef-swiftshader"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"${CEMU_CEF_RUNTIME_DIR}/libvulkan.so.1"
			"$<TARGET_FILE_DIR:${target}>/cef-swiftshader/libvulkan.so.1"
		COMMAND "${CMAKE_COMMAND}" -E rm -f
			"$<TARGET_FILE_DIR:${target}>/libvulkan.so.1")
	foreach(_file icudtl.dat resources.pak chrome_100_percent.pak chrome_200_percent.pak)
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${CEMU_CEF_RESOURCE_DIR}/${_file}" "$<TARGET_FILE_DIR:${target}>/${_file}")
	endforeach()
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CEMU_CEF_RESOURCE_DIR}/locales" "$<TARGET_FILE_DIR:${target}>/locales")
	foreach(_license LICENSE.txt README.txt)
		if(EXISTS "${CEF_ROOT}/${_license}")
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND "${CMAKE_COMMAND}" -E copy_if_different
					"${CEF_ROOT}/${_license}" "$<TARGET_FILE_DIR:${target}>/CEF-${_license}")
		endif()
	endforeach()
	# Use the staged runtime next to the executable and do not retain the
	# absolute CEF SDK directory in a portable build's dynamic section.
	set_target_properties(${target} PROPERTIES
		BUILD_WITH_INSTALL_RPATH TRUE
		BUILD_RPATH "$ORIGIN"
		INSTALL_RPATH "$ORIGIN")
endfunction()
