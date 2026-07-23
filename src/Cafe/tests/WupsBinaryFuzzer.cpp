#include "Cafe/HW/Espresso/WupsBinary.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	std::string error;
	(void)WupsBinaryInspector::Inspect(
		std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size), error);
	return 0;
}
