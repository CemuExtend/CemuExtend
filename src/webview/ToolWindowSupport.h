#pragma once

#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace WebFrontend
{
	class IToolWindowSupport
	{
	  public:
		virtual ~IToolWindowSupport() = default;
		[[nodiscard]] virtual void* GetWindow() const = 0;
		virtual void Show() = 0;
		virtual void Focus() = 0;
		[[nodiscard]] virtual std::optional<std::filesystem::path> PickDirectory(
			std::string_view title) = 0;
	};

	std::unique_ptr<IToolWindowSupport> CreateToolWindowSupport(
		void* parent, bool modal, std::function<void()> closeHandler);
} // namespace WebFrontend
