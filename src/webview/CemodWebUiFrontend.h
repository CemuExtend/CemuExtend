#pragma once

#include "Cafe/OS/libs/cemuextend/CemodWebUiHost.h"
#include "webview/cef/CefOverlayRuntime.h"

#include <functional>
#include <memory>

namespace WebFrontend
{
	class CemodWebUiFrontend final : public cemuextend_hle::ICemodWebUiHost,
									 public std::enable_shared_from_this<CemodWebUiFrontend>
	{
	  public:
		using PostTask = std::function<bool(std::function<void()>)>;

		static std::shared_ptr<CemodWebUiFrontend> Create(void* parent,
														  std::shared_ptr<CefOverlay::BrowserRuntime> browsers, PostTask postTask);
		~CemodWebUiFrontend() override;

		void SetEventSink(EventSink sink) override;
		[[nodiscard]] bool Submit(cemuextend_hle::CemodWebUiHostRequest request,
								  Completion completion) override;
		void Cancel(std::uint64_t addressSpaceId, std::uint32_t generation,
					std::uint32_t sessionId, std::uint32_t correlationId) override;
		void CloseSession(std::uint64_t addressSpaceId, std::uint32_t generation,
						  std::uint32_t sessionId) override;
		void CloseOwner(std::uint64_t addressSpaceId, std::uint32_t generation) override;
		void CloseAll() override;
		void BeginShutdown();
		// Frontend UI thread only. Used during ordered CEF shutdown.
		void Shutdown();

	  private:
		struct Impl;
		explicit CemodWebUiFrontend(std::unique_ptr<Impl> impl);
		std::unique_ptr<Impl> m_impl;
	};
} // namespace WebFrontend
