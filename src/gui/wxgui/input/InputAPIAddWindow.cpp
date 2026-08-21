#include "wxgui/input/InputAPIAddWindow.h"

#include "input/InputManager.h"
#include "wxgui/helpers/wxCustomData.h"
#include "wxgui/helpers/wxHelpers.h"
#include "input/api/Controller.h"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/choice.h>
#include <wx/statline.h>
#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/msgdlg.h>
#include <wx/wupdlock.h>

#include "input/ControllerFactory.h"

using wxTypeData = wxCustomData<InputAPI::Type>;
using wxControllerData = wxCustomData<ControllerPtr>;

namespace
{
	struct AsyncSearchResult
	{
		std::uint64_t generation{};
		InputAPI::Type type{};
		std::string selectedUuid;
		std::vector<ControllerPtr> controllers;
	};
}

struct InputAPIAddWindow::AsyncThreadData
{
	std::mutex mutex;
	bool complete{};
	bool canceled{};
	std::optional<AsyncSearchResult> result;
	std::string error;
};

InputAPIAddWindow::InputAPIAddWindow(wxWindow* parent, const wxPoint& position,
                                     const std::vector<ControllerPtr>& controllers)
	: wxDialog(parent, wxID_ANY, "Add input API", position, wxDefaultSize, wxCAPTION),
	  m_controllers(controllers), m_search_timer(this)
{
	this->SetSizeHints(wxDefaultSize, wxDefaultSize);

	auto* sizer = new wxBoxSizer(wxVERTICAL);

	{
		auto* api_row = new wxFlexGridSizer(2);

		// API
		api_row->Add(new wxStaticText(this, wxID_ANY, _("API")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

		m_input_api = new wxChoice(this, wxID_ANY);
		auto& providers = InputManager::instance().get_api_providers();
		for (const auto& p : providers)
		{
			if (p.empty())
				continue;

			const auto provider = *p.begin();
			m_input_api->Append(wxString::FromUTF8(provider->api_name()), new wxTypeData(provider->api()));
		}

		m_input_api->Bind(wxEVT_CHOICE, &InputAPIAddWindow::on_api_selected, this);
		api_row->Add(m_input_api, 1, wxALL | wxEXPAND, 5);

		// Controller
		api_row->Add(new wxStaticText(this, wxID_ANY, _("Controller")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

		m_controller_list = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr,
		                                   wxCB_READONLY);
		m_controller_list->Bind(wxEVT_COMBOBOX_DROPDOWN, &InputAPIAddWindow::on_controller_dropdown, this);
		m_controller_list->Bind(wxEVT_COMBOBOX, &InputAPIAddWindow::on_controller_selected, this);
		m_controller_list->SetMinSize(wxSize(240, -1));
		m_controller_list->Disable();
		api_row->Add(m_controller_list, 1, wxALL | wxEXPAND, 5);

		sizer->Add(api_row, 0, wxEXPAND, 5);
	}

	sizer->Add(new wxStaticLine(this), 0, wxEXPAND);

	{
		auto* end_row = new wxBoxSizer(wxHORIZONTAL);

		m_ok_button = new wxButton(this, wxID_ANY, _("Add"));
		m_ok_button->Bind(wxEVT_BUTTON, &InputAPIAddWindow::on_add_button, this);
		m_ok_button->Disable();
		end_row->Add(m_ok_button, 0, wxALL, 5);

		auto* cancel_button = new wxButton(this, wxID_ANY, _("Cancel"));
		cancel_button->Bind(wxEVT_BUTTON, &InputAPIAddWindow::on_close_button, this);
		end_row->Add(cancel_button, 0, wxALL, 5);

		sizer->Add(end_row, 0, wxEXPAND, 5);
	}

	{
		// optional settings
		m_settings_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
		auto* panel_sizer = new wxBoxSizer(wxVERTICAL);
		panel_sizer->Add(new wxStaticLine(m_settings_panel), 0, wxEXPAND, 0);

		{
			auto* row = new wxBoxSizer(wxHORIZONTAL);
			// we only have dsu settings atm, so add elements now
			row->Add(new wxStaticText(m_settings_panel, wxID_ANY, _("IP")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
			m_ip = new wxTextCtrl(m_settings_panel, wxID_ANY, "127.0.0.1");
			row->Add(m_ip, 0, wxALL, 5);

			row->Add(new wxStaticText(m_settings_panel, wxID_ANY, _("Port")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
			m_port = new wxTextCtrl(m_settings_panel, wxID_ANY, "26760");
			row->Add(m_port, 0, wxALL, 5);

			panel_sizer->Add(row, 0, wxEXPAND);
		}

		m_settings_panel->SetSizer(panel_sizer);
		m_settings_panel->Layout();
		m_settings_panel->Hide();

		sizer->Add(m_settings_panel, 1, wxEXPAND);
	}

	this->SetSizer(sizer);
	this->Layout();
	sizer->Fit(this);

	this->Bind(wxEVT_TIMER, &InputAPIAddWindow::on_search_timer, this, m_search_timer.GetId());
	m_search_timer.Start(50);
}

InputAPIAddWindow::~InputAPIAddWindow()
{
	discard_thread_result();
	m_search_timer.Stop();
}

void InputAPIAddWindow::on_add_button(wxCommandEvent& event)
{
	const auto selection = m_input_api->GetSelection();
	if (selection == wxNOT_FOUND)
	{
		cemu_assert_debug(false);
		EndModal(wxID_CANCEL);
		return;
	}

	for (const auto& c : m_controllers)
	{
		if (*c == *m_controller)
		{
			wxMessageBox(_("The controller is already added!"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
			return;
		}
	}

	m_type = static_cast<wxTypeData*>(m_input_api->GetClientObject(selection))->get();
	EndModal(wxID_OK);
}

void InputAPIAddWindow::on_close_button(wxCommandEvent& event)
{
	EndModal(wxID_CANCEL);
}

bool InputAPIAddWindow::has_custom_settings() const
{
	const auto selection = m_input_api->GetStringSelection();
	return selection == wxString::FromUTF8(to_string(InputAPI::DSUClient));
}

std::unique_ptr<ControllerProviderSettings> InputAPIAddWindow::get_settings() const
{
	if (!has_custom_settings())
		return {};

	return std::make_unique<DSUProviderSettings>(m_ip->GetValue().ToStdString(),
	                                             ConvertString<uint16>(m_port->GetValue().ToStdString()));
}

void InputAPIAddWindow::on_api_selected(wxCommandEvent& event)
{
	discard_thread_result();

	if (m_input_api->GetSelection() == wxNOT_FOUND)
		return;

	m_controller_list->Enable();
	m_controller_list->SetSelection(wxNOT_FOUND);

    const auto selection = m_input_api->GetStringSelection();
	// keyboard is a special case, as theres only one device supported atm
	if (selection == wxString::FromUTF8(to_string(InputAPI::Keyboard)))
	{
		const auto controllers = InputManager::instance().get_api_provider(InputAPI::Keyboard)->get_controllers();
		if (!controllers.empty())
		{
			m_controller = controllers[0];
			m_ok_button->Enable();

			m_controller_list->Clear();
			const auto display_name = controllers[0]->display_name();
			const auto index = m_controller_list->Append(display_name, new wxCustomData(controllers[0]));
			m_controller_list->SetSelection(index);
		}
	}
    else
    {
#if BOOST_OS_LINUX || BOOST_OS_BSD
        // We rely on the wxEVT_COMBOBOX_DROPDOWN event to trigger filling the controller list,
        // but on wxGTK the dropdown button cannot be clicked if the list is empty
        // so as a quick and dirty workaround we fill the list here
        wxCommandEvent tmpCmdEvt;
        on_controller_dropdown(tmpCmdEvt);
#endif
    }

	const auto show_settings = has_custom_settings();
	// dsu has special settings for ip/port
	if (show_settings != m_settings_panel->IsShown())
	{
		wxWindowUpdateLocker locker(this);
		m_settings_panel->Show(show_settings);
		Layout();
		Fit();
	}
}

void InputAPIAddWindow::on_controller_dropdown(wxCommandEvent& event)
{
	if (m_search_running)
		return;

	int selection = m_input_api->GetSelection();
	if (selection == wxNOT_FOUND)
		return;

	const auto type = static_cast<wxAPIType*>(m_input_api->GetClientObject(selection))->get();
	auto settings = get_settings();

	ControllerProviderPtr provider;
	if (settings)
		provider = InputManager::instance().get_api_provider(type, *settings);
	else
		provider = InputManager::instance().get_api_provider(type);

	if (!provider)
		return;

	std::string selected_uuid;
	selection = m_controller_list->GetSelection();
	if (selection != wxNOT_FOUND)
	{
		// TODO selected_uuid
	}

	m_search_running = true;
	const auto generation = ++m_search_generation;

	wxWindowUpdateLocker lock(m_controller_list);
	m_controller_list->Clear();

	m_controller_list->Append(_("Searching for controllers..."), (wxClientData*)nullptr);
	m_controller_list->SetSelection(wxNOT_FOUND);

	m_search_thread_data = std::make_unique<AsyncThreadData>();
	m_search_thread = std::thread([provider, selected_uuid, generation](std::shared_ptr<AsyncThreadData> data)
	{
		try
		{
			auto availableControllers = provider->get_controllers();
			std::lock_guard lock{data->mutex};
			if (!data->canceled)
				data->result = AsyncSearchResult{generation, provider->api(), selected_uuid,
					std::move(availableControllers)};
			data->complete = true;
		}
		catch (const std::exception& error)
		{
			std::lock_guard lock{data->mutex};
			if (!data->canceled)
				data->error = error.what();
			data->complete = true;
		}
		catch (...)
		{
			std::lock_guard lock{data->mutex};
			if (!data->canceled)
				data->error = "Unknown controller enumeration error";
			data->complete = true;
		}
	}, m_search_thread_data);
}

void InputAPIAddWindow::on_controller_selected(wxCommandEvent& event)
{
	if (m_search_running)
	{
		return;
	}

	const auto selection = m_controller_list->GetSelection();
	if (selection == wxNOT_FOUND)
	{
		return;
	}

	if (auto* controller = (wxControllerData*)m_controller_list->GetClientObject(selection))
	{
		m_controller = controller->ref();
		m_ok_button->Enable();
	}
}

void InputAPIAddWindow::on_controllers_refreshed(wxCommandEvent& event)
{
	const auto result = static_cast<wxCustomData<AsyncSearchResult>*>(
		event.GetClientObject())->get();
	if (result.generation != m_search_generation)
		return;
	wxASSERT(0 <= result.type && result.type < InputAPI::MAX);
	m_search_running = false;
	if (m_search_thread.joinable())
		m_search_thread.join();
	m_search_thread_data.reset();

	bool item_selected = false;

	wxWindowUpdateLocker lock(m_controller_list);
	m_controller_list->Clear();
	for (const auto& c : result.controllers)
	{
		const auto display_name = c->display_name();
		const auto uuid = c->uuid();
		const auto index = m_controller_list->Append(display_name, new wxCustomData(c));
		if (!item_selected && result.selectedUuid == uuid)
		{
			m_controller_list->SetSelection(index);
			item_selected = true;
		}
	}
}

void InputAPIAddWindow::on_search_timer(wxTimerEvent& event)
{
	if (!m_search_thread_data)
		return;
	std::optional<AsyncSearchResult> result;
	std::string error;
	{
		std::lock_guard lock{m_search_thread_data->mutex};
		if (!m_search_thread_data->complete)
			return;
		result = std::move(m_search_thread_data->result);
		error = std::move(m_search_thread_data->error);
	}
	if (m_search_thread.joinable())
		m_search_thread.join();
	m_search_thread_data.reset();
	m_search_running = false;
	if (!error.empty())
	{
		cemuLog_log(LogType::Force, "Controller enumeration failed: {}", error);
		m_controller_list->Clear();
		m_controller_list->Append(_("Controller search failed"), (wxClientData*)nullptr);
		return;
	}
	if (!result || result->generation != m_search_generation)
		return;
	wxCommandEvent refreshed;
	refreshed.SetClientObject(new wxCustomData<AsyncSearchResult>(std::move(*result)));
	on_controllers_refreshed(refreshed);
}

void InputAPIAddWindow::discard_thread_result()
{
	m_search_running = false;
	++m_search_generation;
	if(m_search_thread_data)
	{
		std::lock_guard lock{m_search_thread_data->mutex};
		m_search_thread_data->canceled = true;
	}
	if (m_search_thread.joinable())
	{
		bool complete = false;
		if (m_search_thread_data)
		{
			std::lock_guard lock{m_search_thread_data->mutex};
			complete = m_search_thread_data->complete;
		}
		if (complete)
			m_search_thread.join();
		else
			m_search_thread.detach();
	}
	m_search_thread_data.reset();
}
