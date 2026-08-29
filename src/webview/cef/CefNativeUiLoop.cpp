#include "CefNativeUiLoop.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <glib.h>
#include <gtk/gtk.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#else
#error CefNativeUiLoop is only implemented for Linux, Windows, and macOS
#endif

namespace WebFrontend::CefNative
{
	namespace
	{
		std::mutex s_stateMutex;
		std::thread::id s_uiThread;
		std::atomic_bool s_initialized{};
		std::atomic_bool s_acceptingTasks{};
		std::atomic_uint64_t s_pumpGeneration{};
		std::atomic_uint64_t s_lifecycleGeneration{};
		constexpr auto kMaximumPumpDelay = std::chrono::milliseconds(1000 / 30);
		bool s_pumpActive{};
		bool s_reentrantPumpDetected{};

		void QueuePumpRequest(std::chrono::milliseconds delay, std::uint64_t generation,
			NativeUiTask callback);
		void InvokePumpAndArmWatchdog(std::uint64_t generation, NativeUiTask callback);

		void InvokeSafely(NativeUiTask& task) noexcept
		{
			try
			{
				if (task)
					task();
			}
			catch (...)
			{
				// Never allow an application callback to unwind through a C platform API.
			}
		}

#if defined(__linux__)
		guint s_pumpSource{};
		struct PostedTask
		{
			std::uint64_t generation{};
			NativeUiTask callback;
		};

		gboolean InvokePostedTask(gpointer data)
		{
			auto* task = static_cast<PostedTask*>(data);
			if (s_acceptingTasks.load(std::memory_order_acquire) &&
				task->generation == s_lifecycleGeneration.load(std::memory_order_acquire))
			{
				InvokeSafely(task->callback);
			}
			return G_SOURCE_REMOVE;
		}

		void DestroyPostedTask(gpointer data)
		{
			delete static_cast<PostedTask*>(data);
		}

		struct ScheduledPump
		{
			std::uint64_t generation{};
			NativeUiTask callback;
		};

		gboolean InvokeScheduledPump(gpointer data)
		{
			auto* scheduled = static_cast<ScheduledPump*>(data);
			s_pumpSource = 0;
			InvokePumpAndArmWatchdog(scheduled->generation, std::move(scheduled->callback));
			return G_SOURCE_REMOVE;
		}

		void DestroyScheduledPump(gpointer data)
		{
			delete static_cast<ScheduledPump*>(data);
		}

#elif defined(_WIN32)
		constexpr UINT kPostedTaskMessage = WM_APP + 0x43;
		constexpr UINT kQuitLoopMessage = WM_APP + 0x44;
		constexpr UINT_PTR kPumpTimer = 1;
		constexpr wchar_t kWindowClassName[] = L"CemuExtend.CefNativeUiLoop";
		HWND s_messageWindow{};
		DWORD s_uiThreadId{};
		HINSTANCE s_module{};
		bool s_registeredWindowClass{};
		NativeUiTask s_scheduledPump;

		LRESULT CALLBACK NativeUiWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
		{
			switch (message)
			{
			case kPostedTaskMessage:
			{
				std::unique_ptr<NativeUiTask> task(reinterpret_cast<NativeUiTask*>(lParam));
				InvokeSafely(*task);
				return 0;
			}
			case kQuitLoopMessage:
				PostQuitMessage(0);
				return 0;
			case WM_TIMER:
				if (wParam == kPumpTimer)
				{
					KillTimer(window, kPumpTimer);
					auto callback = std::move(s_scheduledPump);
					InvokeSafely(callback);
					return 0;
				}
				break;
			default:
				break;
			}
			return DefWindowProcW(window, message, wParam, lParam);
		}

#elif defined(__APPLE__)
		CFRunLoopRef s_runLoop{};
		CFRunLoopSourceRef s_taskSource{};
		CFRunLoopTimerRef s_pumpTimer{};
		std::mutex s_taskMutex;
		std::vector<NativeUiTask> s_tasks;

		void DrainPostedTasks(void*)
		{
			std::vector<NativeUiTask> tasks;
			{
				const std::lock_guard lock(s_taskMutex);
				tasks.swap(s_tasks);
			}
			for (auto& task : tasks)
				InvokeSafely(task);
		}

		struct ScheduledPump
		{
			std::uint64_t generation{};
			NativeUiTask callback;
		};

		void ReleaseScheduledPump(const void* data)
		{
			delete static_cast<const ScheduledPump*>(data);
		}

		void InvokeScheduledPump(CFRunLoopTimerRef timer, void* data)
		{
			auto* scheduled = static_cast<ScheduledPump*>(data);
			const auto generation = scheduled->generation;
			auto callback = std::move(scheduled->callback);
			if (timer == s_pumpTimer)
				s_pumpTimer = nullptr;
			// Drop our creation reference only after copying all timer context data.
			CFRunLoopTimerInvalidate(timer);
			CFRelease(timer);
			InvokePumpAndArmWatchdog(generation, std::move(callback));
		}
#endif
	} // namespace

	bool InitializeNativeUiLoop()
	{
		const std::lock_guard lock(s_stateMutex);
		if (s_initialized.load(std::memory_order_acquire))
			return s_uiThread == std::this_thread::get_id();

#if defined(__linux__)
		// The frontend owns GTK initialization because it must pass the real argv.
		// This layer only owns the nested application loop and GLib dispatch sources.
#elif defined(_WIN32)
		s_module = GetModuleHandleW(nullptr);
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.lpfnWndProc = &NativeUiWindowProc;
		windowClass.hInstance = s_module;
		windowClass.lpszClassName = kWindowClassName;
		const ATOM registered = RegisterClassExW(&windowClass);
		if (!registered && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			return false;
		s_registeredWindowClass = registered != 0;
		s_messageWindow = CreateWindowExW(0, kWindowClassName, L"", 0, 0, 0, 0, 0,
			HWND_MESSAGE, nullptr, s_module, nullptr);
		if (!s_messageWindow)
		{
			if (s_registeredWindowClass)
				UnregisterClassW(kWindowClassName, s_module);
			s_registeredWindowClass = false;
			return false;
		}
		s_uiThreadId = GetCurrentThreadId();
#elif defined(__APPLE__)
		s_runLoop = CFRunLoopGetCurrent();
		if (!s_runLoop)
			return false;
		CFRetain(s_runLoop);
		CFRunLoopSourceContext context{};
		context.perform = &DrainPostedTasks;
		s_taskSource = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
		if (!s_taskSource)
		{
			CFRelease(s_runLoop);
			s_runLoop = nullptr;
			return false;
		}
		CFRunLoopAddSource(s_runLoop, s_taskSource, kCFRunLoopCommonModes);
#endif

		s_uiThread = std::this_thread::get_id();
		s_lifecycleGeneration.fetch_add(1, std::memory_order_acq_rel);
		s_acceptingTasks.store(true, std::memory_order_release);
		s_initialized.store(true, std::memory_order_release);
		return true;
	}

	void ShutdownNativeUiLoop()
	{
		const std::lock_guard lock(s_stateMutex);
		if (!s_initialized.load(std::memory_order_acquire) ||
			s_uiThread != std::this_thread::get_id())
		{
			return;
		}

		s_acceptingTasks.store(false, std::memory_order_release);
		s_lifecycleGeneration.fetch_add(1, std::memory_order_acq_rel);
		s_pumpGeneration.fetch_add(1, std::memory_order_acq_rel);
		s_pumpActive = false;
		s_reentrantPumpDetected = false;

#if defined(__linux__)
		if (s_pumpSource)
			g_source_remove(std::exchange(s_pumpSource, 0));
#elif defined(_WIN32)
		KillTimer(s_messageWindow, kPumpTimer);
		s_scheduledPump = {};
		MSG message{};
		while (PeekMessageW(&message, s_messageWindow, kPostedTaskMessage, kPostedTaskMessage, PM_REMOVE))
			delete reinterpret_cast<NativeUiTask*>(message.lParam);
		DestroyWindow(s_messageWindow);
		s_messageWindow = nullptr;
		if (s_registeredWindowClass)
			UnregisterClassW(kWindowClassName, s_module);
		s_registeredWindowClass = false;
		s_uiThreadId = 0;
		s_module = nullptr;
#elif defined(__APPLE__)
		if (s_pumpTimer)
		{
			CFRunLoopTimerInvalidate(s_pumpTimer);
			CFRelease(s_pumpTimer);
			s_pumpTimer = nullptr;
		}
		CFRunLoopRemoveSource(s_runLoop, s_taskSource, kCFRunLoopCommonModes);
		CFRunLoopSourceInvalidate(s_taskSource);
		CFRelease(s_taskSource);
		s_taskSource = nullptr;
		CFRelease(s_runLoop);
		s_runLoop = nullptr;
		{
			const std::lock_guard taskLock(s_taskMutex);
			s_tasks.clear();
		}
#endif

		s_uiThread = {};
		s_initialized.store(false, std::memory_order_release);
	}

	void RunNativeUiLoop()
	{
		if (!s_initialized.load(std::memory_order_acquire) || !IsNativeUiThread())
			return;

#if defined(__linux__)
		gtk_main();
#elif defined(_WIN32)
		MSG message{};
		while (GetMessageW(&message, nullptr, 0, 0) > 0)
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
#elif defined(__APPLE__)
		CFRunLoopRun();
#endif
	}

	void QuitNativeUiLoop()
	{
#if defined(__linux__)
		PostNativeUi([] { gtk_main_quit(); });
#elif defined(_WIN32)
		const std::lock_guard lock(s_stateMutex);
		if (!s_initialized.load(std::memory_order_acquire))
			return;
		if (const auto window = s_messageWindow)
			PostMessageW(window, kQuitLoopMessage, 0, 0);
		else if (s_uiThreadId)
			PostThreadMessageW(s_uiThreadId, WM_QUIT, 0, 0);
#elif defined(__APPLE__)
		const std::lock_guard lock(s_stateMutex);
		if (!s_initialized.load(std::memory_order_acquire))
			return;
		if (const auto runLoop = s_runLoop)
		{
			CFRunLoopStop(runLoop);
			CFRunLoopWakeUp(runLoop);
		}
#endif
	}

	bool PostNativeUi(NativeUiTask task)
	{
		if (!task)
			return false;
		const std::lock_guard lock(s_stateMutex);
		if (!s_initialized.load(std::memory_order_acquire) ||
			!s_acceptingTasks.load(std::memory_order_acquire))
		{
			return false;
		}

#if defined(__linux__)
		auto* posted = new PostedTask{
			s_lifecycleGeneration.load(std::memory_order_acquire), std::move(task)};
		return g_idle_add_full(G_PRIORITY_DEFAULT, &InvokePostedTask, posted,
			&DestroyPostedTask) != 0;
#elif defined(_WIN32)
		auto posted = std::make_unique<NativeUiTask>(std::move(task));
		if (!s_messageWindow ||
			!PostMessageW(s_messageWindow, kPostedTaskMessage, 0,
				reinterpret_cast<LPARAM>(posted.get())))
		{
			return false;
		}
		posted.release();
		return true;
#elif defined(__APPLE__)
		{
			const std::lock_guard lock(s_taskMutex);
			if (!s_acceptingTasks.load(std::memory_order_relaxed))
				return false;
			s_tasks.emplace_back(std::move(task));
		}
		CFRunLoopSourceSignal(s_taskSource);
		CFRunLoopWakeUp(s_runLoop);
		return true;
#endif
	}

	namespace
	{
		void InvokePumpAndArmWatchdog(std::uint64_t generation, NativeUiTask callback)
		{
			if (!callback || !s_acceptingTasks.load(std::memory_order_acquire) ||
				generation != s_pumpGeneration.load(std::memory_order_acquire))
			{
				return;
			}

			if (s_pumpActive)
			{
				// A nested native loop can dispatch a zero-delay request from inside
				// CefDoMessageLoopWork. Do not re-enter CEF; repost that work instead.
				s_reentrantPumpDetected = true;
				return;
			}

			auto watchdogCallback = callback;
			s_reentrantPumpDetected = false;
			s_pumpActive = true;
			InvokeSafely(callback);
			s_pumpActive = false;

			if (!s_acceptingTasks.load(std::memory_order_acquire))
				return;

			if (s_reentrantPumpDetected)
			{
				// The nested call was deliberately discarded. Claim a new generation
				// only if no still-newer CEF request has appeared in the meantime.
				auto expected = s_pumpGeneration.load(std::memory_order_acquire);
				if (s_pumpGeneration.compare_exchange_strong(expected, expected + 1,
					std::memory_order_acq_rel, std::memory_order_acquire))
				{
					QueuePumpRequest(std::chrono::milliseconds::zero(), expected + 1,
						std::move(watchdogCallback));
				}
				return;
			}

			// CEF may occasionally omit a follow-up deadline after a work cycle.
			// Match cefclient's external pump by ensuring that the browser process is
			// serviced at least at 30 Hz. A real CEF request wins via the generation
			// CAS and replaces this watchdog before it fires.
			auto expected = generation;
			if (s_pumpGeneration.compare_exchange_strong(expected, generation + 1,
				std::memory_order_acq_rel, std::memory_order_acquire))
			{
				QueuePumpRequest(kMaximumPumpDelay, generation + 1,
					std::move(watchdogCallback));
			}
		}

		void QueuePumpRequest(std::chrono::milliseconds delay, std::uint64_t generation,
			NativeUiTask callback)
		{
			delay = std::clamp(delay, std::chrono::milliseconds::zero(), kMaximumPumpDelay);
			PostNativeUi([delay, generation, callback = std::move(callback)]() mutable {
				if (generation != s_pumpGeneration.load(std::memory_order_acquire))
					return;

#if defined(__linux__)
				if (s_pumpSource)
					g_source_remove(std::exchange(s_pumpSource, 0));
#elif defined(_WIN32)
				KillTimer(s_messageWindow, kPumpTimer);
				s_scheduledPump = {};
#elif defined(__APPLE__)
				if (s_pumpTimer)
				{
					CFRunLoopTimerInvalidate(s_pumpTimer);
					CFRelease(s_pumpTimer);
					s_pumpTimer = nullptr;
				}
#endif

				if (delay == std::chrono::milliseconds::zero())
				{
					InvokePumpAndArmWatchdog(generation, std::move(callback));
					return;
				}

#if defined(__linux__)
				const auto maximum = static_cast<std::int64_t>(std::numeric_limits<guint>::max());
				const guint milliseconds = static_cast<guint>(std::min(delay.count(), maximum));
				auto* scheduled = new ScheduledPump{generation, std::move(callback)};
				s_pumpSource = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, milliseconds,
					&InvokeScheduledPump, scheduled, &DestroyScheduledPump);
#elif defined(_WIN32)
				s_scheduledPump = [generation, callback = std::move(callback)]() mutable {
					InvokePumpAndArmWatchdog(generation, std::move(callback));
				};
				const auto maximum = static_cast<std::int64_t>(std::numeric_limits<UINT>::max());
				const auto milliseconds = static_cast<UINT>(std::max<std::int64_t>(1,
					std::min(delay.count(), maximum)));
				SetTimer(s_messageWindow, kPumpTimer, milliseconds, nullptr);
#elif defined(__APPLE__)
				auto* scheduled = new ScheduledPump{generation, std::move(callback)};
				CFRunLoopTimerContext context{};
				context.info = scheduled;
				context.release = &ReleaseScheduledPump;
				const auto seconds = static_cast<CFTimeInterval>(delay.count()) / 1000.0;
				s_pumpTimer = CFRunLoopTimerCreate(kCFAllocatorDefault,
					CFAbsoluteTimeGetCurrent() + seconds, 0, 0, 0, &InvokeScheduledPump, &context);
				if (!s_pumpTimer)
				{
					delete scheduled;
					return;
				}
				CFRunLoopAddTimer(s_runLoop, s_pumpTimer, kCFRunLoopCommonModes);
#endif
			});
		}
	} // namespace

	void ScheduleCefMessagePump(std::chrono::milliseconds delay, NativeUiTask callback)
	{
		if (!callback || !s_initialized.load(std::memory_order_acquire))
			return;

		// s_pumpActive is only ever touched on the UI thread, so establish the
		// thread before reading it.
		if (IsNativeUiThread() && s_pumpActive)
		{
			// CEF is asking for another cycle from inside CefDoMessageLoopWork.
			// Posting a zero-delay source here is a trap: showing a window runs a
			// nested GTK loop inside this very pump, that loop dispatches the new
			// source immediately, the dispatch is refused as reentrant, and CEF -
			// still unserviced - asks again. The result is a UI thread spinning at
			// full speed while no CEF work runs at all, which is what a frozen
			// launcher and a blank tool window look like. The pump on the stack
			// re-arms itself once it unwinds, so record the request and return.
			s_reentrantPumpDetected = true;
			return;
		}

		const auto generation = s_pumpGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
		QueuePumpRequest(delay, generation, std::move(callback));
	}

	bool IsNativeUiThread()
	{
		const std::lock_guard lock(s_stateMutex);
		return s_initialized.load(std::memory_order_acquire) &&
			s_uiThread == std::this_thread::get_id();
	}
} // namespace WebFrontend::CefNative
