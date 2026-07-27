#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

class AromaCompatibilityRuntime;
namespace cemuextend_hle { class Cex2Owner; }

// Thin decoupling bridge between coreinit's OSDynLoad_Acquire/FindExport HLE
// and the active title's WUPS runtime. A plugin's statically declared RPL
// imports of "homebrew_*" modules are already resolved at load time by
// AromaCompatibilityRuntime::ResolveImport, but some libwups helper
// libraries (notably the config API) instead resolve their backend
// dynamically at runtime via OSDynLoad_Acquire("homebrew_wupsbackend") +
// OSDynLoad_FindExport(). Without this bridge that Acquire call reports
// module-not-found and the plugin dereferences a garbage function pointer.
// coreinit_DynLoad calls the Try* hooks unconditionally; they only succeed
// while the calling guest thread is executing inside a WupsGuestOwnerScope
// (i.e. plugin code invoked by the runtime) and a runtime has been
// installed via SetActiveRuntime.
namespace cafe::wups
{
// Installed by CreateRplAromaCompatibilityRuntime() for the lifetime of a
// title's WUPS runtime instance; pass an empty weak_ptr on teardown.
void SetActiveRuntime(std::weak_ptr<AromaCompatibilityRuntime> runtime);

std::shared_ptr<cemuextend_hle::Cex2Owner> ResolveCurrentCex2Owner();

// Mirrors OSDynLoad_Acquire(): if moduleName is a "homebrew_*" virtual
// module reachable by the WUPS plugin currently executing, returns true and
// fills handleOut with a synthetic module handle (recognisable via
// IsHomebrewModuleHandle). Returns false for anything else, so the caller
// falls back to the normal RPL module search.
bool TryAcquireHomebrewModule(std::string_view moduleName,
	std::uint32_t& handleOut);

// True if handle was previously returned by TryAcquireHomebrewModule.
bool IsHomebrewModuleHandle(std::uint32_t handle);

// Drops a single requester-bound virtual handle. Stale handles remain in the
// reserved range and are rejected instead of falling through to the RPL loader.
void ReleaseHomebrewModule(std::uint32_t handle);

// Mirrors OSDynLoad_FindExport() for a handle obtained from
// TryAcquireHomebrewModule.
bool TryFindHomebrewExport(std::uint32_t handle, bool isData,
	std::string_view exportName, std::uint32_t& addressOut);
} // namespace cafe::wups
