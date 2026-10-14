#include "imgui_extension.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "resource/IconsFontAwesome5.h"
#include "resource/resource.h"
#ifdef ENABLE_OPENGL
#include "imgui_impl_opengl3.h"
#endif
#ifdef ENABLE_VULKAN
#include "imgui_impl_vulkan.h"
#endif
#include "input/InputManager.h"
#include "config/ActiveSettings.h"
#include "Common/FileStream.h"

#include <set>

// <imgui_internal.h>
template<typename T>
static T ImMin(T lhs, T rhs)
{
	return lhs < rhs ? lhs : rhs;
}
template<typename T>
static T ImMax(T lhs, T rhs)
{
	return lhs >= rhs ? lhs : rhs;
}
static ImVec2 ImRotate(const ImVec2& v, float cos_a, float sin_a)
{
	return ImVec2(v.x * cos_a - v.y * sin_a, v.x * sin_a + v.y * cos_a);
}

int rotation_start_index;
void ImRotateStart()
{
	rotation_start_index = ImGui::GetWindowDrawList()->VtxBuffer.Size;
}

ImVec2 ImRotationCenter()
{
	ImVec2 l(FLT_MAX, FLT_MAX), u(-FLT_MAX, -FLT_MAX); // bounds

	const auto& buf = ImGui::GetWindowDrawList()->VtxBuffer;
	for (int i = rotation_start_index; i < buf.Size; i++)
		l = ImMin(l, buf[i].pos), u = ImMax(u, buf[i].pos);

	return ImVec2((l.x + u.x) / 2, (l.y + u.y) / 2); // or use _ClipRectStack?
}

void ImRotateEnd(float rad, ImVec2 center)
{
	const float s = sin(rad);
	const float c = cos(rad);
	center = ImRotate(center, s, c) - center;

	auto& buf = ImGui::GetWindowDrawList()->VtxBuffer;
	for (int i = rotation_start_index; i < buf.Size; i++)
		buf[i].pos = ImRotate(buf[i].pos, s, c) - center;
}

uint8* extractCafeDefaultFont(sint32* size);
sint32 g_font_size = 0;
uint8* g_font_data = nullptr;
#if !BOOST_OS_WINDOWS
extern int const g_fontawesome_size;
extern char const g_fontawesome_data[];
#endif
std::unordered_map<int, ImFont*> g_imgui_fonts;
std::stack<int> g_font_requests;

// Characters outside the placeholder font (e.g. Japanese typed into the software keyboard) are baked on demand from
// the Wii U system font, so the atlas only grows by the glyphs actually shown
std::set<ImWchar> g_extra_glyphs;
ImVector<ImWchar> g_extra_glyph_ranges;
bool g_extra_glyphs_dirty = false;
std::vector<uint8> g_system_font_data;
bool g_system_font_load_attempted = false;

static bool LoadSystemFontData()
{
	if (g_system_font_load_attempted)
		return !g_system_font_data.empty();
	g_system_font_load_attempted = true;
	const fs::path candidates[] = {
		ActiveSettings::GetMlcPath("sys/title/0005001b/10042400/content/CafeStd.ttf"),
		ActiveSettings::GetDataPath("resources/sharedFonts/CafeStd.ttf")};
	for (const auto& path : candidates)
	{
		std::unique_ptr<FileStream> file(FileStream::openFile2(path));
		if (!file)
			continue;
		g_system_font_data.resize(file->GetSize());
		if (file->readData(g_system_font_data.data(), (uint32)g_system_font_data.size()) == g_system_font_data.size())
			return true;
		g_system_font_data.clear();
	}
	cemuLog_log(LogType::Force, "ImGui: CafeStd.ttf not found, non-Latin characters will not be displayed");
	return false;
}

void ImGui_RequestGlyphs(const wchar_t* text)
{
	for (; text && *text; ++text)
	{
		const wchar_t c = *text;
		if (c < 0x80 || (c >= 0xD800 && c <= 0xDFFF))
			continue;
		if (g_extra_glyphs.insert((ImWchar)c).second)
			g_extra_glyphs_dirty = true;
	}
}

// re-adds every font size already in use, now with the extra glyphs merged in
static void RebuildFontsWithExtraGlyphs()
{
	g_extra_glyphs_dirty = false;
	if (!LoadSystemFontData())
		return;
	ImFontGlyphRangesBuilder builder;
	for (const ImWchar c : g_extra_glyphs)
		builder.AddChar(c);
	g_extra_glyph_ranges.clear();
	builder.BuildRanges(&g_extra_glyph_ranges);

	auto& io = ImGui::GetIO();
	cemu_assert(io.Fonts->Locked == false);
	for (const auto& [size, font] : g_imgui_fonts)
		g_font_requests.emplace(size);
	g_imgui_fonts.clear();
	g_renderer->Flush(true);
	g_renderer->DeleteFontTextures();
	io.Fonts->Clear();
	io.Fonts->AddFontDefault(); // Renderer::Initialize() starts the atlas with it, keep it as the default font
}

void ImGui_PrecacheFonts()
{
	if (g_extra_glyphs_dirty)
		RebuildFontsWithExtraGlyphs();
	while (!g_font_requests.empty())
	{
		const int size = g_font_requests.top();
		g_font_requests.pop();

		auto& io = ImGui::GetIO();
		cemu_assert(io.Fonts->Locked == false);

		if (g_font_size == 0)
			g_font_data = extractCafeDefaultFont(&g_font_size);

		ImFontConfig cfg{};
		cfg.FontDataOwnedByAtlas = false;
		// cfg.FontData = g_font_data;
		// cfg.FontDataSize = g_font_size;
		// cfg.SizePixels = size;
		ImFont* font = io.Fonts->AddFontFromMemoryTTF(g_font_data, g_font_size, (float)size, &cfg);

		ImFontConfig cfgmerge{};
		cfgmerge.FontDataOwnedByAtlas = false;
		cfgmerge.MergeMode = true;
		cfgmerge.GlyphMinAdvanceX = 20.0f;
		// cfgmerge.GlyphOffset = { 2,2 };

		static const ImWchar icon_ranges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};

#if BOOST_OS_WINDOWS
		const auto hinstance = GetModuleHandle(nullptr);
		const HRSRC res = FindResource(hinstance, MAKEINTRESOURCE(IDR_FONTAWESOME), RT_RCDATA);
		if (res)
		{
			const HGLOBAL mem = ::LoadResource(hinstance, res);
			if (mem)
			{
				void* data = LockResource(mem);
				const size_t len = SizeofResource(hinstance, res);

				io.Fonts->AddFontFromMemoryTTF(data, (int)len, (float)size, &cfgmerge, icon_ranges);
			}
		}
#else
		io.Fonts->AddFontFromMemoryTTF((void*)g_fontawesome_data, (int)g_fontawesome_size, (float)size, &cfgmerge, icon_ranges);
#endif

		if (!g_extra_glyph_ranges.empty() && !g_system_font_data.empty())
		{
			ImFontConfig cfgextra{};
			cfgextra.FontDataOwnedByAtlas = false;
			cfgextra.MergeMode = true;
			io.Fonts->AddFontFromMemoryTTF(g_system_font_data.data(), (int)g_system_font_data.size(), (float)size, &cfgextra, g_extra_glyph_ranges.Data);
		}

		g_imgui_fonts[(int)size] = font;

		// Vulkan doesn't let us destroy resources that are still being used, so we flush here
		g_renderer->Flush(true);
		g_renderer->DeleteFontTextures();
	}
}

void ImGui_ClearFonts()
{
	g_imgui_fonts.clear();
}

ImFont* ImGui_GetFont(float size)
{
	const auto it = g_imgui_fonts.find((int)size);
	if (it != g_imgui_fonts.cend())
		return it->second;

	g_font_requests.emplace((int)size);
	return nullptr; // will create the font in next precache call
}

void ImGui_UpdateWindowInformation(bool mainWindow)
{
	static std::map<uint32, ImGuiKey> keyboard_mapping;
	auto& inputManager = InputManager::instance();
	const auto hostSnapshot = inputManager.GetHostNativeSurfaces();
	static uint32 current_key = 0;
	ImGuiIO& io = ImGui::GetIO();
	io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
#if BOOST_OS_WINDOWS
	io.ImeWindowHandle = mainWindow ? hostSnapshot.mainWindow.surface : hostSnapshot.padWindow.surface;
#endif

	io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);

	auto& instance = inputManager;

	const auto mousePos = instance.get_mouse_position(!mainWindow);
	io.MousePos = {(float)mousePos.x, (float)mousePos.y};

	bool padDown;
	const auto pos = instance.get_left_down_mouse_info(&padDown);
	io.MouseDown[0] = padDown != mainWindow && pos.has_value();
	auto get_mapping = [&](uint32 key_code) {
		auto key = keyboard_mapping.find(key_code);
		if (key != keyboard_mapping.end())
			return key->second;
		ImGuiKey mapped_key = (ImGuiKey)((uint32)current_key + ImGuiKey_NamedKey_BEGIN);
		current_key = (current_key + 1) % (uint32)ImGuiKey_NamedKey_COUNT;
		keyboard_mapping[key_code] = mapped_key;
		return mapped_key;
	};
	for (const auto& keyState : inputManager.GetHostKeyStates())
		io.AddKeyEvent(get_mapping(keyState.key), keyState.pressed);

	// printf("%f %f %d\n", io.MousePos.x, io.MousePos.y, io.MouseDown[0]);

	for (auto i = 0; i < InputManager::kMaxController; ++i)
	{
		const auto controller = instance.get_controller(i);
		if (!controller)
			continue;

		if (controller->is_start_down())
			io.NavInputs[ImGuiNavInput_Input] = 1.0f;

		if (controller->is_a_down())
			io.NavInputs[ImGuiNavInput_Activate] = 1.0f;

		if (controller->is_b_down())
			io.NavInputs[ImGuiNavInput_Cancel] = 1.0f;

		if (controller->is_left_down())
			io.NavInputs[ImGuiNavInput_DpadLeft] = 1.0f;

		if (controller->is_right_down())
			io.NavInputs[ImGuiNavInput_DpadRight] = 1.0f;

		if (controller->is_up_down())
			io.NavInputs[ImGuiNavInput_DpadUp] = 1.0f;

		if (controller->is_down_down())
			io.NavInputs[ImGuiNavInput_DpadDown] = 1.0f;
	}
}
