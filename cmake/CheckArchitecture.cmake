file(GLOB_RECURSE architecture_sources
	"${SOURCE_ROOT}/src/*.h"
	"${SOURCE_ROOT}/src/*.hpp"
	"${SOURCE_ROOT}/src/*.cpp"
	"${SOURCE_ROOT}/src/CMakeLists.txt")

set(locator_violations "")
set(lower_gui_violations "")
set(cafe_ui_violations "")
set(title_ui_violations "")
set(graphic_pack_ui_violations "")

foreach(source IN LISTS architecture_sources)
	file(READ "${source}" content)
	if(content MATCHES "Frontend::GetHostServices|frontend/HostServices\\.h")
		list(APPEND locator_violations "${source}")
	endif()

	if(source MATCHES "/src/(Cafe|Cemu|input|audio|imgui|config)/")
		if(content MATCHES "gui/interface/WindowSystem\\.h|interface/WindowSystem\\.h|WindowSystem::")
			list(APPEND lower_gui_violations "${source}")
		endif()
	endif()

	if(source MATCHES "/src/Cafe/" AND content MATCHES "#include[ \t]*[<\"]wx/|#include[ \t]*[<\"]wx")
		list(APPEND cafe_ui_violations "${source}")
	endif()

	# These normal frontend files have moved behind Application::ITitleCatalog.
	# Prevent accidental reintroduction while the larger game-list/title-manager
	# migration proceeds through its own copied DTO/event boundary.
	if(source MATCHES "/src/gui/wxgui/(MainWindow|CemuApp|GeneralSettings2|GettingStartedDialog)\\.(h|cpp)$")
		if(content MATCHES "#include[ \t]*[<\"]Cafe/TitleList/|CafeTitleList::|CafeSaveList::|TitleInfo|GameInfo2")
			list(APPEND title_ui_violations "${source}")
		endif()
	endif()

	# Graphic-pack windows consume copied Application DTOs and commands only.
	if(source MATCHES "/src/gui/wxgui/(GraphicPacksWindow2|DownloadGraphicPacksWindow|DownloadCustomGraphicPackWindow)\\.(h|cpp)$")
		if(content MATCHES "#include[ \t]*[<\"]Cafe/|GraphicPack2|CafeSystem::|CafeTitleList::|LatteAsyncCommands")
			list(APPEND graphic_pack_ui_violations "${source}")
		endif()
	endif()
endforeach()

if(locator_violations)
	list(JOIN locator_violations "\n  " formatted)
	message(FATAL_ERROR "Global HostServices locator is forbidden:\n  ${formatted}")
endif()
if(lower_gui_violations)
	list(JOIN lower_gui_violations "\n  " formatted)
	message(FATAL_ERROR "Lower-layer WindowSystem dependency is forbidden:\n  ${formatted}")
endif()
if(cafe_ui_violations)
	list(JOIN cafe_ui_violations "\n  " formatted)
	message(FATAL_ERROR "Cafe must not include wx headers:\n  ${formatted}")
endif()
if(title_ui_violations)
	list(JOIN title_ui_violations "\n  " formatted)
	message(FATAL_ERROR "Normal wx frontend must use the Application title catalog:\n  ${formatted}")
endif()
if(graphic_pack_ui_violations)
	list(JOIN graphic_pack_ui_violations "\n  " formatted)
	message(FATAL_ERROR "Normal wx frontend must use the Application graphic-pack facade:\n  ${formatted}")
endif()
