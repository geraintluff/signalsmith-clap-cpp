#ifndef LOG_EXPR
#	include <iostream>
#	define LOG_EXPR(expr) std::cout << #expr " = " << (expr) << std::endl;
#endif

#include "plugins.h"

#include "clap/clap.h"

#include "./example-audio-plugin/example-audio-plugin.h"
#include "./example-note-plugin/example-note-plugin.h"
#include "./example-keyboard/example-keyboard.h"
#include "./example-synth/example-synth.h"

#include <cstring>
#include <iostream>

std::string clapBundleResourceDir;

// ---- Plugin factory ----

static uint32_t pluginFactoryGetPluginCount(const struct clap_plugin_factory *) {
	return 4;
}
static const clap_plugin_descriptor_t * pluginFactoryGetPluginDescriptor(const struct clap_plugin_factory *factory, uint32_t index) {
	if (index == 0) return ExampleAudioPlugin::getPluginDescriptor();
	if (index == 1) return ExampleNotePlugin::getPluginDescriptor();
	if (index == 2) return ExampleKeyboard::getPluginDescriptor();
	if (index == 3) return ExampleSynth::getPluginDescriptor();
	return nullptr;
}

static const clap_plugin_t * pluginFactoryCreatePlugin(const struct clap_plugin_factory *, const clap_host_t *host, const char *pluginId) {
	if (!std::strcmp(pluginId, ExampleAudioPlugin::getPluginDescriptor()->id)) {
		return ExampleAudioPlugin::create(host);
	} else if (!std::strcmp(pluginId, ExampleNotePlugin::getPluginDescriptor()->id)) {
		return ExampleNotePlugin::create(host);
	} else if (!std::strcmp(pluginId, ExampleKeyboard::getPluginDescriptor()->id)) {
		return ExampleKeyboard::create(host);
	} else if (!std::strcmp(pluginId, ExampleSynth::getPluginDescriptor()->id)) {
		return ExampleSynth::create(host);
	}
	return nullptr;
}

// ---- Main bundle methods ----

bool clapEntryInit(const char *path) {
	std::cout << "Initialised CLAP module at: " << path << std::endl;
	clapBundleResourceDir = path;
#if defined(__APPLE__) && (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
	clapBundleResourceDir += "/Contents/Resources";
#endif
	return true;
}
void clapEntryDeinit() {
	clapBundleResourceDir = "";
}

const void * clapEntryGetFactory(const char *factoryId) {
	if (!std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID)) {
		static const clap_plugin_factory clapPluginFactory{
			.get_plugin_count=pluginFactoryGetPluginCount,
			.get_plugin_descriptor=pluginFactoryGetPluginDescriptor,
			.create_plugin=pluginFactoryCreatePlugin
		};
		return &clapPluginFactory;
	}
	return nullptr;
}
