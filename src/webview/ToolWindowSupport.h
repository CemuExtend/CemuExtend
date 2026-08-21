#pragma once

#include <functional>
#include <memory>

namespace WebFrontend
{
	class IToolWindowSupport
	{
	public:
		virtual ~IToolWindowSupport() = default;
		[[nodiscard]] virtual void* GetWindow() const = 0;
		virtual void Show() = 0;
		virtual void Focus() = 0;
	};

	std::unique_ptr<IToolWindowSupport> CreateToolWindowSupport(
		void* parent, bool modal, std::function<void()> closeHandler);
}
