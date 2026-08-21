add_library(CemuApplication STATIC
	ApplicationHost.cpp
	ApplicationHost.h
	ApplicationRuntime.cpp
	ApplicationRuntime.h
	AccountFacade.h
	ContentOperations.h
  ApplicationEvents.cpp
  ApplicationEvents.h
  CafeEmulationBackend.cpp
  EmulationController.cpp
  EmulationController.h
  EmulationPresentation.h
  FrontendSettingsFacade.h
  HotkeySettingsFacade.h
  HotkeySettingsFacade.cpp
  InputSettingsFacade.h
  GameProfileFacade.h
  GraphicPackFacade.h
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
endif()
