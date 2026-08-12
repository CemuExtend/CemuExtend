#pragma once

#include "Cafe/HW/Espresso/CemodPackage.h"
#include "Cafe/HW/Espresso/ModExecutionContext.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace cemuextend_hle { class Cex2Owner; }

class TrustedCemodRuntime
{
public:
	TrustedCemodRuntime();
	~TrustedCemodRuntime();

	[[nodiscard]] std::optional<std::uint64_t> Load(CemodPackage package,
		std::uint32_t titlePermissions, const ModServicePermissions& services,
		std::string& error);
	// Starts the trusted lifetime for a title. A retained image from an earlier
	// title is a hard error because its patch target belongs to the old RPL map.
	[[nodiscard]] bool BeginTitle(std::uint64_t titleId, std::string& error);
	[[nodiscard]] bool ReadyForNextTitle(std::string& error) const;

	// Individual trusted images cannot be detached while title PPC threads may
	// still execute them, so live-title unload is intentionally rejected.
	[[nodiscard]] bool Unload(std::uint64_t handle);
	// Revokes the shared owner and requests a deferred title-wide release. It does
	// not restore the bootstrap branch or free codecave memory.
	void UnloadAll();
	[[nodiscard]] bool MarkTitleThreadsStopped(std::string& error);
	// The sole operation that restores bootstrap instructions and releases the
	// codecave. The caller must invoke it after all title PPC threads are deleted.
	[[nodiscard]] bool ReleaseAfterTitleThreadsStopped(std::string& error);
	void UpdatePermissions(std::uint32_t permissions, const ModServicePermissions& services);

	[[nodiscard]] cemuextend_hle::Cex2Owner* Owner();
	[[nodiscard]] std::size_t Size() const;
	[[nodiscard]] bool TitleShutdownPrepared() const;
	[[nodiscard]] bool ReleasePending() const;

  private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
