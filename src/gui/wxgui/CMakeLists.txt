add_library(CemuWxGui STATIC
  canvas/IRenderCanvas.h
  CemuApp.cpp
  CemuApp.h
  CemuUpdateWindow.cpp
  CemuUpdateWindow.h
#  CemuExtend GUI models and WUPS/Aroma management UI
  CemodManagementModel.cpp
  CemodManagementModel.h
  CemodPluginManagerDialog.cpp
  CemodPluginManagerDialog.h
  CemodPermissionDialog.cpp
  CemodPermissionDialog.h
  ChecksumTool.cpp
  ChecksumTool.h
  components/wxDownloadManagerList.cpp
  components/wxDownloadManagerList.h
  components/wxGameList.cpp
  components/wxGameList.h
  components/wxInputDraw.cpp
  components/wxInputDraw.h
  components/wxLogCtrl.cpp
  components/wxLogCtrl.h
  components/wxTitleManagerList.cpp
  components/wxTitleManagerList.h
  dialogs/CreateAccount/wxCreateAccountDialog.cpp
  dialogs/CreateAccount/wxCreateAccountDialog.h
  dialogs/SaveImport/SaveImportWindow.cpp
  dialogs/SaveImport/SaveImportWindow.h
  dialogs/SaveImport/SaveTransfer.cpp
  dialogs/SaveImport/SaveTransfer.h
  DownloadGraphicPacksWindow.cpp
  DownloadGraphicPacksWindow.h
  DownloadCustomGraphicPackWindow.cpp
  DownloadCustomGraphicPackWindow.h
  GameProfileWindow.cpp
  GameProfileWindow.h
  GameUpdateWindow.cpp
  GameUpdateWindow.h
  GeneralSettings2.cpp
  GeneralSettings2.h
  GettingStartedDialog.cpp
  GettingStartedDialog.h
  GraphicPacksWindow2.cpp
  GraphicPacksWindow2.h
  WxFrontendContext.h
  WxFrontendRuntime.cpp
  WxWindowState.cpp
  WxWindowState.h
  helpers/wxControlObject.h
  helpers/wxCustomData.h
  helpers/wxHelpers.cpp
  helpers/wxHelpers.h
  helpers/wxLogEvent.h
  helpers/wxWayland.cpp
  helpers/wxWayland.h
  input/HotkeySettings.cpp
  input/HotkeySettings.h
  input/InputAPIAddWindow.cpp
  input/InputAPIAddWindow.h
  input/InputSettings2.cpp
  input/InputSettings2.h
  input/panels/ClassicControllerInputPanel.cpp
  input/panels/ClassicControllerInputPanel.h
  input/panels/InputPanel.cpp
  input/panels/InputPanel.h
  input/panels/ProControllerInputPanel.cpp
  input/panels/ProControllerInputPanel.h
  input/panels/VPADInputPanel.cpp
  input/panels/VPADInputPanel.h
  input/panels/WiimoteInputPanel.cpp
  input/panels/WiimoteInputPanel.h
  input/settings/DefaultControllerSettings.cpp
  input/settings/DefaultControllerSettings.h
  input/settings/WiimoteControllerSettings.cpp
  input/settings/WiimoteControllerSettings.h
  LoggingWindow.cpp
  LoggingWindow.h
  MainWindow.cpp
  MainWindow.h
  PadViewFrame.cpp
  PadViewFrame.h
  TitleManager.cpp
  TitleManager.h
  wxCemuConfig.cpp
  wxCemuConfig.h
  wxcomponents/checktree.cpp
  wxcomponents/checktree.h
  wxgui.h
  wxHelper.h
)

add_library(CemuWxGuiCommon STATIC
  components/TextList.cpp
  components/TextList.h
  helpers/wxCustomEvents.cpp
  helpers/wxCustomEvents.h
)

add_library(CemuWxRendererAdapters STATIC
  canvas/IRenderCanvas.h
  canvas/RendererWindowAdapter.cpp
  canvas/RendererWindowAdapter.h
)

# Debugger windows that intentionally inspect emulator internals live outside
# the normal GUI archive.  The normal-facing adapter facade stays core-neutral
# while the executable composition root supplies the implementation archive.
add_library(CemuWxDebuggerAdapters STATIC
  AudioDebuggerWindow.cpp
  AudioDebuggerWindow.h
  debugger/BreakpointWindow.cpp
  debugger/BreakpointWindow.h
  debugger/DebuggerEvents.cpp
  debugger/DebuggerEvents.h
  debugger/DebuggerWindow2.cpp
  debugger/DebuggerWindow2.h
  debugger/DebuggerWindowAdapter.cpp
  debugger/DebuggerWindowAdapter.h
  debugger/DisasmCtrl.cpp
  debugger/DisasmCtrl.h
  debugger/DumpCtrl.cpp
  debugger/DumpCtrl.h
  debugger/DumpWindow.cpp
  debugger/DumpWindow.h
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

add_library(CemuWxDeviceAdapters STATIC
  EmulatedUSBDevices/EmulatedUSBDeviceAdapter.cpp
  EmulatedUSBDevices/EmulatedUSBDeviceAdapter.h
  EmulatedUSBDevices/EmulatedUSBDeviceFrame.cpp
  EmulatedUSBDevices/EmulatedUSBDeviceFrame.h
)

set_property(TARGET CemuWxGuiCommon PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
cemu_use_precompiled_header(CemuWxGuiCommon)
target_include_directories(CemuWxGuiCommon PUBLIC "../")
target_link_libraries(CemuWxGuiCommon PRIVATE
  CemuCommon
  PUBLIC wxWidgets::wxWidgets
)

if (ENABLE_OPENGL)
  target_sources(CemuWxRendererAdapters PRIVATE
    canvas/OpenGLCanvas.cpp
    canvas/OpenGLCanvas.h
  )
endif()

if (ENABLE_VULKAN)
  target_sources(CemuWxRendererAdapters PRIVATE
    canvas/VulkanCanvas.cpp
    canvas/VulkanCanvas.h
  )
endif()

if (ENABLE_METAL)
  target_sources(CemuWxRendererAdapters PRIVATE
    canvas/MetalCanvas.cpp
    canvas/MetalCanvas.h
  )
endif()

set_property(TARGET CemuWxRendererAdapters PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
cemu_use_precompiled_header(CemuWxRendererAdapters)
target_include_directories(CemuWxRendererAdapters PUBLIC "../")
target_link_libraries(CemuWxRendererAdapters PRIVATE
  CemuCafe
  CemuCommon
  CemuHostContracts
  CemuConfig
  PUBLIC wxWidgets::wxWidgets
)

set_property(TARGET CemuWxDebuggerAdapters PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
cemu_use_precompiled_header(CemuWxDebuggerAdapters)
target_include_directories(CemuWxDebuggerAdapters PUBLIC "../")
target_link_libraries(CemuWxDebuggerAdapters PRIVATE
  CemuCafe
  CemuCommon
  CemuComponents
  CemuConfig
  CemuResource
  CemuUtil
  CemuWxGuiCommon
  PUBLIC wxWidgets::wxWidgets
)

set_property(TARGET CemuWxDeviceAdapters PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
cemu_use_precompiled_header(CemuWxDeviceAdapters)
target_include_directories(CemuWxDeviceAdapters PUBLIC "../")
target_link_libraries(CemuWxDeviceAdapters PRIVATE
  CemuCafe
  CemuCommon
  CemuComponents
  CemuConfig
  CemuResource
  PUBLIC wxWidgets::wxWidgets
)

if(ENABLE_WXWIDGETS AND UNIX AND NOT APPLE)
  # VulkanCanvas includes the native GTK/Wayland adapter directly.  Keep these
  # platform requirements on the renderer-adapter target instead of inheriting
  # them accidentally from the normal GUI archive.
  target_link_libraries(CemuWxRendererAdapters PRIVATE GTK3::gtk)
  if (ENABLE_WAYLAND)
    target_link_libraries(CemuWxRendererAdapters PRIVATE Wayland::Client CemuWaylandProtocols)
  endif()
endif()

if (ENABLE_BLUEZ)
  target_sources(CemuWxGui PRIVATE
    input/PairingDialog.cpp
    input/PairingDialog.h
  )
endif()

set_property(TARGET CemuWxGui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

cemu_use_precompiled_header(CemuWxGui)

if(BUILD_TESTING)
  add_executable(cemod_gui_model_tests
    CemodManagementModel.cpp
    tests/CemodManagementModelTests.cpp
  )
  target_include_directories(cemod_gui_model_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(cemod_gui_model_tests PRIVATE CemuExtendDomain fmt::fmt)
  add_test(NAME cemod_gui_model_tests COMMAND cemod_gui_model_tests)
endif()


target_include_directories(CemuWxGui PUBLIC "../")
# PUBLIC because rapidjson/document.h is included in ChecksumTool.h
target_include_directories(CemuWxGui PUBLIC ${RAPIDJSON_INCLUDE_DIRS})

target_link_libraries(CemuWxGui PRIVATE
  CemuAudio
  CemuCommon
  CemuConfig
  CemuWxGuiCommon
  CemuApplication
  CemuExtendDomain
  CemuFrontendModel
  CemuHostContracts
  CemuInput
  CemuResource
  CemuUtil
  libzip::zip
  CemuComponents
  PUBLIC
  CURL::libcurl
)

if(ENABLE_WXWIDGETS AND UNIX AND NOT APPLE)
	# PUBLIC because gdk/gdkkeysyms.h is included in guiWrapper.h
	target_link_libraries(CemuWxGui PUBLIC GTK3::gtk)
  if (ENABLE_WAYLAND)
    target_link_libraries(CemuWxGui PRIVATE Wayland::Client CemuWaylandProtocols)
  endif()
endif()

if(ENABLE_CUBEB)
	target_link_libraries(CemuWxGui PRIVATE cubeb::cubeb)
endif()

if(ENABLE_SDL)
	target_link_libraries(CemuWxGui PRIVATE SDL3::SDL3)
endif()

if(UNIX AND NOT APPLE)
	if(ENABLE_FERAL_GAMEMODE)
		target_link_libraries(CemuWxGui PRIVATE gamemode)
	endif()
endif()
if (ENABLE_WXWIDGETS)
	# PUBLIC because wx/app.h is included in CemuApp.h
	target_link_libraries(CemuWxGui PUBLIC wxWidgets::wxWidgets)
endif()

if(WIN32)
	target_link_libraries(CemuWxGui PRIVATE bthprops)
endif()

if(ALLOW_PORTABLE)
	target_compile_definitions(CemuWxGui PRIVATE CEMU_ALLOW_PORTABLE)
endif ()
