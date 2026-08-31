#pragma once

#include "Cemu/CemuExtend/Formats/CemodPackage.h"
#include "cemuextend/services.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cemuextend_hle
{
	struct CemodWebUiContent
	{
		std::string principal;
		std::string modId;
		std::uint64_t titleId{};
		CemodWebUi manifest;
		std::map<std::string, std::vector<std::byte>> assets;
	};

	struct CemodWebUiHostRequest
	{
		std::uint64_t addressSpaceId{};
		std::uint32_t generation{};
		std::uint32_t sessionId{};
		std::uint32_t correlationId{};
		cemuextend::wire::UiOperation operation{};
		std::vector<std::byte> payload;
		std::shared_ptr<const CemodWebUiContent> content;
	};

	struct CemodWebUiHostEvent
	{
		std::uint64_t addressSpaceId{};
		std::uint32_t generation{};
		std::uint32_t sessionId{};
		cemuextend::wire::UiEvent event{};
		std::vector<std::byte> payload;
	};

	class ICemodWebUiHost
	{
	  public:
		using Completion = std::function<void(cemuextend::wire::Status,
											  std::vector<std::byte>)>;
		using EventSink = std::function<void(CemodWebUiHostEvent)>;

		virtual ~ICemodWebUiHost() = default;
		virtual void SetEventSink(EventSink sink) = 0;
		[[nodiscard]] virtual bool Submit(CemodWebUiHostRequest request,
										  Completion completion) = 0;
		virtual void Cancel(std::uint64_t addressSpaceId, std::uint32_t generation,
							std::uint32_t sessionId, std::uint32_t correlationId) = 0;
		virtual void CloseSession(std::uint64_t addressSpaceId, std::uint32_t generation,
								  std::uint32_t sessionId) = 0;
		virtual void CloseOwner(std::uint64_t addressSpaceId,
								std::uint32_t generation) = 0;
		virtual void CloseAll() = 0;
	};
} // namespace cemuextend_hle
