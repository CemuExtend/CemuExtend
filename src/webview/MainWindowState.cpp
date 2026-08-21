#include "webview/MainWindowState.h"

namespace WebFrontend
{
	MainWindowState::MainWindowState(std::uintptr_t identity)
	{
		m_state.mainWindowIdentity = identity;
	}

	bool MainWindowState::BeginLaunch()
	{
		std::scoped_lock lock(m_mutex);
		if (m_state.mode != MainWindowContentMode::Library)
			return false;
		m_state.mode = MainWindowContentMode::LaunchPending;
		++m_state.generation;
		return true;
	}

	bool MainWindowState::CommitLaunch()
	{
		std::scoped_lock lock(m_mutex);
		if (m_state.mode != MainWindowContentMode::LaunchPending)
			return false;
		m_state.webViewVisible = false;
		m_state.renderSurfaceVisible = true;
		m_state.mode = MainWindowContentMode::Playing;
		++m_state.generation;
		return true;
	}

	bool MainWindowState::RollbackLaunch()
	{
		std::scoped_lock lock(m_mutex);
		if (m_state.mode != MainWindowContentMode::LaunchPending &&
			m_state.mode != MainWindowContentMode::Playing)
			return false;
		m_state.renderSurfaceVisible = false;
		m_state.webViewVisible = true;
		m_state.mode = MainWindowContentMode::Library;
		++m_state.generation;
		return true;
	}

	bool MainWindowState::FinishEmulation()
	{
		std::scoped_lock lock(m_mutex);
		if (m_state.mode != MainWindowContentMode::Playing)
			return false;
		m_state.renderSurfaceVisible = false;
		m_state.webViewVisible = true;
		m_state.mode = MainWindowContentMode::Library;
		++m_state.generation;
		return true;
	}

	bool MainWindowState::BeginShutdown()
	{
		std::scoped_lock lock(m_mutex);
		if (m_state.mode == MainWindowContentMode::ShuttingDown)
			return false;
		m_state.mode = MainWindowContentMode::ShuttingDown;
		m_state.webViewVisible = false;
		m_state.renderSurfaceVisible = false;
		++m_state.generation;
		return true;
	}

	MainWindowSnapshot MainWindowState::Snapshot() const
	{
		std::scoped_lock lock(m_mutex);
		return m_state;
	}
}
