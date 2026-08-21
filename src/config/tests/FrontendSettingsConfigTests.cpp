#include "Common/precompiled.h"

#include "config/CemuConfig.h"

int main()
{
	{
		tinyxml2::XMLDocument document;
		assert(document.Parse(R"(<content>
			<fullscreen>true</fullscreen><open_pad>true</open_pad><check_update>false</check_update>
		</content>)") == tinyxml2::XML_SUCCESS);
		XMLConfigParser parser(&document);
		CemuConfig config;
		config.Load(parser);
		assert(config.frontend.start_fullscreen.GetValue());
		assert(config.frontend.open_pad.GetValue());
		assert(!config.frontend.check_updates.GetValue());
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
		config.frontend.setup_completed = true;
		config.Save(parser);
		const auto* content = document.FirstChildElement("content");
		assert(content);
		const auto* frontend = content->FirstChildElement("Frontend");
		assert(frontend && !frontend->NextSiblingElement("Frontend"));
		assert(frontend->FirstChildElement("StartFullscreen")->BoolText(false));
		assert(frontend->FirstChildElement("OpenPad")->BoolText(false));
		assert(!frontend->FirstChildElement("CheckUpdates")->BoolText(true));
		assert(frontend->FirstChildElement("SetupCompleted")->BoolText(false));
	}

	return 0;
}
