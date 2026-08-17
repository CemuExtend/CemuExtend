#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace Application
{
	enum class GameProfileCpuMode : std::uint8_t
	{
		SingleCoreInterpreter,
		SingleCoreRecompiler,
		MultiCoreRecompiler,
		Auto,
	};

	enum class GameProfileGraphicsApi : std::uint8_t
	{
		Default,
		OpenGL,
		Vulkan,
		Metal,
	};

	struct GameProfileUpdate
	{
		bool loadSharedLibraries{true};
		bool startWithPadView{};
		GameProfileCpuMode cpuMode{GameProfileCpuMode::Auto};
		std::uint32_t threadQuantum{45000};
		GameProfileGraphicsApi graphicsApi{GameProfileGraphicsApi::Default};
		bool accurateShaderMultiplication{true};
		bool shaderFastMath{true};
		std::uint8_t metalBufferCacheMode{};
		std::uint8_t positionInvariance{};
		std::array<std::optional<std::string>, 8> controllerProfiles;
	};

	struct GameProfileView
	{
		GameProfileUpdate settings;
		std::optional<std::string> gameName;
		bool defaultProfile{true};
	};

	struct GameProfileSaveResult
	{
		bool saved{};
		std::string diagnostic;

		[[nodiscard]] explicit operator bool() const { return saved; }
	};

	class IGameProfileService
	{
	public:
		virtual ~IGameProfileService() = default;
		[[nodiscard]] virtual GameProfileView LoadGameProfile(
			std::uint64_t titleId) const = 0;
		[[nodiscard]] virtual GameProfileSaveResult SaveGameProfile(
			std::uint64_t titleId, const GameProfileUpdate& update) = 0;
	};
}
