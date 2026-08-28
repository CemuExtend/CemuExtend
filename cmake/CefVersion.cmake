# Keep the CEF version and archive hashes in one place so local fetches and CI
# consume exactly the same SDK on every supported host.
set(CEMU_CEF_VERSION "151.3.24+g2384915+chromium-151.0.7922.174")

set(CEMU_CEF_SHA256_linux64 "995fb57fe6b9af9ea184a983bc878cea9cc5895d3ed071065a1d4ad78d0ea3af")
set(CEMU_CEF_SHA256_linuxarm64 "6e2b8a52b165c8c4cdab9f6857d92c5d54bd3182915a16d0ddd015d470799849")
set(CEMU_CEF_SHA256_windows64 "f3164754dd14d89c5596a7b23b8bce137dd500eada9d4cf0b450b6df98e022aa")
set(CEMU_CEF_SHA256_macosx64 "708ea84b99343846ce3917687d141e86705866535d4b4738872490bf6717c653")
set(CEMU_CEF_SHA256_macosarm64 "63cdfe20e648be9a9edb33b9c61557d564fd3b357f9b17d003ce04ac6511d1bb")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
		set(CEMU_CEF_PLATFORM "linux64")
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
		set(CEMU_CEF_PLATFORM "linuxarm64")
	endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND
		NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND
		NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
		set(CEMU_CEF_PLATFORM "windows64")
	endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	set(_cemu_cef_macos_arch "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT _cemu_cef_macos_arch)
		set(_cemu_cef_macos_arch "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	if(_cemu_cef_macos_arch MATCHES "^(x86_64|amd64|AMD64)$")
		set(CEMU_CEF_PLATFORM "macosx64")
	elseif(_cemu_cef_macos_arch MATCHES "^(aarch64|arm64|ARM64)$")
		set(CEMU_CEF_PLATFORM "macosarm64")
	endif()
endif()

if(NOT CEMU_CEF_PLATFORM)
	message(FATAL_ERROR
		"CEF ${CEMU_CEF_VERSION} is not configured for "
		"${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR} "
		"(CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}')")
endif()

set(_cemu_cef_sha256_variable "CEMU_CEF_SHA256_${CEMU_CEF_PLATFORM}")
set(CEMU_CEF_SHA256 "${${_cemu_cef_sha256_variable}}")
set(CEMU_CEF_ARCHIVE "cef_binary_${CEMU_CEF_VERSION}_${CEMU_CEF_PLATFORM}.tar.bz2")
set(CEMU_CEF_URL "https://cef-builds.spotifycdn.com/${CEMU_CEF_ARCHIVE}")

unset(_cemu_cef_macos_arch)
unset(_cemu_cef_sha256_variable)
