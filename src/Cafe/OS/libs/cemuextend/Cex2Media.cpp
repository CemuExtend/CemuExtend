#include "Cafe/OS/libs/cemuextend/Cex2Media.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <Windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace cemuextend_hle
{
	namespace
	{
		using namespace cemuextend::wire;
		constexpr std::uint32_t kMaximumUrlBytes = 2048;
		constexpr std::uint16_t kMaximumWidth = 1280;
		constexpr std::uint16_t kMaximumHeight = 720;
		constexpr std::uint16_t kMaximumFps = 30;
		constexpr std::size_t kMaximumPlaylistEntries = 100;
		constexpr std::size_t kMaximumAudioSamples = 32000U * 2U;

		class Child final
		{
		  public:
			~Child()
			{
				Stop();
			}
			bool Start(const std::vector<std::string>& arguments)
			{
				Stop();
				if (arguments.empty())
					return false;
#ifdef _WIN32
				SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
				HANDLE write{};
				if (!CreatePipe(&read_, &write, &security, 0))
					return false;
				SetHandleInformation(read_, HANDLE_FLAG_INHERIT, 0);
				std::wstring command;
				for (const auto& argument : arguments)
				{
					if (!command.empty())
						command += L' ';
					command += L'"';
					for (char ch : argument)
					{
						if (ch == '"')
							command += L'\\';
						command += static_cast<wchar_t>(static_cast<unsigned char>(ch));
					}
					command += L'"';
				}
				STARTUPINFOW startup{sizeof(startup)};
				startup.dwFlags = STARTF_USESTDHANDLES;
				startup.hStdOutput = write;
				startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
				startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
				PROCESS_INFORMATION info{};
				const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
											   CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
				CloseHandle(write);
				if (!ok)
				{
					CloseHandle(read_);
					read_ = nullptr;
					return false;
				}
				process_ = info.hProcess;
				CloseHandle(info.hThread);
#else
				int pipes[2]{};
				if (pipe(pipes) != 0)
					return false;
				if (fcntl(pipes[0], F_SETFD, FD_CLOEXEC) == -1 ||
					fcntl(pipes[1], F_SETFD, FD_CLOEXEC) == -1)
				{
					close(pipes[0]);
					close(pipes[1]);
					return false;
				}
				posix_spawn_file_actions_t actions;
				posix_spawn_file_actions_init(&actions);
				posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
				posix_spawn_file_actions_addclose(&actions, pipes[0]);
				posix_spawn_file_actions_addclose(&actions, pipes[1]);
				std::vector<char*> argv;
				for (const auto& argument : arguments)
					argv.push_back(const_cast<char*>(argument.c_str()));
				argv.push_back(nullptr);
				const int result = posix_spawnp(&pid_, argv.front(), &actions, nullptr, argv.data(), environ);
				posix_spawn_file_actions_destroy(&actions);
				close(pipes[1]);
				if (result != 0)
				{
					close(pipes[0]);
					pid_ = -1;
					return false;
				}
				read_ = pipes[0];
#endif
				return true;
			}
			std::size_t Read(std::span<std::byte> output)
			{
#ifdef _WIN32
				HANDLE readHandle{};
				{
					std::scoped_lock lock(mutex_);
					readHandle = read_;
				}
				DWORD read{};
				return readHandle && ReadFile(readHandle, output.data(), static_cast<DWORD>(output.size()), &read, nullptr) ? read : 0;
#else
				int readDescriptor{};
				{
					std::scoped_lock lock(mutex_);
					readDescriptor = read_;
				}
				if (readDescriptor < 0)
					return 0;
				ssize_t result{};
				do
				{
					result = ::read(readDescriptor, output.data(), output.size());
				}
				while (result < 0 && errno == EINTR);
				return result > 0 ? static_cast<std::size_t>(result) : 0;
#endif
			}
			bool ReadExact(std::span<std::byte> output)
			{
				std::size_t offset{};
				while (offset < output.size())
				{
					const auto read = Read(output.subspan(offset));
					if (read == 0)
						return false;
					offset += read;
				}
				return true;
			}
			std::string ReadAll(std::size_t maximum)
			{
				std::string result;
				std::array<std::byte, 4096> buffer{};
				while (result.size() < maximum)
				{
					const auto count = Read(buffer);
					if (count == 0)
						break;
					result.append(reinterpret_cast<const char*>(buffer.data()), count);
				}
				return result;
			}
			void Stop()
			{
#ifdef _WIN32
				HANDLE process{};
				HANDLE readHandle{};
				{
					std::scoped_lock lock(mutex_);
					process = std::exchange(process_, nullptr);
					readHandle = std::exchange(read_, nullptr);
				}
				if (process)
				{
					TerminateProcess(process, 0);
					WaitForSingleObject(process, 2000);
					CloseHandle(process);
				}
				if (readHandle)
					CloseHandle(readHandle);
#else
				pid_t process{};
				int readDescriptor{};
				{
					std::scoped_lock lock(mutex_);
					process = std::exchange(pid_, -1);
					readDescriptor = std::exchange(read_, -1);
				}
				// Closing the read side first prevents a child blocked on a full stdout
				// pipe from deadlocking this thread in waitpid during cancellation.
				if (readDescriptor >= 0)
					close(readDescriptor);
				if (process > 0)
				{
					kill(process, SIGTERM);
					bool reaped{};
					for (unsigned attempt{}; attempt < 200; ++attempt)
					{
						const auto result = waitpid(process, nullptr, WNOHANG);
						if (result == process || (result < 0 && errno == ECHILD))
						{
							reaped = true;
							break;
						}
						if (result < 0 && errno != EINTR)
							break;
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					}
					if (!reaped)
					{
						kill(process, SIGKILL);
						while (waitpid(process, nullptr, 0) < 0 && errno == EINTR)
						{
						}
					}
				}
#endif
			}

		  private:
			std::mutex mutex_;
#ifdef _WIN32
			HANDLE process_{};
			HANDLE read_{};
#else
			pid_t pid_{-1};
			int read_{-1};
#endif
		};

		std::vector<std::string> Lines(std::string text)
		{
			std::vector<std::string> result;
			for (std::size_t start{}; start < text.size();)
			{
				const auto end = text.find('\n', start);
				auto line = text.substr(start, end == std::string::npos ? text.size() - start : end - start);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (!line.empty())
					result.push_back(std::move(line));
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			return result;
		}

		bool IsYoutubeUrl(std::string_view url)
		{
			return url.starts_with("https://www.youtube.com/") || url.starts_with("https://youtube.com/") ||
				   url.starts_with("https://youtu.be/");
		}

		struct Session
		{
			std::mutex mutex;
			std::condition_variable wake;
			std::jthread worker;
			std::vector<std::array<std::uint8_t, 4>> palette;
			std::vector<std::uint8_t> frame;
			std::vector<std::uint8_t> readFrame;
			std::vector<std::int16_t> audio;
			std::vector<std::string> playlist;
			std::string source;
			std::uint64_t owner{};
			std::uint64_t positionUs{};
			std::uint64_t durationUs{};
			std::uint32_t handle{};
			std::uint32_t frameId{};
			std::uint32_t playlistIndex{};
			std::uint32_t droppedFrames{};
			std::uint32_t effectiveMilliFps{};
			std::uint32_t commandGeneration{};
			std::uint32_t lastReadFrameId{};
			std::uint32_t readFrameId{};
			std::uint32_t fpsFrames{};
			std::uint64_t readFramePresentationTimeUs{};
			std::size_t audioRead{};
			std::chrono::steady_clock::time_point lastFpsUpdate{std::chrono::steady_clock::now()};
			std::uint16_t width{};
			std::uint16_t height{};
			std::uint16_t fps{};
			Status error{Status::Ok};
			MediaState state{MediaState::Opening};
			bool paused{};
			bool stopped{};
			bool seekPending{};
		};

		struct Registry
		{
			std::mutex mutex;
			std::unordered_map<std::uint32_t, std::shared_ptr<Session>> sessions;
			std::uint32_t nextHandle{1};
		};
		Registry& Sessions()
		{
			static Registry value;
			return value;
		}

		std::vector<std::uint8_t> BuildLookup(const Session& session)
		{
			std::vector<std::uint8_t> lookup(32U * 32U * 32U);
			for (unsigned r{}; r < 32; ++r)
				for (unsigned g{}; g < 32; ++g)
					for (unsigned b{}; b < 32; ++b)
					{
						unsigned bestDistance = std::numeric_limits<unsigned>::max();
						std::uint8_t best{};
						// Map palette entry zero is transparent. Never choose it for opaque video
						// pixels merely because its RGB payload is also black. The guest also marks
						// unavailable map-color slots transparent; those indices are not valid
						// renderer inputs either.
						for (std::size_t i{session.palette.size() > 1 ? 1U : 0U}; i < session.palette.size(); ++i)
						{
							if (session.palette[i][3] == 0)
								continue;
							const int dr = static_cast<int>(r * 255 / 31) - session.palette[i][0];
							const int dg = static_cast<int>(g * 255 / 31) - session.palette[i][1];
							const int db = static_cast<int>(b * 255 / 31) - session.palette[i][2];
							const unsigned distance = static_cast<unsigned>(dr * dr + dg * dg + db * db);
							if (distance < bestDistance)
							{
								bestDistance = distance;
								best = static_cast<std::uint8_t>(i);
							}
						}
						lookup[(r << 10U) | (g << 5U) | b] = best;
					}
			return lookup;
		}

		void Run(std::stop_token stop, Session* session)
		{
#ifdef CEMU_CEX2_TESTING
			std::scoped_lock lock(session->mutex);
			session->state = MediaState::Failed;
			session->error = Status::NotSupported;
			return;
#else
			Child resolver;
			if (!resolver.Start({"yt-dlp", "--flat-playlist", "--playlist-end", "100", "--print", "%(webpage_url)s", "--", session->source}))
			{
				std::scoped_lock lock(session->mutex);
				session->state = MediaState::Failed;
				session->error = Status::NotFound;
				return;
			}
			std::stop_callback stopResolver(stop, [&] { resolver.Stop(); });
			auto playlist = Lines(resolver.ReadAll(1024U * 1024U));
			resolver.Stop();
			if (playlist.empty())
				playlist.push_back(session->source);
			if (playlist.size() > kMaximumPlaylistEntries)
				playlist.resize(kMaximumPlaylistEntries);
			{
				std::scoped_lock lock(session->mutex);
				session->playlist = playlist;
			}
			const auto lookup = BuildLookup(*session);
			while (!stop.stop_requested())
			{
				std::uint32_t index{};
				std::uint32_t generation{};
				std::uint64_t seekUs{};
				{
					std::scoped_lock lock(session->mutex);
					if (session->stopped || session->playlistIndex >= session->playlist.size())
						break;
					index = session->playlistIndex;
					generation = session->commandGeneration;
					seekUs = session->seekPending ? session->positionUs : 0;
					session->seekPending = false;
					session->audio.clear();
					session->audioRead = 0;
				}
				Child urlResolver;
				const auto limit = "bestvideo[height<=" + std::to_string(session->height) + "]+bestaudio/best[height<=" + std::to_string(session->height) + "]";
				if (!urlResolver.Start({"yt-dlp", "--get-url", "--no-playlist", "--match-filter", "!is_live", "--age-limit", "0", "-f", limit, "--", playlist[index]}))
				{
					std::scoped_lock lock(session->mutex);
					session->state = MediaState::Failed;
					session->error = Status::NotFound;
					break;
				}
				std::stop_callback stopUrlResolver(stop, [&] { urlResolver.Stop(); });
				auto urls = Lines(urlResolver.ReadAll(64U * 1024U));
				urlResolver.Stop();
				if (urls.empty())
				{
					std::scoped_lock lock(session->mutex);
					session->state = MediaState::Failed;
					session->error = Status::InvalidArgument;
					break;
				}
				Child video, audio;
				std::stop_callback stopVideo(stop, [&] { video.Stop(); });
				std::stop_callback stopAudio(stop, [&] { audio.Stop(); });
				const auto filter = "scale=" + std::to_string(session->width) + ":" + std::to_string(session->height) + ":force_original_aspect_ratio=decrease,pad=" + std::to_string(session->width) + ":" + std::to_string(session->height) + ":(ow-iw)/2:(oh-ih)/2:black";
				std::vector<std::string> videoArguments{"ffmpeg", "-nostdin", "-loglevel", "error", "-re"};
				std::vector<std::string> audioArguments{"ffmpeg", "-nostdin", "-loglevel", "error", "-re"};
				if (seekUs != 0)
				{
					const auto seek = std::to_string(static_cast<double>(seekUs) / 1000000.0);
					videoArguments.insert(videoArguments.end(), {"-ss", seek});
					audioArguments.insert(audioArguments.end(), {"-ss", seek});
				}
				videoArguments.insert(videoArguments.end(), {"-i", urls.front(), "-an", "-vf", filter, "-r", std::to_string(session->fps), "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"});
				audioArguments.insert(audioArguments.end(), {"-i", urls.back(), "-vn", "-f", "s16le", "-ac", "1", "-ar", "32000", "pipe:1"});
				if (!video.Start(videoArguments) || !audio.Start(audioArguments))
				{
					std::scoped_lock lock(session->mutex);
					session->state = MediaState::Failed;
					session->error = Status::NotFound;
					break;
				}
				std::vector<std::byte> rgb(static_cast<std::size_t>(session->width) * session->height * 3U);
				std::vector<std::byte> pcm((32000U / session->fps) * sizeof(std::int16_t));
				while (!stop.stop_requested() && video.ReadExact(rgb))
				{
					{
						std::unique_lock lock(session->mutex);
						session->wake.wait(lock, [&] { return stop.stop_requested() || !session->paused || session->stopped || session->commandGeneration != generation; });
						if (session->stopped || session->commandGeneration != generation)
							break;
					}
					if (!audio.ReadExact(pcm))
						std::fill(pcm.begin(), pcm.end(), std::byte{});
					std::vector<std::uint8_t> indexed(rgb.size() / 3U);
					for (std::size_t pixel{}; pixel < indexed.size(); ++pixel)
					{
						const auto r = static_cast<std::uint8_t>(rgb[pixel * 3U]);
						const auto g = static_cast<std::uint8_t>(rgb[pixel * 3U + 1U]);
						const auto b = static_cast<std::uint8_t>(rgb[pixel * 3U + 2U]);
						indexed[pixel] = lookup[((r >> 3U) << 10U) | ((g >> 3U) << 5U) | (b >> 3U)];
					}
					std::scoped_lock lock(session->mutex);
					if (session->frameId != 0 && session->lastReadFrameId != session->frameId)
						++session->droppedFrames;
					session->frame = std::move(indexed);
					++session->frameId;
					session->state = MediaState::Playing;
					session->positionUs += 1000000U / session->fps;
					++session->fpsFrames;
					const auto now = std::chrono::steady_clock::now();
					const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - session->lastFpsUpdate).count();
					if (elapsed >= 1000)
					{
						session->effectiveMilliFps = static_cast<std::uint32_t>(session->fpsFrames * 1000000ULL / static_cast<std::uint64_t>(elapsed));
						session->fpsFrames = 0;
						session->lastFpsUpdate = now;
					}
					const auto* samples = reinterpret_cast<const std::int16_t*>(pcm.data());
					session->audio.insert(session->audio.end(), samples, samples + pcm.size() / 2U);
					if (session->audio.size() > kMaximumAudioSamples)
						session->audio.erase(session->audio.begin(), session->audio.begin() + (session->audio.size() - kMaximumAudioSamples));
				}
				{
					std::scoped_lock lock(session->mutex);
					if (session->stopped)
						break;
					if (session->commandGeneration == generation)
					{
						++session->playlistIndex;
						session->positionUs = 0;
						++session->commandGeneration;
					}
				}
			}
			std::scoped_lock lock(session->mutex);
			if (session->error == Status::Ok)
				session->state = session->stopped ? MediaState::Closed : MediaState::Ended;
#endif
		}

		std::shared_ptr<Session> Find(std::uint64_t owner, std::uint32_t handle)
		{
			auto& registry = Sessions();
			std::scoped_lock lock(registry.mutex);
			const auto found = registry.sessions.find(handle);
			return found != registry.sessions.end() && found->second->owner == owner ? found->second : nullptr;
		}

		Cex2MediaResult Fail(Status status)
		{
			return {status, {}};
		}
		template<typename T>
		Cex2MediaResult Reply(const T& value)
		{
			Cex2MediaResult result{Status::Ok, {}};
			result.payload.resize(sizeof(value));
			std::memcpy(result.payload.data(), &value, sizeof(value));
			return result;
		}
	} // namespace

	Cex2MediaResult Cex2Media::Dispatch(std::uint64_t owner, std::string_view, std::uint16_t operation, std::span<const std::byte> payload)
	{
		switch (static_cast<MediaOperation>(operation))
		{
		case MediaOperation::Probe:
			if (!payload.empty())
				return Fail(Status::InvalidArgument);
			return {Status::Ok, {}};
		case MediaOperation::Open:
		{
			if (payload.size() < sizeof(MediaOpenRequest))
				return Fail(Status::InvalidArgument);
			MediaOpenRequest request{};
			std::memcpy(&request, payload.data(), sizeof(request));
			const auto urlBytes = request.urlBytes.get();
			const auto paletteEntries = request.paletteEntries.get();
			if (urlBytes == 0 || urlBytes > kMaximumUrlBytes || paletteEntries == 0 || paletteEntries > 256 ||
				payload.size() != sizeof(request) + urlBytes + paletteEntries * 4U || request.width.get() < 16 || request.width.get() > kMaximumWidth || request.height.get() < 16 || request.height.get() > kMaximumHeight || request.framesPerSecond.get() == 0 || request.framesPerSecond.get() > kMaximumFps)
				return Fail(Status::InvalidArgument);
			const std::string url(reinterpret_cast<const char*>(payload.data() + sizeof(request)), urlBytes);
			if (!IsYoutubeUrl(url) || url.find_first_of("\r\n") != std::string::npos)
				return Fail(Status::InvalidArgument);
			auto session = std::make_shared<Session>();
			session->owner = owner;
			session->source = url;
			session->width = request.width.get();
			session->height = request.height.get();
			session->fps = request.framesPerSecond.get();
			const auto* colors = payload.data() + sizeof(request) + urlBytes;
			for (std::size_t i{}; i < paletteEntries; ++i)
			{
				std::array<std::uint8_t, 4> color{};
				std::memcpy(color.data(), colors + i * 4U, 4);
				session->palette.push_back(color);
			}
			{
				auto& registry = Sessions();
				std::scoped_lock lock(registry.mutex);
				for (const auto& [id, current] : registry.sessions)
					if (current->owner == owner)
						return Fail(Status::Busy);
				session->handle = registry.nextHandle++;
				registry.sessions.emplace(session->handle, session);
			}
			session->worker = std::jthread(Run, session.get());
			MediaOpenResponse response{};
			response.handle = session->handle;
			return Reply(response);
		}
		case MediaOperation::Control:
		{
			if (payload.size() != sizeof(MediaControlRequest))
				return Fail(Status::InvalidArgument);
			MediaControlRequest request{};
			std::memcpy(&request, payload.data(), sizeof(request));
			auto session = Find(owner, request.handle.get());
			if (!session)
				return Fail(Status::NotFound);
			std::scoped_lock lock(session->mutex);
			const auto control = static_cast<MediaControl>(request.control);
			if (control == MediaControl::Pause)
			{
				session->paused = true;
				session->state = MediaState::Paused;
			}
			else if (control == MediaControl::Play)
			{
				session->paused = false;
				session->state = MediaState::Playing;
				session->wake.notify_all();
			}
			else if (control == MediaControl::Stop)
			{
				session->stopped = true;
				session->wake.notify_all();
			}
			else if (control == MediaControl::Next && session->playlistIndex + 1 < session->playlist.size())
			{
				++session->playlistIndex;
				session->positionUs = 0;
				++session->commandGeneration;
				session->wake.notify_all();
			}
			else if (control == MediaControl::Previous)
			{
				if (session->playlistIndex > 0)
					--session->playlistIndex;
				session->positionUs = 0;
				++session->commandGeneration;
				session->wake.notify_all();
			}
			else if (control == MediaControl::Seek)
			{
				session->positionUs = request.positionUs.get();
				session->seekPending = true;
				++session->commandGeneration;
				session->wake.notify_all();
			}
			else if (control != MediaControl::Pause && control != MediaControl::Play && control != MediaControl::Stop && control != MediaControl::Next && control != MediaControl::Previous)
				return Fail(Status::InvalidArgument);
			return {Status::Ok, {}};
		}
		case MediaOperation::Status:
		{
			if (payload.size() != sizeof(MediaStatusRequest))
				return Fail(Status::InvalidArgument);
			MediaStatusRequest request{};
			std::memcpy(&request, payload.data(), sizeof(request));
			auto session = Find(owner, request.handle.get());
			if (!session)
				return Fail(Status::NotFound);
			MediaStatusResponse response{};
			std::scoped_lock lock(session->mutex);
			response.handle = session->handle;
			response.playlistIndex = session->playlistIndex;
			response.playlistEntries = static_cast<std::uint32_t>(session->playlist.size());
			response.droppedFrames = session->droppedFrames;
			response.positionUs = session->positionUs;
			response.durationUs = session->durationUs;
			response.effectiveMilliFps = session->effectiveMilliFps;
			response.error = static_cast<std::int32_t>(session->error);
			response.state = static_cast<std::uint8_t>(session->state);
			return Reply(response);
		}
		case MediaOperation::ReadFrame:
		{
			if (payload.size() != sizeof(MediaReadFrameRequest))
				return Fail(Status::InvalidArgument);
			MediaReadFrameRequest request{};
			std::memcpy(&request, payload.data(), sizeof(request));
			auto session = Find(owner, request.handle.get());
			if (!session)
				return Fail(Status::NotFound);
			MediaReadFrameResponse response{};
			std::vector<std::byte> chunk;
			{
				std::scoped_lock lock(session->mutex);
				const auto requestedFrameId = request.frameId.get();
				if (requestedFrameId == 0 && !session->frame.empty())
				{
					session->readFrame = session->frame;
					session->readFrameId = session->frameId;
					session->readFramePresentationTimeUs = session->positionUs;
				}
				const bool hasReadFrame = !session->readFrame.empty() &&
										  (requestedFrameId == 0 || requestedFrameId == session->readFrameId);
				response.handle = session->handle;
				response.frameId = hasReadFrame ? session->readFrameId : session->frameId;
				response.presentationTimeUs = hasReadFrame ? session->readFramePresentationTimeUs : session->positionUs;
				response.error = static_cast<std::int32_t>(session->error);
				response.state = static_cast<std::uint8_t>(session->state == MediaState::Failed	 ? MediaFrameState::Failed
														   : hasReadFrame						 ? MediaFrameState::Ready
														   : session->state == MediaState::Ended ? MediaFrameState::Ended
														   : session->frame.empty()				 ? MediaFrameState::Pending
																								 : MediaFrameState::Superseded);
				response.totalBytes = hasReadFrame ? static_cast<std::uint32_t>(session->readFrame.size()) : 0;
				if (hasReadFrame)
				{
					const auto offset = std::min<std::size_t>(request.offset.get(), session->readFrame.size());
					const auto length = std::min<std::size_t>({request.length.get(), session->readFrame.size() - offset, 65520U - sizeof(response)});
					chunk.resize(length);
					std::memcpy(chunk.data(), session->readFrame.data() + offset, length);
					response.chunkBytes = static_cast<std::uint32_t>(length);
					if (offset + length == session->readFrame.size())
						session->lastReadFrameId = session->readFrameId;
				}
			}
			auto result = Reply(response);
			result.payload.insert(result.payload.end(), chunk.begin(), chunk.end());
			return result;
		}
		case MediaOperation::Close:
		{
			if (payload.size() != 4)
				return Fail(Status::InvalidArgument);
			Be32 handle{};
			std::memcpy(&handle, payload.data(), 4);
			auto session = Find(owner, handle.get());
			if (!session)
				return Fail(Status::NotFound);
			{
				std::scoped_lock lock(session->mutex);
				session->stopped = true;
				session->wake.notify_all();
			}
			session->worker.request_stop();
			auto& registry = Sessions();
			std::scoped_lock lock(registry.mutex);
			registry.sessions.erase(handle.get());
			return {Status::Ok, {}};
		}
		}
		return Fail(Status::NotSupported);
	}

	void Cex2Media::ReleaseSession(std::uint64_t owner)
	{
		std::vector<std::shared_ptr<Session>> removed;
		auto& registry = Sessions();
		{
			std::scoped_lock lock(registry.mutex);
			for (auto it = registry.sessions.begin(); it != registry.sessions.end();)
			{
				if (it->second->owner != owner)
				{
					++it;
					continue;
				}
				removed.push_back(it->second);
				it = registry.sessions.erase(it);
			}
		}
		for (auto& session : removed)
		{
			{
				std::scoped_lock lock(session->mutex);
				session->stopped = true;
				session->wake.notify_all();
			}
			session->worker.request_stop();
		}
	}

	void Cex2Media::MixMicrophone(std::span<std::int16_t> samples)
	{
		auto& registry = Sessions();
		std::scoped_lock registryLock(registry.mutex);
		for (const auto& [id, session] : registry.sessions)
		{
			(void)id;
			std::scoped_lock lock(session->mutex);
			if (session->paused || session->audioRead >= session->audio.size())
				continue;
			const auto count = std::min(samples.size(), session->audio.size() - session->audioRead);
			int maximum = 32767;
			for (std::size_t i{}; i < count; ++i)
				maximum = std::max(maximum, std::abs(static_cast<int>(samples[i]) + session->audio[session->audioRead + i]));
			for (std::size_t i{}; i < count; ++i)
			{
				const int mixed = static_cast<int>(samples[i]) + session->audio[session->audioRead + i];
				samples[i] = static_cast<std::int16_t>(mixed * 32767 / maximum);
			}
			session->audioRead += count;
			if (session->audioRead > 32000U)
			{
				session->audio.erase(session->audio.begin(), session->audio.begin() + session->audioRead);
				session->audioRead = 0;
			}
			break;
		}
	}

	std::size_t Cex2Media::ActiveSessions()
	{
		auto& registry = Sessions();
		std::scoped_lock lock(registry.mutex);
		return registry.sessions.size();
	}
} // namespace cemuextend_hle
