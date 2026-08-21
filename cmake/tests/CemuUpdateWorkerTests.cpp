#include "gui/wxgui/CemuUpdateWorker.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <future>
#include <thread>

int main()
{
	using Work = CemuUpdateWorkerMailbox::Work;
	CemuUpdateWorkerMailbox mailbox(Work::CheckVersion);
	assert(mailbox.Wait() == Work::CheckVersion);

	std::promise<void> workerBusy;
	std::promise<void> finishCurrentWork;
	auto finish = finishCurrentWork.get_future();
	std::promise<void> workerExited;
	auto exited = workerExited.get_future();
	std::thread worker([&] {
		workerBusy.set_value();
		finish.wait();
		assert(!mailbox.Wait().has_value());
		workerExited.set_value();
	});

	workerBusy.get_future().wait();
	mailbox.RequestStop();
	assert(mailbox.StopRequested());
	assert(!mailbox.Request(Work::UpdateVersion));
	finishCurrentWork.set_value();
	assert(exited.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
	worker.join();
}
