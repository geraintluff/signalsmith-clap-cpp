#ifndef LOG_EXPR
#	include <iostream>
#	define LOG_EXPR(expr) std::cout << #expr " = " << (expr) << std::endl;
#endif

#include "clap/clap.h"

#include "signalsmith-clap/cpp.h"

#include "signalsmith-basics/chorus.h"
#include "cbor-walker/cbor-walker.h"
#include "webview-gui/webview-gui.h"

#include "../plugins.h"

#include <atomic>

struct ExampleAudioPlugin {
	using Plugin = ExampleAudioPlugin;

	static const clap_plugin_descriptor * getPluginDescriptor() {
		static const char * features[] = {
			CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
			CLAP_PLUGIN_FEATURE_STEREO,
			nullptr
		};
		static clap_plugin_descriptor descriptor{
			.clap_version=CLAP_VERSION_INIT,
			.id="uk.co.signalsmith-audio.plugins.example-audio-plugin",
			.name="C++ Example Audio Plugin (Chorus)",
			.vendor="Signalsmith Audio",
			.url=nullptr,
			.manual_url=nullptr,
			.support_url=nullptr,
			.version="1.0.0",
			.description="Audio plugin from a starter CLAP project",
			.features=features
		};
		return &descriptor;
	};

	static const clap_plugin * create(const clap_host *host) {
		return &(new ExampleAudioPlugin(host))->clapPlugin;
	}
	
	const clap_host *host;
	// Extensions aren't filled out until `.pluginInit()`
	const clap_host_state *hostState = nullptr;
	const clap_host_audio_ports *hostAudioPorts = nullptr;
	const clap_host_params *hostParams = nullptr;
	const clap_host_gui *hostGui = nullptr;

	signalsmith::basics::ChorusFloat chorus;

	struct Param {
		double value = 0;
		clap_param_info info;
		const char *formatString = "%.2f";
		
		// User interactions which we need to send as events to the host
		std::atomic_flag sentValue = ATOMIC_FLAG_INIT;
		std::atomic_flag sentGestureStart = ATOMIC_FLAG_INIT;
		std::atomic_flag sentGestureEnd =ATOMIC_FLAG_INIT;

		std::atomic_flag sentUiState = ATOMIC_FLAG_INIT;

		Param(const char *name, clap_id paramId, double min, double initial, double max) : value(initial) {
			info = {
				.id=paramId,
				.flags=CLAP_PARAM_IS_AUTOMATABLE,
				.cookie=this,
				.name={}, // assigned below
				.module={},
				.min_value=min,
				.max_value=max,
				.default_value=initial
			};
			std::strncpy(info.name, name, CLAP_NAME_SIZE);
			
			sentValue.test_and_set();
			sentGestureStart.test_and_set();
			sentGestureEnd.test_and_set();
		}

		void sendEvents(const clap_output_events *outEvents) {
			if (!sentGestureStart.test_and_set()) {
				clap_event_param_gesture event{
					.header={
						.size=sizeof(clap_event_param_gesture),
						.time=0,
						.space_id=CLAP_CORE_EVENT_SPACE_ID,
						.type=CLAP_EVENT_PARAM_GESTURE_BEGIN,
						.flags=CLAP_EVENT_IS_LIVE
					},
					.param_id=info.id
				};
				outEvents->try_push(outEvents, &event.header);
			}
			if (!sentValue.test_and_set()) {
				clap_event_param_value event{
					.header={
						.size=sizeof(clap_event_param_value),
						.time=0,
						.space_id=CLAP_CORE_EVENT_SPACE_ID,
						.type=CLAP_EVENT_PARAM_VALUE,
						.flags=CLAP_EVENT_IS_LIVE
					},
					.param_id=info.id,
					.cookie=this,
					.note_id=-1,
					.port_index=-1,
					.channel=-1,
					.key=-1,
					.value=value
				};
				outEvents->try_push(outEvents, &event.header);
			}
			if (!sentGestureEnd.test_and_set()) {
				clap_event_param_gesture event{
					.header={
						.size=sizeof(clap_event_param_gesture),
						.time=0,
						.space_id=CLAP_CORE_EVENT_SPACE_ID,
						.type=CLAP_EVENT_PARAM_GESTURE_END,
						.flags=CLAP_EVENT_IS_LIVE
					},
					.param_id=info.id
				};
				outEvents->try_push(outEvents, &event.header);
			}
		}
	};
	Param mix{"mix", 0xCA5CADE5, 0, 0.6, 1};
	Param depthMs{"depth", 0xBA55FEED, 2, 15, 50};
	Param detune{"detune", 0xCA55E77E, 1, 6, 30};
	Param stereo{"stereo", 0x0FF51DE5, 0, 1, 2};
	std::array<Param *, 4> params = {&mix, &depthMs, &detune, &stereo};
	
	ExampleAudioPlugin(const clap_host *host) : host(host) {
		depthMs.formatString = "%.1f ms";
		detune.formatString = "%.0f cents";
	}

	// Makes a C function pointer to a C++ method
	template<auto methodPtr>
	auto clapPluginMethod() -> decltype(signalsmith::clap::pluginMethod<methodPtr>()) {
		return signalsmith::clap::pluginMethod<methodPtr>();
	}

	const clap_plugin clapPlugin{
		.desc=getPluginDescriptor(),
		.plugin_data=this,
		.init=clapPluginMethod<&Plugin::pluginInit>(),
		.destroy=clapPluginMethod<&Plugin::pluginDestroy>(),
		.activate=clapPluginMethod<&Plugin::pluginActivate>(),
		.deactivate=clapPluginMethod<&Plugin::pluginDeactivate>(),
		.start_processing=clapPluginMethod<&Plugin::pluginStartProcessing>(),
		.stop_processing=clapPluginMethod<&Plugin::pluginStopProcessing>(),
		.reset=clapPluginMethod<&Plugin::pluginReset>(),
		.process=clapPluginMethod<&Plugin::pluginProcess>(),
		.get_extension=clapPluginMethod<&Plugin::pluginGetExtension>(),
		.on_main_thread=clapPluginMethod<&Plugin::pluginOnMainThread>()
	};

	bool pluginInit() {
		using namespace signalsmith::clap;
		getHostExtension(host, CLAP_EXT_STATE, hostState);
		getHostExtension(host, CLAP_EXT_AUDIO_PORTS, hostAudioPorts);
		getHostExtension(host, CLAP_EXT_PARAMS, hostParams);
		getHostExtension(host, CLAP_EXT_GUI, hostGui);
		return true;
	}
	void pluginDestroy() {
		delete this;
	}
	bool pluginActivate(double sRate, uint32_t minFrames, uint32_t maxFrames) {
		chorus.configure(sRate, maxFrames, 2);
		return true;
	}
	void pluginDeactivate() {
	}
	bool pluginStartProcessing() {
		return true;
	}
	void pluginStopProcessing() {
	}
	void pluginReset() {
		chorus.reset();
	}
	void processEvent(const clap_event_header *event) {
		if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
		if (event->type == CLAP_EVENT_PARAM_VALUE) {
			auto &eventParam = *(const clap_event_param_value *)event;
			if (eventParam.cookie) {
				// if provided, it's the parameter
				auto &param = *(Param *)eventParam.cookie;
				param.value = eventParam.value;
				param.sentUiState.clear();
			} else {
				// Otherwise, match the ID
				for (auto *param : params) {
					if (eventParam.param_id == param->info.id) {
						param->value = eventParam.value;
						param->sentUiState.clear();
						break;
					}
				}
			}

			// Request a callback so we can tell the host our state is dirty
			stateDirty = true;
			// Tell the UI as well
			sentWebviewState.clear();
			host->request_callback(host);
		}
	}
	clap_process_status pluginProcess(const clap_process *process) {
		auto &audioInput = process->audio_inputs[0];
		auto &audioOutput = process->audio_outputs[0];

		auto *eventsIn = process->in_events;
		auto *eventsOut = process->out_events;
		uint32_t eventCount = eventsIn->size(eventsIn);
		// We could (should?) split the processing up and apply these events partway through the block
		// but for simplicity here we don't support sample-accurate automation
		for (uint32_t i = 0; i < eventCount; ++i) {
			auto *event = eventsIn->get(eventsIn, i);
			processEvent(event);
			eventsOut->try_push(eventsOut, event);
		}
		
		chorus.mix = mix.value;
		chorus.depthMs = depthMs.value;
		chorus.detune = detune.value;
		chorus.stereo = stereo.value;
		chorus.process(audioInput.data32, audioOutput.data32, process->frames_count);

		for (auto *param : params) {
			param->sendEvents(eventsOut);
		}

		return CLAP_PROCESS_CONTINUE;
	}

	bool stateDirty = false;
	void pluginOnMainThread() {
		if (stateDirty && hostState) {
			hostState->mark_dirty(host);
			stateDirty = false;
		}
		webviewSendIfNeeded();
	}

	const void * pluginGetExtension(const char *extId) {
		if (!std::strcmp(extId, CLAP_EXT_STATE)) {
			static const clap_plugin_state ext{
				.save=clapPluginMethod<&Plugin::stateSave>(),
				.load=clapPluginMethod<&Plugin::stateLoad>(),
			};
			return &ext;
		} else if (!std::strcmp(extId, CLAP_EXT_AUDIO_PORTS)) {
			static const clap_plugin_audio_ports ext{
				.count=clapPluginMethod<&Plugin::audioPortsCount>(),
				.get=clapPluginMethod<&Plugin::audioPortsGet>(),
			};
			return &ext;
		} else if (!std::strcmp(extId, CLAP_EXT_PARAMS)) {
			static const clap_plugin_params ext{
				.count=clapPluginMethod<&Plugin::paramsCount>(),
				.get_info=clapPluginMethod<&Plugin::paramsGetInfo>(),
				.get_value=clapPluginMethod<&Plugin::paramsGetValue>(),
				.value_to_text=clapPluginMethod<&Plugin::paramsValueToText>(),
				.text_to_value=clapPluginMethod<&Plugin::paramsTextToValue>(),
				.flush=clapPluginMethod<&Plugin::paramsFlush>(),
			};
			return &ext;
		} else if (!std::strcmp(extId, CLAP_EXT_GUI)) {
			static const clap_plugin_gui ext{
				.is_api_supported=clapPluginMethod<&Plugin::guiIsApiSupported>(),
				.get_preferred_api=clapPluginMethod<&Plugin::guiGetPreferredApi>(),
				.create=clapPluginMethod<&Plugin::guiCreate>(),
				.destroy=clapPluginMethod<&Plugin::guiDestroy>(),
				.set_scale=clapPluginMethod<&Plugin::guiSetScale>(),
				.get_size=clapPluginMethod<&Plugin::guiGetSize>(),
				.can_resize=clapPluginMethod<&Plugin::guiCanResize>(),
				.get_resize_hints=clapPluginMethod<&Plugin::guiGetResizeHints>(),
				.adjust_size=clapPluginMethod<&Plugin::guiAdjustSize>(),
				.set_size=clapPluginMethod<&Plugin::guiSetSize>(),
				.set_parent=clapPluginMethod<&Plugin::guiSetParent>(),
				.set_transient=clapPluginMethod<&Plugin::guiSetTransient>(),
				.suggest_title=clapPluginMethod<&Plugin::guiSuggestTitle>(),
				.show=clapPluginMethod<&Plugin::guiShow>(),
				.hide=clapPluginMethod<&Plugin::guiHide>(),
			};
			return &ext;
		}
		return nullptr;
	}
	
	// ---- state save/load ----
	
	bool stateSave(const clap_ostream_t *stream) {
		std::vector<unsigned char> bytes;
		signalsmith::cbor::CborWriter cbor{bytes};
		cbor.openMap(4);
		for (auto *param : params) {
			cbor.addInt(param->info.id); // CBOR keys can be any type
			cbor.addFloat(param->value);
		}
		return signalsmith::clap::writeAllToStream(bytes, stream);
	}
	bool stateLoad(const clap_istream_t *stream) {
		std::vector<unsigned char> bytes;
		if (!signalsmith::clap::readAllFromStream(bytes, stream) || bytes.empty()) return false;

		using Cbor = signalsmith::cbor::CborWalker;
		Cbor cbor{bytes};
		if (!cbor.isMap()) return false;
		cbor.forEachPair([&](Cbor key, Cbor value){
			for (auto *param : params) {
				if (uint32_t(key) == param->info.id) {
					param->value = double(value);
				}
			}
		});
		return true;
	}

	// ---- audio ports ----

	uint32_t audioPortsCount(bool isInput) {
		return 1;
	}
	bool audioPortsGet(uint32_t index, bool isInput, clap_audio_port_info *info) {
		if (index > audioPortsCount(isInput)) return false;
		*info = {
			.id=0xF0CACC1A,
			.name={'m', 'a', 'i', 'n'},
			.flags=CLAP_AUDIO_PORT_IS_MAIN + CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE,
			.channel_count=2,
			.port_type=CLAP_PORT_STEREO,
			.in_place_pair=CLAP_INVALID_ID
		};
		return true;
	}

	// ---- parameters ----
	
	uint32_t paramsCount() {
		return uint32_t(params.size());
	}
	
	bool paramsGetInfo(uint32_t index, clap_param_info *info) {
		if (index >= params.size()) return false;
		*info = params[index]->info;
		return true;
	}
	
	bool paramsGetValue(clap_id paramId, double *value) {
		for (auto *param : params) {
			if (param->info.id == paramId) {
				*value = param->value;
				return true;
			}
		}
		return false;
	}
	
	bool paramsValueToText(clap_id paramId, double value, char *text, uint32_t textCapacity) {
		for (auto *param : params) {
			if (param->info.id == paramId) {
				std::snprintf(text, textCapacity, param->formatString, value);
				return true;
			}
		}
		return false;
	}

	bool paramsTextToValue(clap_id paramId, const char *text, double *value) {
		return false; // not supported
	}
	
	void paramsFlush(const clap_input_events *eventsIn, const clap_output_events *eventsOut) {
		uint32_t eventCount = eventsIn->size(eventsIn);
		for (uint32_t i = 0; i < eventCount; ++i) {
			auto *event = eventsIn->get(eventsIn, i);
			processEvent(event);
			eventsOut->try_push(eventsOut, event);
		}
		for (auto *param : params) {
			param->sendEvents(eventsOut);
		}
	}

	// ---- GUI ----
	
	using WebviewGui = webview_gui::WebviewGui;
	std::unique_ptr<WebviewGui> webview;
	std::atomic_flag sentWebviewState = ATOMIC_FLAG_INIT;

	static WebviewGui::Platform clapApiToPlatform(const char *api) {
		auto platform = WebviewGui::NONE;
		if (!std::strcmp(api, CLAP_WINDOW_API_WIN32)) platform = WebviewGui::HWND;
		if (!std::strcmp(api, CLAP_WINDOW_API_COCOA)) platform = WebviewGui::COCOA;
		if (!std::strcmp(api, CLAP_WINDOW_API_X11)) platform = WebviewGui::X11EMBED;
		return platform;
	}

	bool guiIsApiSupported(const char *api, bool isFloating) {
		if (isFloating) return false;
		return WebviewGui::supports(clapApiToPlatform(api));
	}
	bool guiGetPreferredApi(const char **api, bool *isFloating) {
		*isFloating = false;
		*api = nullptr;
		if (WebviewGui::supports(WebviewGui::HWND)) *api = CLAP_WINDOW_API_WIN32;
		if (WebviewGui::supports(WebviewGui::COCOA)) *api = CLAP_WINDOW_API_COCOA;
		if (WebviewGui::supports(WebviewGui::X11EMBED)) *api = CLAP_WINDOW_API_X11;
		return *api != nullptr;
	}
	bool guiCreate(const char *api, bool isFloating) {
		if (isFloating) return false;
		if (webview) return true; // already created before
		webview = WebviewGui::createUnique(clapApiToPlatform(api), "/", [this](const char *path, WebviewGui::Resource &resource){
			return webviewGetResource(path, resource);
		});
		if (webview) {
			uint32_t w, h;
			guiGetSize(&w, &h);
			webview->setSize(w, h);
			webview->receive = [&](const unsigned char *bytes, size_t length){
				webviewReceive(bytes, length);
			};
		}
		return bool(webview);
	}
	void guiDestroy() {
		// We *could* skip this, and retain the webview indefinitely
		// but this is more polite since it releases memory
		webview = nullptr;
	}
	bool guiSetScale(double scale) {
		return true;
	}
	bool guiGetSize(uint32_t *width, uint32_t *height) {
		*width = 300;
		*height = 200;
		return true;
	}
	bool guiCanResize() {
		return false;
	}
	bool guiGetResizeHints(clap_gui_resize_hints *hints) {
		return false;
	}
	bool guiAdjustSize(uint32_t *width, uint32_t *height) {
		return guiGetSize(width, height);
	}
	bool guiSetSize(uint32_t w, uint32_t h) {
		return false;
	}
	bool guiSetParent(const clap_window *window) {
		if (webview) {
			webview->attach(window->ptr);
			return true;
		}
		return false;
	}
	
	bool guiSetTransient(const clap_window *window) {
		return false;
	}
	
	void guiSuggestTitle(const char *title) {}
	
	bool guiShow() {
		return true;
	}
	bool guiHide() {
		return true;
	}
	
	bool webviewGetResource(const char *path, WebviewGui::Resource &resource);
	bool webviewReceive(const unsigned char *bytes, size_t length) {
		using Cbor = signalsmith::cbor::CborWalker;
		
		auto updateParam = [&](Param &param, Cbor cbor){
			cbor.forEachPair([&](Cbor key, Cbor value){
				auto keyString = key.utf8View();
				if (keyString == "value" && value.isNumber()) {
					param.value = value;
					param.sentValue.clear();
				} else if (keyString == "gesture") {
					if (bool(value)) {
						param.sentGestureStart.clear();
					} else {
						param.sentGestureEnd.clear();
					}
				}
			});
		};
		
		Cbor cbor{bytes, length};
		if (cbor.utf8View() == "ready") {
			for (auto *param : params) {
				// Resend everything
				param->sentUiState.clear();
			}
			sentWebviewState.clear();
			webviewSendIfNeeded();
			return true;
		}
		
		cbor.forEachPair([&](Cbor key, Cbor value){
			auto keyString = key.utf8View();
			if (keyString == "mix") {
				updateParam(mix, value);
			} else if (keyString == "depth") {
				updateParam(depthMs, value);
			} else if (keyString == "detune") {
				updateParam(detune, value);
			} else if (keyString == "stereo") {
				updateParam(stereo, value);
			}
		});

		if (hostParams) hostParams->request_flush(host);

		return !cbor.error();
	}
	void webviewSendIfNeeded() {
		if (!webview) return;
		if (sentWebviewState.test_and_set()) return;

		std::vector<unsigned char> bytes;
		signalsmith::cbor::CborWriter cbor{bytes};
		cbor.openMap();
		
		auto updateParam = [&](const char *key, Param &param){
			if (param.sentUiState.test_and_set()) return;
			cbor.addUtf8(key);
			cbor.openMap(1);
			cbor.addUtf8("value");
			cbor.addFloat(param.value);
		};
		updateParam("mix", mix);
		updateParam("depth", depthMs);
		updateParam("detune", detune);
		updateParam("stereo", stereo);
		cbor.close();
		
		webview->send(bytes.data(), bytes.size());
	}
};
