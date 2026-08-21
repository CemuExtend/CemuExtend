#include "Common/precompiled.h"

#include "config/CemuConfig.h"

int main()
{
	{
		tinyxml2::XMLDocument document;
		assert(document.Parse(R"(<content>
			<fullscreen>true</fullscreen><open_pad>true</open_pad><check_update>false</check_update><save_screenshot>false</save_screenshot>
		</content>)") == tinyxml2::XML_SUCCESS);
		XMLConfigParser parser(&document);
		CemuConfig config;
		config.Load(parser);
		assert(config.frontend.start_fullscreen.GetValue());
		assert(config.frontend.open_pad.GetValue());
		assert(!config.frontend.check_updates.GetValue());
		assert(!config.frontend.save_screenshots.GetValue());
		assert(config.frontend.setup_completed.GetValue());
	}

	{
		tinyxml2::XMLDocument document;
		assert(document.Parse(R"(<content>
			<fullscreen>true</fullscreen>
			<Frontend><StartFullscreen>false</StartFullscreen><SetupCompleted>false</SetupCompleted></Frontend>
		</content>)") == tinyxml2::XML_SUCCESS);
		XMLConfigParser parser(&document);
		CemuConfig config;
		config.Load(parser);
		// A legacy key is emitted only by the wx compatibility frontend and must
		// win while that optional selector remains available.
		assert(config.frontend.start_fullscreen.GetValue());
		assert(!config.frontend.setup_completed.GetValue());
	}

	{
		tinyxml2::XMLDocument document;
		XMLConfigParser parser(&document);
		CemuConfig config;
		config.frontend.start_fullscreen = true;
		config.frontend.open_pad = true;
		config.frontend.check_updates = false;
		config.frontend.save_screenshots = false;
		config.frontend.setup_completed = true;
		config.Save(parser);
		const auto* content = document.FirstChildElement("content");
		assert(content);
		const auto* frontend = content->FirstChildElement("Frontend");
		assert(frontend && !frontend->NextSiblingElement("Frontend"));
		assert(frontend->FirstChildElement("StartFullscreen")->BoolText(false));
		assert(frontend->FirstChildElement("OpenPad")->BoolText(false));
		assert(!frontend->FirstChildElement("CheckUpdates")->BoolText(true));
		assert(!frontend->FirstChildElement("SaveScreenshots")->BoolText(true));
		assert(frontend->FirstChildElement("SetupCompleted")->BoolText(false));
		const auto* hotkeys = frontend->FirstChildElement("Hotkeys");
		assert(hotkeys && !hotkeys->NextSiblingElement("Hotkeys"));
	}

	{
		tinyxml2::XMLDocument document;
		assert(document.Parse(R"(<content><Frontend><Hotkeys>
			<ControllerModifier>4</ControllerModifier>
			<ToggleFullscreen>68 1 9</ToggleFullscreen>
			<TakeScreenshot>69 0 -1</TakeScreenshot>
		</Hotkeys></Frontend></content>)") == tinyxml2::XML_SUCCESS);
		XMLConfigParser parser(&document);
		CemuConfig config;
		config.Load(parser);
		assert(config.frontend.hotkeys.controller_modifier == 4);
		assert(config.frontend.hotkeys.toggle_fullscreen.keyboard_usage == 0x44);
		assert(config.frontend.hotkeys.toggle_fullscreen.keyboard_modifiers == 1);
		assert(config.frontend.hotkeys.toggle_fullscreen.controller_button == 9);
		assert(config.frontend.hotkeys.take_screenshot.keyboard_usage == 0x45);
	}

	{
		tinyxml2::XMLDocument document;
		assert(document.Parse(R"(<content></content><Hotkeys>
			<modifiers>0 6</modifiers>
			<ToggleFullscreen>350 7</ToggleFullscreen>
			<ToggleFullscreenAlt>8205 -1</ToggleFullscreenAlt>
			<ExitFullscreen>27 -1</ExitFullscreen>
			<TakeScreenshot>351 -1</TakeScreenshot>
		</Hotkeys>)") == tinyxml2::XML_SUCCESS);
		XMLConfigParser parser(&document);
		CemuConfig config;
		config.Load(parser);
		assert(config.frontend.hotkeys.controller_modifier == 6);
		assert(config.frontend.hotkeys.toggle_fullscreen.keyboard_usage == 0x44);
		assert(config.frontend.hotkeys.toggle_fullscreen.controller_button == 7);
		assert(config.frontend.hotkeys.toggle_fullscreen_alternative.keyboard_usage == 0x28);
		assert(config.frontend.hotkeys.toggle_fullscreen_alternative.keyboard_modifiers == 4);
		assert(config.frontend.hotkeys.exit_fullscreen.keyboard_usage == 0x29);
		assert(config.frontend.hotkeys.take_screenshot.keyboard_usage == 0x45);
	}

	return 0;
}
