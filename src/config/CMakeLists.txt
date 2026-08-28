add_library(CemuConfig
    ActiveSettings.h
    CemuConfig.cpp
    CemuConfig.h
    ConfigValue.h
    LaunchSettings.h
    NetworkSettings.cpp
    NetworkSettings.h
    XMLConfig.h
)

set_property(TARGET CemuConfig PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

cemu_use_precompiled_header(CemuConfig)

target_include_directories(CemuConfig PUBLIC "../")

target_link_libraries(CemuConfig PRIVATE
	CemuCommon
	CemuComponents
)

if(BUILD_TESTING)
	add_executable(frontend_settings_config_tests tests/FrontendSettingsConfigTests.cpp)
	target_link_libraries(frontend_settings_config_tests PRIVATE CemuConfig CemuCommon)
	add_test(NAME frontend_settings_config_tests COMMAND frontend_settings_config_tests)
endif()
