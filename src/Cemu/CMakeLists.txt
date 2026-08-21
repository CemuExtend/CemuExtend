add_library(CemuExtendDomain STATIC
  CemuExtend/CemodInspectionService.cpp
  CemuExtend/CemodInspectionService.h
  CemuExtend/Formats/CemodPackage.cpp
  CemuExtend/Formats/CemodPackage.h
	CemuExtend/Formats/ExternalModuleMappingPolicy.h
  CemuExtend/Formats/WupsBinary.cpp
  CemuExtend/Formats/WupsBinary.h
)

target_include_directories(CemuExtendDomain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(CemuExtendDomain PUBLIC OpenSSL::Crypto libzip::zip ZLIB::ZLIB fmt::fmt)

if(BUILD_TESTING)
  add_executable(cemod_inspection_service_tests
    CemuExtend/tests/CemodInspectionServiceTests.cpp
  )
  target_link_libraries(cemod_inspection_service_tests PRIVATE CemuExtendDomain)
  add_test(NAME cemod_inspection_service_tests COMMAND cemod_inspection_service_tests)
endif()

add_library(CemuComponents
  ExpressionParser/ExpressionParser.cpp
  ExpressionParser/ExpressionParser.h
  FileCache/FileCache.cpp
  FileCache/FileCache.h
  Logging/CemuDebugLogging.h
  Logging/CemuLogging.cpp
  Logging/CemuLogging.h
)

add_library(CemuDownloadManager STATIC
  Tools/DownloadManager/DownloadManager.cpp
  Tools/DownloadManager/DownloadManager.h
)

if(ENABLE_DISCORD_RPC)
target_sources(CemuComponents PRIVATE
		DiscordPresence/DiscordPresence.cpp
		DiscordPresence/DiscordPresence.h
		DiscordPresence/DiscordRPCLite.cpp
		DiscordPresence/DiscordRPCLite.h)
endif()

set_property(TARGET CemuComponents PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(TARGET CemuDownloadManager PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

cemu_use_precompiled_header(CemuComponents)
cemu_use_precompiled_header(CemuDownloadManager)

target_include_directories(CemuComponents PUBLIC "../")
target_include_directories(CemuDownloadManager PUBLIC "../")

target_link_libraries(CemuComponents PRIVATE
  CemuCommon
  CemuHostContracts
  CemuUtil
  ZLIB::ZLIB
)

target_link_libraries(CemuDownloadManager PRIVATE
  CemuCafe
  CemuCommon
  CemuComponents
  CemuConfig
  CemuHostContracts
  CemuUtil
)

# PUBLIC because fmt/format.h is included in ExpressionParser/ExpressionParser.h
target_link_libraries(CemuComponents PUBLIC fmt::fmt)
