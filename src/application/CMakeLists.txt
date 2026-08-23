add_library(CemuApplication STATIC
	ApplicationHost.cpp
	ApplicationHost.h
	ApplicationPaths.cpp
	ApplicationPaths.h
	ApplicationRuntime.cpp
	ApplicationRuntime.h
	AccountFacade.h
	ContentOperations.h
	CemodManagerFacade.h
	DiagnosticsFacade.h
	DiagnosticFacade.cpp
	DiagnosticFacade.h
  ApplicationEvents.cpp
  ApplicationEvents.h
  CafeEmulationBackend.cpp
  EmulationController.cpp
  EmulationController.h
	EmulatedUsbFacade.cpp
	EmulatedUsbFacade.h
  EmulationPresentation.h
  FrontendSettingsFacade.h
	HotkeySettingsFacade.h
	HotkeySettingsFacade.cpp
	LoggingFacade.h
	LoggingFacade.cpp
  InputSettingsFacade.h
  MemorySearchFacade.h
  MemorySearchFacade.cpp
  PpcDebuggerFacade.h
  PpcDebuggerFacade.cpp
  GameProfileFacade.h
	GraphicPackFacade.h
	ManagedContentFacade.h
	NfcFacade.h
	SaveFacade.h
  TitleCatalog.h
  TitleInstallFacade.h
)

target_include_directories(CemuApplication PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(CemuApplication PRIVATE
	CemuAudio
	CemuCafe
	CemuCommon
	CemuConfig
	CemuInput
	CemuRuntimeOverlay
	CemuComponents
	CemuDownloadManager
	CemuUtil
	CURL::libcurl
	cemuextend::wire
	ZArchive::zarchive
	OpenSSL::Crypto
	libzip::zip
	pugixml::pugixml
)

if(ALLOW_PORTABLE)
	target_compile_definitions(CemuApplication PRIVATE CEMU_ALLOW_PORTABLE)
endif()
if(WIN32)
	target_link_libraries(CemuApplication PRIVATE shell32)
endif()

if(BUILD_TESTING)
  add_executable(emulation_controller_tests
	ApplicationEvents.cpp
    EmulationController.cpp
	HotkeySettingsFacade.cpp
    tests/CafeBackendFactoryStub.cpp
    tests/EmulationControllerTests.cpp
  )
  target_include_directories(emulation_controller_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
  add_test(NAME emulation_controller_tests COMMAND emulation_controller_tests)

  add_executable(application_events_tests tests/ApplicationEventsTests.cpp)
  target_link_libraries(application_events_tests PRIVATE CemuApplication)
  add_test(NAME application_events_tests COMMAND application_events_tests)

  add_executable(application_logging_facade_tests tests/LoggingFacadeTests.cpp)
  target_link_libraries(application_logging_facade_tests PRIVATE CemuApplication CemuCommon)
  add_test(NAME application_logging_facade_tests COMMAND application_logging_facade_tests)

  add_executable(emulated_usb_facade_tests tests/EmulatedUsbFacadeTests.cpp)
  target_link_libraries(emulated_usb_facade_tests PRIVATE CemuApplication)
  add_test(NAME emulated_usb_facade_tests COMMAND emulated_usb_facade_tests)

  add_executable(diagnostic_facade_tests tests/DiagnosticFacadeTests.cpp)
  target_link_libraries(diagnostic_facade_tests PRIVATE CemuApplication)
  add_test(NAME diagnostic_facade_tests COMMAND diagnostic_facade_tests)

  add_executable(memory_search_facade_tests tests/MemorySearchFacadeTests.cpp)
  target_link_libraries(memory_search_facade_tests PRIVATE CemuApplication)
  add_test(NAME memory_search_facade_tests COMMAND memory_search_facade_tests)

  add_executable(ppc_debugger_facade_tests tests/PpcDebuggerFacadeTests.cpp)
  target_link_libraries(ppc_debugger_facade_tests PRIVATE CemuApplication)
  add_test(NAME ppc_debugger_facade_tests COMMAND ppc_debugger_facade_tests)
endif()
