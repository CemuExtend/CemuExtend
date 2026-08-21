#include "application/PpcDebuggerFacade.h"

#include <cassert>
#include <stdexcept>

namespace
{
	class FakeBackend final : public Application::IPpcDebuggerBackend
	{
	public:
		Application::PpcDebuggerBackendSnapshot Capture(Application::GuestAddress center,
			std::uint32_t count, std::uint32_t limit) override
		{
			assert(count <= 128 && limit == 256);
			return {true, trapped, {0x1000}, {}, 0x2000,
				{{center.value ? center : Application::GuestAddress{0x1000}, 0x60000000,
					"nop", "", true, true}},
				{{77, {0x1000}, true, false}}, false, ""};
		}
		void ToggleExecuteBreakpoint(Application::GuestAddress address) override
		{ toggled = address.value; }
		void SetBreakpointEnabled(std::uint64_t identity,
			Application::GuestAddress address, bool value) override
		{ assert(identity == 77 && address.value == 0x1000); enabled = value; }
		void DeleteBreakpoint(std::uint64_t identity,
			Application::GuestAddress address) override
		{ assert(identity == 77 && address.value == 0x1000); deleted = true; }
		void Control(Application::PpcDebuggerControl value) override { command = value; }

		bool trapped{true}; bool enabled{true}; bool deleted{};
		std::uint32_t toggled{};
		Application::PpcDebuggerControl command{Application::PpcDebuggerControl::Break};
	};

	template<typename Callback> bool Rejects(Callback callback)
	{
		try { callback(); } catch (const std::exception&) { return true; }
		return false;
	}
}

int main()
{
	using namespace Application;
	auto backend = std::make_unique<FakeBackend>();
	auto* fake = backend.get();
	PpcDebuggerFacade facade(std::move(backend));
	const auto first = facade.Capture(4, {0x1000}, 32);
	assert(first.available && first.trapped && first.instructions.size() == 1);
	assert(first.breakpoints.size() == 1 && !first.breakpoints.front().identity.empty());
	assert(Rejects([&] { facade.DeleteBreakpoint(5, first.generation,
		first.breakpoints.front().identity); }));
	assert(Rejects([&] { facade.ToggleExecuteBreakpoint(4, first.generation - 1, {0x1004}); }));

	facade.SetBreakpointEnabled(4, first.generation,
		first.breakpoints.front().identity, false);
	assert(!fake->enabled);
	assert(Rejects([&] { facade.SetBreakpointEnabled(4, first.generation,
		first.breakpoints.front().identity, true); }));

	const auto second = facade.Capture(4, {0x1000}, 16);
	facade.Control(4, second.generation, PpcDebuggerControl::StepInto);
	assert(fake->command == PpcDebuggerControl::StepInto);

	fake->trapped = false;
	const auto running = facade.Capture(4, {0x1000}, 16);
	assert(Rejects([&] { facade.Control(4, running.generation, PpcDebuggerControl::StepOver); }));
	facade.Control(4, running.generation, PpcDebuggerControl::Break);

	const auto beforeClose = facade.Capture(4, {0x1000}, 16);
	facade.CloseOwner(4);
	assert(Rejects([&] { facade.ToggleExecuteBreakpoint(4, beforeClose.generation, {0x1000}); }));
	assert(Rejects([&] { (void)facade.Capture(4, {0x1002}, 16); }));
	facade.BeginShutdown();
	assert(Rejects([&] { (void)facade.Capture(4, {0x1000}, 16); }));
}
