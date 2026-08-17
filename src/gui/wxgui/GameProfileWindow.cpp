#include "wxgui/GameProfileWindow.h"

#include "application/EmulationController.h"

#include <wx/statbox.h>
#include <wx/sizer.h>
#include <wx/panel.h>
#include <wx/notebook.h>
#include <wx/stattext.h>
#include <wx/wupdlock.h>
#include <wx/slider.h>

#include "wxgui/helpers/wxHelpers.h"
#include "input/InputManager.h"

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#include "resource/embedded/resources.h"
#endif

GameProfileWindow::GameProfileWindow(wxWindow* parent,
	Application::EmulationController& emulationController, uint64_t title_id)
	: wxFrame(parent, wxID_ANY, _("Edit game profile"), wxDefaultPosition,
		wxSize{390, 350}, wxCLOSE_BOX | wxCLIP_CHILDREN | wxCAPTION |
			wxRESIZE_BORDER | wxTAB_TRAVERSAL | wxSYSTEM_MENU),
	  m_title_id(title_id), m_emulationController(emulationController)
{
	SetIcon(wxICON(X_GAME_PROFILE));

	const auto profile = m_emulationController.LoadGameProfile(title_id);
	m_profile = profile.settings;
	if (profile.gameName)
		this->SetTitle(_("Edit game profile") + " - " + *profile.gameName);

	this->SetSizeHints(wxDefaultSize, wxDefaultSize);

	auto* main_sizer = new wxBoxSizer(wxVERTICAL);

	auto* m_notebook = new wxNotebook(this, wxID_ANY);
	// general
	{
		auto* panel = new wxPanel(m_notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
		auto* sizer = new wxBoxSizer(wxVERTICAL);
		{
			auto* box_sizer = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _("General")), wxVERTICAL);
			auto* box = box_sizer->GetStaticBox();

			m_load_libs = new wxCheckBox(box, wxID_ANY, _("Load shared libraries"));
			m_load_libs->SetToolTip(_("EXPERT OPTION\nThis option will load libraries from the cafeLibs directory"));
			box_sizer->Add(m_load_libs, 0, wxALL, 5);

			m_start_with_padview = new wxCheckBox(box, wxID_ANY, _("Launch with gamepad view"));
			m_start_with_padview->SetToolTip(_("Games will be launched with gamepad view toggled as default. The view can be toggled with CTRL + TAB"));
			box_sizer->Add(m_start_with_padview, 0, wxALL, 5);

			sizer->Add(box_sizer, 0, wxEXPAND, 5);
		}
		// cpu
		{
			auto* box_sizer = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _("CPU")), wxVERTICAL);
			auto* box = box_sizer->GetStaticBox();

			auto* first_row = new wxFlexGridSizer(0, 2, 0, 0);
			first_row->SetFlexibleDirection(wxBOTH);
			first_row->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

			first_row->Add(new wxStaticText(box, wxID_ANY, _("Mode")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

			wxString cpu_modes[] = { _("Single-core interpreter"), _("Single-core recompiler"), _("Multi-core recompiler"), _("Auto (recommended)") };
			const sint32 m_cpu_modeNChoices = std::size(cpu_modes);
			m_cpu_mode = new wxChoice(box, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_cpu_modeNChoices, cpu_modes, 0);
			m_cpu_mode->SetToolTip(_("Set the CPU emulation mode"));
			first_row->Add(m_cpu_mode, 0, wxALL, 5);

			first_row->Add(new wxStaticText(box, wxID_ANY, _("Thread quantum")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

			auto* quantum_sizer = new wxFlexGridSizer(0, 2, 0, 0);
			quantum_sizer->SetFlexibleDirection(wxBOTH);
			quantum_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

			wxString quantum_values[] = { "20000", "45000", "60000", "80000" ,"100000" };
			m_thread_quantum = new wxChoice(box, wxID_ANY, wxDefaultPosition, wxDefaultSize, std::size(quantum_values), quantum_values);
			m_thread_quantum->SetMinSize(wxSize(85, -1));
			m_thread_quantum->SetToolTip(_("EXPERT OPTION\nSet the maximum thread slice runtime (in virtual cycles)"));
			quantum_sizer->Add(m_thread_quantum, 0, wxALL, 5);

			quantum_sizer->Add(new wxStaticText(box, wxID_ANY, _("cycles")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

			first_row->Add(quantum_sizer, 0, wxEXPAND, 5);

			box_sizer->Add(first_row, 0, wxEXPAND, 5);


			sizer->Add(box_sizer, 0, wxEXPAND, 5);
		}

		panel->SetSizer(sizer);
		panel->Layout();
		sizer->Fit(panel);
		m_notebook->AddPage(panel, _("General"), true);
	}

	// graphic
	{
		auto* panel = new wxPanel(m_notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
		auto* sizer = new wxBoxSizer(wxVERTICAL);

		//m_extended_texture_readback = new wxCheckBox(panel, wxID_ANY, _("Extended texture readback"));
		//m_extended_texture_readback->SetToolTip(_("Improves emulation accuracy of CPU to GPU memory access at the cost of performance. Required for some games."));
		//sizer->Add(m_extended_texture_readback, 0, wxALL, 5);

		auto* first_row = new wxFlexGridSizer(0, 2, 0, 0);
		first_row->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

		/*first_row->Add(new wxStaticText(panel, wxID_ANY, _("Precompiled shaders")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
		wxString precompiled_modes[] = { _("auto"), _("enable"), _("disable") };
		const sint32 precompiled_count = std::size(precompiled_modes);
		m_precompiled = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, precompiled_count, precompiled_modes, 0);
		m_precompiled->SetToolTip(_("Precompiled shaders can speed up the load time on the shader loading screen.\nAuto will enable it for AMD/Intel but disable it for NVIDIA GPUs as a workaround for a driver bug.\n\nRecommended: Auto"));
		first_row->Add(m_precompiled, 0, wxALL, 5);*/

		first_row->Add(new wxStaticText(panel, wxID_ANY, _("Graphics API")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

		wxString gapi_values[] = { "", "OpenGL", "Vulkan",
#ifdef ENABLE_METAL
            "Metal"
#endif
		};
		m_graphic_api = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, (int)std::size(gapi_values), gapi_values);
		first_row->Add(m_graphic_api, 0, wxALL, 5);

		first_row->Add(new wxStaticText(panel, wxID_ANY, _("Shader multiplication accuracy")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

		wxString mul_values[] = { _("false"), _("true")};
		m_shader_mul_accuracy = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, (int)std::size(mul_values), mul_values);
		m_shader_mul_accuracy->SetToolTip(_("EXPERT OPTION\nControls the accuracy of floating point multiplication in shaders.\n\nRecommended: true"));
		first_row->Add(m_shader_mul_accuracy, 0, wxALL, 5);

#ifdef ENABLE_METAL
		first_row->Add(new wxStaticText(panel, wxID_ANY, _("Shader fast math")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

		wxString math_values[] = { _("false"), _("true") };
		m_shader_fast_math = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, (int)std::size(math_values), math_values);
		m_shader_fast_math->SetToolTip(_("EXPERT OPTION\nEnables fast math for all shaders. May (rarely) cause graphical bugs.\n\nMetal only\n\nRecommended: true"));
		first_row->Add(m_shader_fast_math, 0, wxALL, 5);

		first_row->Add(new wxStaticText(panel, wxID_ANY, _("Metal buffer cache mode")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

		wxString cache_values[] = { _("auto"), _("device private"), _("device shared"), _("host") };
		m_metal_buffer_cache_mode = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, (int)std::size(cache_values), cache_values);
		m_metal_buffer_cache_mode->SetToolTip(_("EXPERT OPTION\nDecides how the buffer cache memory will be managed.\n\nMetal only\n\nRecommended: auto"));
		first_row->Add(m_metal_buffer_cache_mode, 0, wxALL, 5);

		first_row->Add(new wxStaticText(panel, wxID_ANY, _("Position invariance")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

		wxString pos_values[] = { _("auto"), _("false"), _("true") };
		m_position_invariance = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, (int)std::size(pos_values), pos_values);
		m_position_invariance->SetToolTip(_("EXPERT OPTION\nDisables most optimizations for vertex positions. May fix polygon cutouts or flickering in some games.\n\nMetal only\n\nRecommended: auto"));
		first_row->Add(m_position_invariance, 0, wxALL, 5);
#endif

		/*first_row->Add(new wxStaticText(panel, wxID_ANY, _("GPU buffer cache accuracy")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
		wxString accuarcy_values[] = { _("high"), _("medium"), _("low") };
		m_cache_accuracy = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, (int)std::size(accuarcy_values), accuarcy_values);
		m_cache_accuracy->SetToolTip(_("Lower value results in higher performance, but may cause graphical issues"));
		first_row->Add(m_cache_accuracy, 0, wxALL, 5);*/

		sizer->Add(first_row, 0, wxEXPAND, 5);


		panel->SetSizer(sizer);
		panel->Layout();
		sizer->Fit(panel);
		m_notebook->AddPage(panel, _("Graphic"), false);
	}

	//// audio
	//{
	//	auto panel = new wxPanel(m_notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
	//	wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

	//	m_disable_audio = new wxCheckBox(panel, wxID_ANY, _("Disable Audio"), wxDefaultPosition, wxDefaultSize, wxCHK_3STATE | wxCHK_ALLOW_3RD_STATE_FOR_USER);
	//	sizer->Add(m_disable_audio, 0, wxALL, 5);


	//	panel->SetSizer(sizer);
	//	panel->Layout();
	//	sizer->Fit(panel);
	//	m_notebook->AddPage(panel, _("Audio"), false);
	//}

	// controller
	{
		auto panel = new wxPanel(m_notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
		wxBoxSizer* sizer;
		sizer = new wxBoxSizer(wxVERTICAL);

		wxFlexGridSizer* profile_sizer;
		profile_sizer = new wxFlexGridSizer(0, 2, 0, 0);
		profile_sizer->SetFlexibleDirection(wxBOTH);
		profile_sizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

		for (int i = 0; i < 8; ++i)
		{
			profile_sizer->Add(new wxStaticText(panel, wxID_ANY, formatWxString(_("Controller {}"), i + 1)), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

			m_controller_profile[i] = new wxComboBox(panel, wxID_ANY,"", wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_DROPDOWN| wxCB_READONLY);
			m_controller_profile[i]->SetMinSize(wxSize(250, -1));
			m_controller_profile[i]->Bind(wxEVT_COMBOBOX_DROPDOWN, &GameProfileWindow::OnControllerProfileDropdown, this);
			m_controller_profile[i]->SetToolTip(_("Forces a given controller profile"));
			profile_sizer->Add(m_controller_profile[i], 0, wxALL, 5);
		}

		sizer->Add(profile_sizer, 0, wxEXPAND, 5);

		panel->SetSizer(sizer);
		panel->Layout();
		sizer->Fit(panel);
		m_notebook->AddPage(panel, _("Controller"), false);
	}

	main_sizer->Add(m_notebook, 1, wxEXPAND | wxALL, 5);


	this->SetSizer(main_sizer);
	this->Layout();

	this->Centre(wxBOTH);

	ApplyProfile();
}

GameProfileWindow::~GameProfileWindow()
{
	SaveProfile();
}

void GameProfileWindow::OnStreamoutSizeChange(wxCommandEvent& event)
{
	wxSlider* slider = wxDynamicCast(event.GetEventObject(), wxSlider);
	wxASSERT(slider);
	wxStaticText* text = wxDynamicCast(slider->GetClientData(), wxStaticText);
	wxASSERT(text);
	text->SetLabelText(fmt::format("{} MB", slider->GetValue()));
	event.Skip();
}

void GameProfileWindow::OnControllerProfileDropdown(wxCommandEvent& event)
{
	wxComboBox* cb = wxDynamicCast(event.GetEventObject(), wxComboBox);
	wxASSERT(cb);

	wxWindowUpdateLocker lock(cb);

	const auto selected_value = cb->GetStringSelection();
	cb->Clear();
	cb->Append(wxEmptyString);

	auto profiles = InputManager::get_profiles();
	for (const auto& profile : profiles)
	{
		cb->Append(wxString::FromUTF8(profile));
	}

	cb->SetStringSelection(selected_value);
}

void GameProfileWindow::ApplyProfile()
{
	// general
	m_load_libs->SetValue(m_profile.loadSharedLibraries);
	m_start_with_padview->SetValue(m_profile.startWithPadView);

	// cpu
	// wxString cpu_modes[] = { _("Singlecore-Interpreter"), _("Singlecore-Recompiler"), _("Triplecore-Recompiler"), _("Auto (recommended)") };
	switch (m_profile.cpuMode)
	{
	case Application::GameProfileCpuMode::SingleCoreInterpreter:
		m_cpu_mode->SetSelection(0); break;
	case Application::GameProfileCpuMode::SingleCoreRecompiler:
		m_cpu_mode->SetSelection(1); break;
	case Application::GameProfileCpuMode::MultiCoreRecompiler:
		m_cpu_mode->SetSelection(2); break;
	default: m_cpu_mode->SetSelection(3);
	}

	m_thread_quantum->SetStringSelection(fmt::format("{}", m_profile.threadQuantum));

	// gpu
	switch (m_profile.graphicsApi)
	{
	case Application::GameProfileGraphicsApi::OpenGL: m_graphic_api->SetSelection(1); break;
	case Application::GameProfileGraphicsApi::Vulkan: m_graphic_api->SetSelection(2); break;
#ifdef ENABLE_METAL
	case Application::GameProfileGraphicsApi::Metal: m_graphic_api->SetSelection(3); break;
#endif
	default: m_graphic_api->SetSelection(0); break;
	}
	m_shader_mul_accuracy->SetSelection(m_profile.accurateShaderMultiplication ? 1 : 0);
#ifdef ENABLE_METAL
	m_shader_fast_math->SetSelection(m_profile.shaderFastMath ? 1 : 0);
	m_metal_buffer_cache_mode->SetSelection(m_profile.metalBufferCacheMode);
	m_position_invariance->SetSelection(m_profile.positionInvariance);
#endif

	//// audio
	//m_disable_audio->Set3StateValue(GetCheckboxState(m_game_profile.disableAudio));

	// controller
	auto profiles = InputManager::get_profiles();

	for (const auto& cb : m_controller_profile)
	{
		cb->Clear();
		for (const auto& profile : profiles)
		{
			cb->Append(wxString::FromUTF8(profile));
		}
	}

	for (int i = 0; i < InputManager::kMaxController; ++i)
	{
		const bool has_value = m_profile.controllerProfiles[i].has_value();
		if (has_value)
		{
			const auto& v = m_profile.controllerProfiles[i].value();
			m_controller_profile[i]->SetStringSelection(wxString::FromUTF8(v));
		}

		else
			m_controller_profile[i]->SetSelection(wxNOT_FOUND);
	}
}

void GameProfileWindow::SaveProfile()
{
	// general
	m_profile.loadSharedLibraries = m_load_libs->GetValue();
	m_profile.startWithPadView = m_start_with_padview->GetValue();

	// cpu
	switch(m_cpu_mode->GetSelection())
	{
	case 0: m_profile.cpuMode = Application::GameProfileCpuMode::SingleCoreInterpreter; break;
	case 1: m_profile.cpuMode = Application::GameProfileCpuMode::SingleCoreRecompiler; break;
	case 2: m_profile.cpuMode = Application::GameProfileCpuMode::MultiCoreRecompiler; break;
	default:
		m_profile.cpuMode = Application::GameProfileCpuMode::Auto;
	}


	const wxString thread_quantum = m_thread_quantum->GetStringSelection();
	m_profile.threadQuantum = 45000;
	if (!thread_quantum.empty())
	{
		m_profile.threadQuantum = ConvertString<uint32>(thread_quantum.ToStdString());
		m_profile.threadQuantum = std::min<uint32>(m_profile.threadQuantum, 536870912);
		m_profile.threadQuantum = std::max<uint32>(m_profile.threadQuantum, 5000);
	}

	// gpu
	m_profile.accurateShaderMultiplication = m_shader_mul_accuracy->GetSelection() != 0;
#ifdef ENABLE_METAL
	m_profile.shaderFastMath = m_shader_fast_math->GetSelection() != 0;
	m_profile.metalBufferCacheMode = static_cast<std::uint8_t>(
		m_metal_buffer_cache_mode->GetSelection());
	m_profile.positionInvariance = static_cast<std::uint8_t>(
		m_position_invariance->GetSelection());
#endif

	switch (m_graphic_api->GetSelection())
	{
	case 1: m_profile.graphicsApi = Application::GameProfileGraphicsApi::OpenGL; break;
	case 2: m_profile.graphicsApi = Application::GameProfileGraphicsApi::Vulkan; break;
#ifdef ENABLE_METAL
	case 3: m_profile.graphicsApi = Application::GameProfileGraphicsApi::Metal; break;
#endif
	default: m_profile.graphicsApi = Application::GameProfileGraphicsApi::Default; break;
	}

	// controller
	for (int i = 0; i < 8; ++i)
	{
		if(m_controller_profile[i]->GetSelection() == wxNOT_FOUND)
		{
			m_profile.controllerProfiles[i].reset();
			continue;
		}

		const wxString profile_name = m_controller_profile[i]->GetStringSelection();
		if (profile_name.empty())
			m_profile.controllerProfiles[i].reset();
		else
			m_profile.controllerProfiles[i] = profile_name.ToUTF8();
	}

	const auto result = m_emulationController.SaveGameProfile(m_title_id, m_profile);
	if (!result)
		cemuLog_log(LogType::Force, "Unable to save game profile: {}", result.diagnostic);
}

void GameProfileWindow::SetSliderValue(wxSlider* slider, sint32 new_value) const
{
	wxASSERT(slider);
	slider->SetValue(new_value);

	wxCommandEvent slider_event(wxEVT_SLIDER, slider->GetId());
	slider_event.SetEventObject(slider);
	slider_event.SetClientData((void*)IsFrozen());
	wxPostEvent(slider->GetEventHandler(), slider_event);
}
