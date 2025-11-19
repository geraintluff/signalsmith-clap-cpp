#include "clap/clap.h"

#include "signalsmith-clap/cpp.h"
#include "signalsmith-clap/note-manager.h"
#include "signalsmith-clap/params.h"

#include "cbor-walker/cbor-walker.h"
#include "webview-gui/clap-webview-gui.h"

#include "../plugins.h"

#include <atomic>
#include <mutex>
#include <random>

struct ExampleKeyboard {
	using Plugin = ExampleKeyboard;
	
	static const clap_plugin_descriptor * getPluginDescriptor() {
		static const char * features[] = {
			CLAP_PLUGIN_FEATURE_NOTE_EFFECT,
			nullptr
		};
		static clap_plugin_descriptor descriptor{
			.id="uk.co.signalsmith-audio.plugins.example-keyboard",
			.name="C++ Example Virtual Keyboard",
			.vendor="Signalsmith Audio",
			.url=nullptr,
			.manual_url=nullptr,
			.support_url=nullptr,
			.version="1.0.0",
			.description="Virtual keyboard from a starter CLAP project",
			.features=features
		};
		return &descriptor;
	};

	static const clap_plugin * create(const clap_host *host) {
		return &(new Plugin(host))->clapPlugin;
	}
	
	const clap_host *host;
	// Extensions aren't filled out until `.pluginInit()`
	const clap_host_state *hostState = nullptr;
	const clap_host_audio_ports *hostAudioPorts = nullptr;
	const clap_host_note_ports *hostNotePorts = nullptr;
	const clap_host_params *hostParams = nullptr;
	const webview_gui::clap_host_webview *hostWebview = nullptr;

	double sampleRate = 1;
	std::vector<bool> noteSentToMeters;
	using NoteManager = signalsmith::clap::NoteManager;
	NoteManager noteManager{1024};

	using Param = signalsmith::clap::Param;
	Param log2Rate{"log2Rate", "rate (log2)", 0x01234567, -2.0, 1.0, 4.0};
	Param regularity{"regularity", "regularity", 0x02468ACE, 0.0, 0.5, 1.0};
	Param velocityRand{"velocityRand", "velocity rand.", 0x12345678, 0.0, 0.5, 1.0};
	std::array<Param *, 3> params{&log2Rate, &regularity, &velocityRand};
	
	ExampleKeyboard(const clap_host *host) : host(host) {
		log2Rate.formatFn = [](double value){
			char text[16] = {};
			std::snprintf(text, 15, "%.2f Hz", std::exp2(value));
			return std::string(text);
		};
		
		noteSentToMeters.resize(noteManager.polyphony());
		metersNotes.reserve(noteManager.polyphony());
		sentMeters.test_and_set(); // nothing to send initially
		
		webview.setSize(860, 160); // default size
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
		getHostExtension(host, CLAP_EXT_NOTE_PORTS, hostNotePorts);
		getHostExtension(host, CLAP_EXT_PARAMS, hostParams);

		webview.init(&clapPlugin, host);
		hostWebview = webview.extHostWebview;
		return true;
	}
	void pluginDestroy() {
		delete this;
	}
	bool pluginActivate(double sRate, uint32_t minFrames, uint32_t maxFrames) {
		sampleRate = sRate;
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
		noteManager.reset();
		sampleCounter = 0;

		std::lock_guard<std::mutex> guard{outputEventMutex}; // Not ideal if it blocks, but it's not live processing
		outputEventQueue.clear(); // empty any old events
	}
	void processEvent(const clap_event_header *event) {
		if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
		if (event->type == CLAP_EVENT_PARAM_VALUE) {
			auto &eventParam = *(const clap_event_param_value *)event;
			if (eventParam.cookie) {
				// if provided, it's the parameter
				auto *param = (Param *)eventParam.cookie;
				param->setValueFromEvent(eventParam);
			} else {
				// Otherwise, match the ID
				for (auto *param : params) {
					if (eventParam.param_id == param->info.id) {
						param->setValueFromEvent(eventParam);
						break;
					}
				}
			}

			// Tell the host our state is dirty
			stateIsClean.clear();
			// Tell the UI as well
			sentWebviewState.clear();
			// Request a callback for both of the above
			host->request_callback(host);
		}
	}
	
	std::atomic_flag hasMeters = ATOMIC_FLAG_INIT;
	std::atomic_flag sentMeters = ATOMIC_FLAG_INIT;
	double meterInterval = 0, meterIntervalCounter = 0, meterStopCounter = 0;
	size_t sampleCounter = 0;
	double meterTime = 0;
	
	std::mutex outputEventMutex;
	std::vector<clap_event_note> outputEventQueue;
	
	clap_process_status pluginProcess(const clap_process *process) {
		noteManager.startBlock();
		auto *eventsIn = process->in_events;
		auto *eventsOut = process->out_events;
		
		bool hasOutputEvents = outputEventMutex.try_lock(); // OK if we fail, we'll try again very soon - almost certainly faster than the UI refresh rate
		size_t outputEventIndex = 0;
		auto sendOutputEvents = [&](size_t toTime){
			if (!hasOutputEvents) return;
			while (outputEventIndex < outputEventQueue.size() && outputEventQueue[outputEventIndex].header.time <= toTime) {
				eventsOut->try_push(eventsOut, &outputEventQueue[outputEventIndex].header);
				++outputEventIndex;
			}
		};

		uint32_t eventCount = eventsIn->size(eventsIn);
		for (uint32_t i = 0; i < eventCount; ++i) {
			auto *event = eventsIn->get(eventsIn, i);
			for (auto &noteTask : noteManager.processEvent(event, eventsOut)) {
				if (noteTask.state == noteManager.stateDown) {
					noteSentToMeters[noteTask.voiceIndex] = false;
				} else if (noteTask.released()) {
					noteManager.stop(noteTask, eventsOut);
				}
			}
			processEvent(event);
			eventsOut->try_push(eventsOut, event);
			sendOutputEvents(event->time);
		}
		sendOutputEvents(uint32_t(-1));
		if (hasOutputEvents) {
			outputEventQueue.clear();
			outputEventMutex.unlock();
		}

		for (auto &noteTask : noteManager.processTo(process->frames_count)) {
			if (noteTask.state == noteManager.stateDown) {
				noteSentToMeters[noteTask.voiceIndex] = false;
			} else if (noteTask.released()) {
				noteManager.stop(noteTask, eventsOut);
			}
		}
		
		sampleCounter += process->frames_count;

		meterIntervalCounter -= process->frames_count/sampleRate;
		meterStopCounter -= process->frames_count/sampleRate;
		if (meterStopCounter > 0 && meterIntervalCounter < 0 && !hasMeters.test_and_set()) {
			// Copy meters over
			metersNotes.resize(0);
			for (auto &note : noteManager) {
				auto ageSamples = note.ageAt(process->frames_count);
				float ageSeconds = ageSamples/sampleRate;
				metersNotes.push_back(MetersNote{
					.key=float(note.key),
					.hue=float(note.velocity),
					.brightness=float(note.velocity*(2 - note.velocity)),
					.width=0.2f + 0.8f/(ageSeconds + 1),
					.attack=!noteSentToMeters[note.voiceIndex]
				});
				noteSentToMeters[note.voiceIndex] = true;
			}
			meterTime = sampleCounter/sampleRate;
			// Schedule next meters after the appropriate amount of audio
			meterIntervalCounter += meterInterval;
			
			// Ready to send
			sentMeters.clear();
			host->request_callback(host);
		}
		
		return CLAP_PROCESS_CONTINUE;
	}
	
	struct MetersNote {
		float key, hue, brightness, width;
		bool attack;
	};
	std::vector<MetersNote> metersNotes;
	// This is called on the UI thread, to serialise the meters we copied over in `.process()`
	void writeMeters(std::vector<unsigned char> &bytes) {
		signalsmith::cbor::CborWriter cbor{bytes};
		cbor.openMap(2);

		cbor.addUtf8("time");
		cbor.addFloat(meterTime);

		cbor.addUtf8("keys");
		cbor.openArray(metersNotes.size());
		for (auto &n : metersNotes) {
			cbor.openMap();
			cbor.addUtf8("key");
			cbor.addFloat(n.key);
			cbor.addUtf8("hue");
			cbor.addFloat(n.hue);
			cbor.addUtf8("brightness");
			cbor.addFloat(n.brightness);
			cbor.addUtf8("width");
			cbor.addFloat(n.width);
			cbor.addUtf8("attack");
			cbor.addBool(n.attack);
			cbor.close();
		}
	}
	
	std::atomic_flag stateIsClean = ATOMIC_FLAG_INIT;
	void pluginOnMainThread() {
		if (hostState && !stateIsClean.test_and_set()) {
			hostState->mark_dirty(host);
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
		} else if (!std::strcmp(extId, CLAP_EXT_NOTE_PORTS)) {
			static const clap_plugin_note_ports ext{
				.count=clapPluginMethod<&Plugin::notePortsCount>(),
				.get=clapPluginMethod<&Plugin::notePortsGet>(),
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
		} else if (!std::strcmp(extId, webview_gui::CLAP_EXT_WEBVIEW)) {
			static const webview_gui::clap_plugin_webview ext{
				.get_uri=clapPluginMethod<&Plugin::webviewGetUri>(),
				.get_resource=clapPluginMethod<&Plugin::webviewGetResource>(),
				.receive=clapPluginMethod<&Plugin::webviewReceive>(),
			};
			return &ext;
		} else if (!std::strcmp(extId, CLAP_EXT_GUI)) {
			return webview.extPluginGui;
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
		stateIsClean.test_and_set();
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

	// some hosts (*cough* REAPER *cough*) give us a stereo input/output port unless we support this extension to say we have none
	uint32_t audioPortsCount(bool isInput) {
		return 0;
	}
	bool audioPortsGet(uint32_t index, bool isInput, clap_audio_port_info *info) {
		return false;
	}

	// ---- note ports ----

	uint32_t notePortsCount(bool isInput) {
		return 1;
	}
	bool notePortsGet(uint32_t index, bool isInput, clap_note_port_info *info) {
		if (index > notePortsCount(isInput)) return false;
		*info = {
			.id=0xC0DEBA55,
			.supported_dialects=CLAP_NOTE_DIALECT_CLAP|CLAP_NOTE_DIALECT_MIDI|CLAP_NOTE_DIALECT_MIDI_MPE,
			.preferred_dialect=CLAP_NOTE_DIALECT_CLAP,
			.name={'n', 'o', 't', 'e', 's'}
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
				if (param->formatFn) {
					auto str = param->formatFn(value);
					std::strncpy(text, str.c_str(), textCapacity);
				} else {
					std::snprintf(text, textCapacity, param->formatString, value);
				}
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
	
	webview_gui::ClapWebviewGui webview;
	std::atomic_flag sentWebviewState = ATOMIC_FLAG_INIT;
	
	int32_t webviewGetUri(char *uri, uint32_t uri_capacity) {
		std::string fileUrl = "file://" + clapBundleResourceDir + "/example-keyboard/keyboard.html";
#ifdef WIN32
		for (auto &c : fileUrl) {
			if (c == '\\') c = '/';
		}
#endif
		if (uri) std::strncpy(uri, fileUrl.c_str(), uri_capacity);
		return fileUrl.size();
	}
	
	bool webviewGetResource(const char *path, char *mediaType, uint32_t mediaTypeCapacity, const clap_ostream *stream) {
		// Since we're using an absolute (`file:`) URL, we don't need this
		return false;
	}

	bool webviewReceive(const void *bytes, uint32_t length) {
		using Cbor = signalsmith::cbor::CborWalker;
		Cbor cbor{(const unsigned char *)bytes, length};
		
		// Just a number on its own -> FPS
		if (cbor.isNumber()) {
			double fps = cbor;
			meterInterval = 1/fps;
			meterStopCounter = 0.5; // send 500ms of meters before requiring another FPS update
			if (meterIntervalCounter < -meterInterval) meterIntervalCounter = 0;
		} else if (cbor.isMap()) {
			clap_event_note event{
				.header={
					.size=sizeof(clap_event_note),
					.time=0,
					.space_id=CLAP_CORE_EVENT_SPACE_ID,
					.type=CLAP_EVENT_NOTE_ON,
					.flags=CLAP_EVENT_IS_LIVE
				},
				.note_id=-1,
				.port_index=0,
				.channel=0,
				.key=60,
				.velocity=0
			};
			cbor.forEachPair([&](Cbor key, Cbor value){
				if (key == "action") {
					if (value == "up") event.header.type = CLAP_EVENT_NOTE_OFF;
				} else if (key == "key") {
					event.key = value;
				} else if (key == "velocity") {
					event.velocity = value;
				}
			});
			
			std::lock_guard<std::mutex> guard{outputEventMutex}; // OK to block (if the audio thread is processing right now), this UI thread is not realtime
			outputEventQueue.push_back(event);
		}
		return !cbor.error();
	}
	void webviewSendIfNeeded() {
		if (!sentMeters.test_and_set()) {
			std::vector<unsigned char> bytes;
			writeMeters(bytes);
			hasMeters.clear(); // We're done with the metering data - `.process()` can fill it up again
			hostWebview->send(host, bytes.data(), bytes.size());
		}

		if (!sentWebviewState.test_and_set()) {
			std::vector<unsigned char> bytes;
			signalsmith::cbor::CborWriter cbor{bytes};
			cbor.openMap();
			
			for (auto *param : params) {
				if (param->sentUiState.test_and_set()) continue;
				cbor.addUtf8(param->key);
				cbor.openMap(1);
				cbor.addUtf8("value");
				cbor.addFloat(param->value);
			}
			cbor.close();
			
			hostWebview->send(host, bytes.data(), bytes.size());
		}
	}
};
