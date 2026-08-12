#include "Cafe/HW/Espresso/TrustedCemodLifecycle.h"

#include <cstdlib>
#include <iostream>

namespace
{
	[[noreturn]] void CheckFailed(const char* expression, int line)
	{
		std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
		std::abort();
	}

#define CHECK(condition)                       \
	do                                         \
	{                                          \
		if (!(condition))                      \
			CheckFailed(#condition, __LINE__); \
	}                                          \
	while (false)

	void TestLateReleaseGate()
	{
		TrustedCemodLifecycle lifecycle;
		std::string error;
		constexpr std::uint64_t firstTitle = 0x0005000011111111ULL;
		constexpr std::uint64_t secondTitle = 0x0005000022222222ULL;

		CHECK(lifecycle.IsReady());
		CHECK(!lifecycle.Begin(0, error));
		CHECK(lifecycle.Begin(firstTitle, error));
		CHECK(lifecycle.Accepts(firstTitle));
		CHECK(!lifecycle.Accepts(secondTitle));
		CHECK(!lifecycle.Begin(secondTitle, error));
		CHECK(!lifecycle.MarkThreadsStopped(error));
		CHECK(!lifecycle.CompleteRelease(error));

		lifecycle.RequestRelease();
		CHECK(lifecycle.ReleasePending());
		CHECK(!lifecycle.Accepts(firstTitle));
		CHECK(!lifecycle.Begin(secondTitle, error));
		CHECK(!lifecycle.CompleteRelease(error));

		// RPLUnmapped, rpl_entry and explicit shutdown may all repeat the request.
		lifecycle.RequestRelease();
		CHECK(lifecycle.ReleasePending());
		CHECK(lifecycle.MarkThreadsStopped(error));
		CHECK(lifecycle.MarkThreadsStopped(error));
		CHECK(lifecycle.CompleteRelease(error));
		CHECK(lifecycle.IsReady());
		CHECK(lifecycle.CompleteRelease(error));
		CHECK(lifecycle.Begin(secondTitle, error));
	}
} // namespace

int main()
{
	TestLateReleaseGate();
	return 0;
}
