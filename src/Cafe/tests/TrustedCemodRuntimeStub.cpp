#include "Cafe/HW/Espresso/TrustedCemodRuntime.h"
#include "Cafe/HW/Espresso/TrustedCemodLifecycle.h"

struct TrustedCemodRuntime::Impl
{
	TrustedCemodLifecycle lifecycle;
};

TrustedCemodRuntime::TrustedCemodRuntime() : m_impl(std::make_unique<Impl>()) {}
TrustedCemodRuntime::~TrustedCemodRuntime() = default;

std::optional<std::uint64_t> TrustedCemodRuntime::Load(CemodPackage,
													   std::uint32_t, const ModServicePermissions&, std::string& error)
{
	error = "trusted runtime is not linked into isolated runtime unit tests";
	return std::nullopt;
}

bool TrustedCemodRuntime::BeginTitle(std::uint64_t titleId, std::string& error)
{
	return m_impl->lifecycle.Begin(titleId, error);
}
bool TrustedCemodRuntime::ReadyForNextTitle(std::string& error) const
{
	if (m_impl->lifecycle.IsReady())
		return true;
	error = m_impl->lifecycle.ReleasePending() ? "trusted title release is still pending" : "trusted title is still active";
	return false;
}
bool TrustedCemodRuntime::Unload(std::uint64_t)
{
	return false;
}
void TrustedCemodRuntime::UnloadAll()
{
	m_impl->lifecycle.RequestRelease();
}
bool TrustedCemodRuntime::MarkTitleThreadsStopped(std::string& error)
{
	return m_impl->lifecycle.MarkThreadsStopped(error);
}
bool TrustedCemodRuntime::ReleaseAfterTitleThreadsStopped(std::string& error)
{
	return m_impl->lifecycle.CompleteRelease(error);
}
void TrustedCemodRuntime::UpdatePermissions(std::uint32_t, const ModServicePermissions&) {}
cemuextend_hle::Cex2Owner* TrustedCemodRuntime::Owner()
{
	return nullptr;
}
std::size_t TrustedCemodRuntime::Size() const
{
	return m_impl->lifecycle.IsReady() ? 0 : 1;
}
bool TrustedCemodRuntime::TitleShutdownPrepared() const
{
	return m_impl->lifecycle.IsReady() || m_impl->lifecycle.ReleasePending();
}
bool TrustedCemodRuntime::ReleasePending() const
{
	return m_impl->lifecycle.ReleasePending();
}
