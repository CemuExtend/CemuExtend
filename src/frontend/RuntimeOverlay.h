#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RuntimeOverlay
{
	enum class Position : std::uint8_t
	{
		Disabled,
		TopLeft,
		TopCenter,
		TopRight,
		BottomLeft,
		BottomCenter,
		BottomRight,
	};

	enum class NoticeKind : std::uint8_t
	{
		Account,
		Controller,
		Friend,
		Battery,
		Shader,
		Pipeline,
		Message,
	};

	enum class Interaction : std::uint8_t
	{
		Passive,
		SoftwareKeyboard,
		ErrorDialog,
	};

	struct TextStyle
	{
		Position position{Position::Disabled};
		std::uint32_t color{0xFFFFFFFF};
		std::uint32_t scale{100};
		bool operator==(const TextStyle&) const = default;
	};

	struct Stats
	{
		double fps{};
		std::uint32_t drawCalls{};
		std::uint32_t fastDrawCalls{};
		double cpuUsage{};
		std::vector<double> cpuPerCore;
		std::uint32_t ramUsageMb{};
		std::int32_t vramUsageMb{-1};
		std::int32_t vramTotalMb{-1};
		std::vector<std::pair<std::string, std::string>> debugLines;
		bool operator==(const Stats&) const = default;
	};

	struct Visibility
	{
		bool fps{true};
		bool drawCalls{};
		bool cpuUsage{};
		bool cpuPerCore{};
		bool ramUsage{};
		bool vramUsage{};
		bool debug{};
		bool operator==(const Visibility&) const = default;
	};

	struct Notice
	{
		std::uint64_t id{};
		NoticeKind kind{NoticeKind::Message};
		std::string text;
		std::optional<std::uint32_t> player;
		std::chrono::steady_clock::time_point expiresAt{};
		bool operator==(const Notice&) const = default;
	};

	struct ShaderProgress
	{
		std::uint64_t generation{};
		bool visible{};
		bool pipelines{};
		std::uint32_t current{};
		std::uint32_t total{};
		std::uint32_t vertexShaders{};
		std::uint32_t pixelShaders{};
		std::uint32_t geometryShaders{};
		std::shared_ptr<const std::string> backgroundImageTv;
		std::shared_ptr<const std::string> backgroundImagePad;
		bool operator==(const ShaderProgress&) const = default;
	};

	struct SoftwareKeyboard
	{
		std::uint64_t generation{};
		bool active{};
		bool keyboardOnly{};
		bool shifted{};
		std::uint32_t maximumLength{};
		std::string text;
		bool operator==(const SoftwareKeyboard&) const = default;
	};

	struct ErrorDialog
	{
		std::uint64_t generation{};
		bool active{};
		std::string title;
		std::string message;
		std::string leftButton;
		std::string rightButton;
		float opacity{1.0f};
		bool operator==(const ErrorDialog&) const = default;
	};

	struct Snapshot
	{
		std::uint64_t sequence{};
		TextStyle overlayStyle;
		TextStyle notificationStyle{Position::TopLeft};
		Visibility visibility;
		Stats stats;
		std::vector<Notice> notices;
		ShaderProgress shaderProgress;
		SoftwareKeyboard keyboard;
		ErrorDialog errorDialog;
		Interaction interaction{Interaction::Passive};
		bool operator==(const Snapshot&) const = default;
	};

	class Model final
	{
	  public:
		using ChangeHandler = std::function<void()>;

		static Model& Instance();

		[[nodiscard]] Snapshot GetSnapshot();
		void SetChangeHandler(ChangeHandler handler);
		void ClearChangeHandler();
		void Reset();

		void SetPresentation(TextStyle overlayStyle, TextStyle notificationStyle,
							 Visibility visibility, Stats stats);
		std::uint64_t PushNotice(NoticeKind kind, std::string text,
								 std::chrono::milliseconds duration,
								 std::optional<std::uint32_t> player = {});
		void ReplaceNotices(NoticeKind kind, std::vector<Notice> notices);
		void SetShaderProgress(ShaderProgress progress);
		void SetSoftwareKeyboard(SoftwareKeyboard keyboard);
		void SetErrorDialog(ErrorDialog dialog);

	  private:
		void PublishLocked(ChangeHandler& handler);
		bool PruneExpiredLocked(std::chrono::steady_clock::time_point now);

		std::mutex m_mutex;
		Snapshot m_snapshot;
		std::uint64_t m_nextNoticeId{1};
		ChangeHandler m_changeHandler;
	};

	[[nodiscard]] std::string_view PositionName(Position position);
	[[nodiscard]] std::string_view NoticeKindName(NoticeKind kind);
} // namespace RuntimeOverlay
