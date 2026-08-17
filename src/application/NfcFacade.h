#pragma once

#include <cstdint>

namespace Application
{
	enum class NfcTouchResult : std::uint8_t
	{
		Success,
		Inactive,
		NoAccess,
		InvalidFileFormat,
		UnknownError,
	};
}
