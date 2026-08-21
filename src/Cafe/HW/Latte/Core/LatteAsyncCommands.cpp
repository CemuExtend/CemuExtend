#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceCopy.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"

#include <condition_variable>
#include <future>

void LatteThread_Exit();

SlimRWLock swl_gpuAsyncCommands;

typedef struct  
{
	uint32 type;
	union
	{
		struct
		{
			MPTR physAddr;
			MPTR mipAddr;
			uint32 swizzle;
			sint32 format;
			sint32 width;
			sint32 height;
			sint32 depth;
			uint32 pitch;
			uint32 slice;
			sint32 dim;
			Latte::E_HWTILEMODE tilemode;
			sint32 aa;
			sint32 level;
		}forceTextureReadback;

		struct
		{
			uint64 shaderBaseHash; 
			uint64 shaderAuxHash; 
			LatteConst::ShaderType shaderType;
		}deleteShader;

		struct
		{
			LatteSurfaceCopyParam src;
			LatteSurfaceCopyParam dst;
			LatteSurfaceCopyRect rect;
		}textureCopy;
	};
	std::function<void()> rendererTask;
}LatteAsyncCommand_t;

#define ASYNC_CMD_FORCE_TEXTURE_READBACK		1
#define ASYNC_CMD_DELETE_SHADER					2
#define ASYNC_CMD_TEXTURE_COPY					3
#define ASYNC_CMD_RENDERER_TASK				4

std::queue<LatteAsyncCommand_t> LatteAsyncCommandQueue;

void LatteAsyncCommands_queueForceTextureReadback(MPTR physAddr, MPTR mipAddr, uint32 swizzle, sint32 format, sint32 width, sint32 height, sint32 depth, uint32 pitch, uint32 slice, sint32 dim, Latte::E_HWTILEMODE tilemode, sint32 aa, sint32 level)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_FORCE_TEXTURE_READBACK;
	
	asyncCommand.forceTextureReadback.physAddr = physAddr;
	asyncCommand.forceTextureReadback.mipAddr = mipAddr;
	asyncCommand.forceTextureReadback.swizzle = swizzle;
	asyncCommand.forceTextureReadback.format = format;
	asyncCommand.forceTextureReadback.width = width;
	asyncCommand.forceTextureReadback.height = height;
	asyncCommand.forceTextureReadback.depth = depth;
	asyncCommand.forceTextureReadback.pitch = pitch;
	asyncCommand.forceTextureReadback.slice = slice;
	asyncCommand.forceTextureReadback.dim = dim;
	asyncCommand.forceTextureReadback.tilemode = tilemode;
	asyncCommand.forceTextureReadback.aa = aa;
	asyncCommand.forceTextureReadback.level = level;
	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

void LatteAsyncCommands_queueDeleteShader(uint64 shaderBaseHash, uint64 shaderAuxHash, LatteConst::ShaderType shaderType)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_DELETE_SHADER;

	asyncCommand.deleteShader.shaderBaseHash = shaderBaseHash;
	asyncCommand.deleteShader.shaderAuxHash = shaderAuxHash;
	asyncCommand.deleteShader.shaderType = shaderType;

	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

void LatteAsyncCommand_queueTextureCopy(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_TEXTURE_COPY;
	asyncCommand.textureCopy.src = src;
	asyncCommand.textureCopy.dst = dst;
	asyncCommand.textureCopy.rect = rect;

	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

void LatteAsyncCommands_waitUntilAllProcessed()
{
	for (;;)
	{
		swl_gpuAsyncCommands.LockWrite();
		const auto empty = LatteAsyncCommandQueue.empty();
		swl_gpuAsyncCommands.UnlockWrite();
		if (empty)
			return;
		_mm_pause();
	}
}

void LatteAsyncCommands_runOnRendererThread(std::function<void()> task)
{
	if (!task)
		return;
	if (Latte_GetStopSignal())
		throw std::runtime_error("the renderer thread is stopping");

	enum class TaskState : std::uint8_t
	{
		Pending,
		Running,
		Completed,
		Cancelled,
	};
	struct Completion
	{
		std::atomic<TaskState> state{TaskState::Pending};
		std::promise<void> promise;
	};
	auto completion = std::make_shared<Completion>();
	auto future = completion->promise.get_future();
	LatteAsyncCommand_t command{};
	command.type = ASYNC_CMD_RENDERER_TASK;
	command.rendererTask = [task = std::move(task), completion]() mutable {
		auto expected = TaskState::Pending;
		if (!completion->state.compare_exchange_strong(expected, TaskState::Running,
			std::memory_order_acq_rel))
			return;
		try
		{
			task();
			completion->promise.set_value();
		}
		catch (...)
		{
			completion->promise.set_exception(std::current_exception());
		}
		completion->state.store(TaskState::Completed, std::memory_order_release);
	};
	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(std::move(command));
	swl_gpuAsyncCommands.UnlockWrite();

	while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
	{
		if (Latte_GetStopSignal())
		{
			auto expected = TaskState::Pending;
			if (completion->state.compare_exchange_strong(expected, TaskState::Cancelled,
				std::memory_order_acq_rel))
				throw std::runtime_error("the renderer thread stopped before completing a host task");
		}
	}
	future.get();
}

void LatteAsyncCommands_runWithRendererPaused(std::function<void()> task)
{
	if (!task)
		return;
	if (Latte_GetStopSignal())
		throw std::runtime_error("the renderer thread is stopping");

	struct PauseState
	{
		std::mutex mutex;
		std::condition_variable condition;
		bool paused{};
		bool released{};
		bool completed{};
		bool cancelled{};
	};
	auto state = std::make_shared<PauseState>();
	LatteAsyncCommand_t command{};
	command.type = ASYNC_CMD_RENDERER_TASK;
	command.rendererTask = [state] {
		std::unique_lock lock(state->mutex);
		if (state->cancelled)
			return;
		state->paused = true;
		state->condition.notify_all();
		state->condition.wait(lock, [&] { return state->released; });
		state->completed = true;
		state->condition.notify_all();
	};
	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(std::move(command));
	swl_gpuAsyncCommands.UnlockWrite();

	std::unique_lock lock(state->mutex);
	while (!state->paused)
	{
		if (state->condition.wait_for(lock, std::chrono::milliseconds(10),
			[&] { return state->paused; }))
			break;
		if (Latte_GetStopSignal())
		{
			state->cancelled = true;
			throw std::runtime_error("the renderer thread stopped before entering a host barrier");
		}
	}
	lock.unlock();
	std::exception_ptr failure;
	try
	{
		task();
	}
	catch (...)
	{
		failure = std::current_exception();
	}
	lock.lock();
	state->released = true;
	state->condition.notify_all();
	state->condition.wait(lock, [&] { return state->completed; });
	lock.unlock();
	if (failure)
		std::rethrow_exception(failure);
}

/*
 * Called by the GPU command processor frequently
 */
void LatteAsyncCommands_checkAndExecute()
{
	if (Latte_GetStopSignal())
		LatteThread_Exit();
	swl_gpuAsyncCommands.LockWrite();
	if (LatteAsyncCommandQueue.empty())
	{
		swl_gpuAsyncCommands.UnlockWrite();
		return;
	}
	while (LatteAsyncCommandQueue.empty() == false)
	{
		// get first command in queue
		LatteAsyncCommand_t asyncCommand = LatteAsyncCommandQueue.front();
		swl_gpuAsyncCommands.UnlockWrite();
		if (asyncCommand.type == ASYNC_CMD_FORCE_TEXTURE_READBACK)
		{
			cemu_assert_debug(asyncCommand.forceTextureReadback.level == 0); // implement mip swizzle and verify
			LatteTextureView* textureView = LatteTC_GetTextureSliceViewOrTryCreate(asyncCommand.forceTextureReadback.physAddr, asyncCommand.forceTextureReadback.mipAddr, (Latte::E_GX2SURFFMT)asyncCommand.forceTextureReadback.format, asyncCommand.forceTextureReadback.tilemode, asyncCommand.forceTextureReadback.width, asyncCommand.forceTextureReadback.height, asyncCommand.forceTextureReadback.depth, asyncCommand.forceTextureReadback.pitch, 0, asyncCommand.forceTextureReadback.slice, asyncCommand.forceTextureReadback.level);
			if (textureView != nullptr)
			{
				LatteTexture_UpdateDataToLatest(textureView->baseTexture);
				// start transfer
				LatteTextureReadback_StartTransfer(textureView);
				// wait until finished
				LatteTextureReadback_UpdateFinishedTransfers(true);
			}
			else
			{
				cemuLog_logDebug(LogType::Force, "Texture not found for readback");
			}
		}
		else if (asyncCommand.type == ASYNC_CMD_DELETE_SHADER)
		{
			LatteSHRC_RemoveFromCacheByHash(asyncCommand.deleteShader.shaderBaseHash, asyncCommand.deleteShader.shaderAuxHash, asyncCommand.deleteShader.shaderType);
		}
		else if (asyncCommand.type == ASYNC_CMD_TEXTURE_COPY)
		{
			LatteSurfaceCopy_copySurfaceNew(asyncCommand.textureCopy.src, asyncCommand.textureCopy.dst, asyncCommand.textureCopy.rect);
		}
		else if (asyncCommand.type == ASYNC_CMD_RENDERER_TASK)
		{
			asyncCommand.rendererTask();
		}
		else
		{
			cemu_assert_unimplemented();
		}
		swl_gpuAsyncCommands.LockWrite();
		LatteAsyncCommandQueue.pop();
	}
	swl_gpuAsyncCommands.UnlockWrite();
}
