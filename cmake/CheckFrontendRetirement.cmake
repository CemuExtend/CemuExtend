if(NOT DEFINED SOURCE_ROOT)
	message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(READ "${SOURCE_ROOT}/CMakeLists.txt" root_cmake)
file(READ "${SOURCE_ROOT}/src/CMakeLists.txt" source_cmake)
file(READ "${SOURCE_ROOT}/vcpkg.json" manifest)

set(active_build_configuration "${root_cmake}\n${source_cmake}\n${manifest}")
foreach(forbidden IN ITEMS
		"ENABLE_WXWIDGETS"
		"find_package(wxWidgets"
		"CemuWxFrontend"
		"CemuWxGui"
		"wxWidgets::wxWidgets")
	string(FIND "${active_build_configuration}" "${forbidden}" forbidden_position)
	if(NOT forbidden_position EQUAL -1)
		message(FATAL_ERROR "Retired wxWidgets build configuration remains: ${forbidden}")
	endif()
endforeach()

if(manifest MATCHES "\\\"name\\\"[ \t\r\n]*:[ \t\r\n]*\\\"wxwidgets\\\"")
	message(FATAL_ERROR "The vcpkg manifest still depends on wxwidgets")
endif()

string(FIND "${root_cmake}" "set(CEMU_FRONTEND \"webview\"" webview_default_position)
if(webview_default_position EQUAL -1)
	message(FATAL_ERROR "The native desktop frontend must default to webview")
endif()
string(FIND "${root_cmake}" "^(webview|headless)$" frontend_choices_position)
if(frontend_choices_position EQUAL -1)
	message(FATAL_ERROR "Frontend selection must preserve exactly webview and headless")
endif()
