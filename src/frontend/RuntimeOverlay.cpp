#include "frontend/RuntimeOverlay.h"

#include <algorithm>

namespace RuntimeOverlay
{
	Model& Model::Instance()
	{
		static Model model;
		return model;
	}

	Snapshot Model::GetSnapshot()
	{
		ChangeHandler handler;
		Snapshot snapshot;
		{
			std::scoped_lock lock(m_mutex);
			if (PruneExpiredLocked(std::chrono::steady_clock::now()))
				PublishLocked(handler);
			snapshot = m_snapshot;
		}
		if (handler)
			handler();
		return snapshot;
	}

	void Model::SetChangeHandler(ChangeHandler handler)
	{
		std::scoped_lock lock(m_mutex);
		m_changeHandler = std::move(handler);
	}

	void Model::ClearChangeHandler()
	{
		std::scoped_lock lock(m_mutex);
		m_changeHandler = {};
	}

	void Model::Reset()
	{
		ChangeHandler handler;
		{
			std::scoped_lock lock(m_mutex);
			const auto nextSequence = m_snapshot.sequence + 1;
			m_snapshot = {};
			m_snapshot.sequence = nextSequence;
			m_nextNoticeId = 1;
			handler = m_changeHandler;
		}
		if (handler)
			handler();
	}

	void Model::SetPresentation(TextStyle overlayStyle, TextStyle notificationStyle,
								Visibility visibility, Stats stats)
	{
		ChangeHandler handler;
		{
			std::scoped_lock lock(m_mutex);
			if (m_snapshot.overlayStyle == overlayStyle &&
				m_snapshot.notificationStyle == notificationStyle &&
				m_snapshot.visibility == visibility && m_snapshot.stats == stats)
				return;
			m_snapshot.overlayStyle = overlayStyle;
			m_snapshot.notificationStyle = notificationStyle;
			m_snapshot.visibility = visibility;
			m_snapshot.stats = std::move(stats);
			PublishLocked(handler);
		}
		if (handler)
			handler();
	}

	std::uint64_t Model::PushNotice(NoticeKind kind, std::string text,
									std::chrono::milliseconds duration,
									std::optional<std::uint32_t> player)
	{
		ChangeHandler handler;
		std::uint64_t id{};
		{
			std::scoped_lock lock(m_mutex);
			id = m_nextNoticeId++;
			m_snapshot.notices.push_back({id, kind, std::move(text), player,
										  std::chrono::steady_clock::now() + duration});
			PublishLocked(handler);
		}
		if (handler)
			handler();
		return id;
	}

	void Model::ReplaceNotices(NoticeKind kind, std::vector<Notice> notices)
	{
		ChangeHandler handler;
		{
			std::scoped_lock lock(m_mutex);
			std::erase_if(m_snapshot.notices,
						  [kind](const Notice& notice) { return notice.kind == kind; });
			for (auto& notice : notices)
			{
				if (!notice.id)
					notice.id = m_nextNoticeId++;
				m_snapshot.notices.push_back(std::move(notice));
			}
			PublishLocked(handler);
		}
		if (handler)
			handler();
	}

	void Model::SetShaderProgress(ShaderProgress progress)
	{
		ChangeHandler handler;
		{
			std::scoped_lock lock(m_mutex);
			if (m_snapshot.shaderProgress == progress)
				return;
			m_snapshot.shaderProgress = std::move(progress);
			PublishLocked(handler);
		}
		if (handler)
			handler();
	}

	void Model::SetSoftwareKeyboard(SoftwareKeyboard keyboard)
	{
		ChangeHandler handler;
		{
			std::scoped_lock lock(m_mutex);
			if (m_snapshot.keyboard == keyboard)
				return;
			m_snapshot.keyboard = std::move(keyboard);
			m_snapshot.interaction = m_snapshot.keyboard.active
										 ? Interaction::SoftwareKeyboard
										 : (m_snapshot.errorDialog.active ? Interaction::ErrorDialog
																		  : Interaction::Passive);
			PublishLocked(handler);
		}
		if (handler)
			handler();
	}

	void Model::SetErrorDialog(ErrorDialog dialog)
	{
		ChangeHandler handler;
		{
			std::scoped_lock lock(m_mutex);
			if (m_snapshot.errorDialog == dialog)
				return;
			m_snapshot.errorDialog = std::move(dialog);
			m_snapshot.interaction = m_snapshot.keyboard.active
										 ? Interaction::SoftwareKeyboard
										 : (m_snapshot.errorDialog.active ? Interaction::ErrorDialog
																		  : Interaction::Passive);
			PublishLocked(handler);
		}
		if (handler)
			handler();
	}

	void Model::PublishLocked(ChangeHandler& handler)
	{
		++m_snapshot.sequence;
		handler = m_changeHandler;
	}

	bool Model::PruneExpiredLocked(std::chrono::steady_clock::time_point now)
	{
		const auto oldSize = m_snapshot.notices.size();
		std::erase_if(m_snapshot.notices, [now](const Notice& notice) {
			return notice.expiresAt != std::chrono::steady_clock::time_point{} &&
				   notice.expiresAt <= now;
		});
		return m_snapshot.notices.size() != oldSize;
	}

	std::string_view PositionName(Position position)
	{
		switch (position)
		{
		case Position::Disabled:
			return "disabled";
		case Position::TopLeft:
			return "topLeft";
		case Position::TopCenter:
			return "topCenter";
		case Position::TopRight:
			return "topRight";
		case Position::BottomLeft:
			return "bottomLeft";
		case Position::BottomCenter:
			return "bottomCenter";
		case Position::BottomRight:
			return "bottomRight";
		}
		return "disabled";
	}

	std::string_view NoticeKindName(NoticeKind kind)
	{
		switch (kind)
		{
		case NoticeKind::Account:
			return "account";
		case NoticeKind::Controller:
			return "controller";
		case NoticeKind::Friend:
			return "friend";
		case NoticeKind::Battery:
			return "battery";
		case NoticeKind::Shader:
			return "shader";
		case NoticeKind::Pipeline:
			return "pipeline";
		case NoticeKind::Message:
			return "message";
		}
		return "message";
	}
} // namespace RuntimeOverlay
