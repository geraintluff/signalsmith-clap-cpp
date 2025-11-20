#include "clap/clap.h"

#include "signalsmith-clap/cpp.h"
#include "signalsmith-clap/note-manager.h"
#include "signalsmith-clap/params.h"

#include "cbor-walker/cbor-walker.h"
#include "webview-gui/clap-webview-gui.h"

#include "../plugins.h"

#include <atomic>
#include <random>

struct ExampleNotePlugin {
	using Plugin = ExampleNotePlugin;
	
	static const clap_plugin_descriptor * getPluginDescriptor() {
		static const char * features[] = {
			CLAP_PLUGIN_FEATURE_NOTE_EFFECT,
			nullptr
		};
		static clap_plugin_descriptor descriptor{
			.id="uk.co.signalsmith-audio.plugins.example-note-plugin",
			.name="C++ Example Note Plugin",
			.vendor="Signalsmith Audio",
			.url=nullptr,
			.manual_url=nullptr,
			.support_url=nullptr,
			.version="1.0.0",
			.description="Note plugin from a starter CLAP project",
			.features=features
		};
		return &descriptor;
	};

	static const clap_plugin * create(const clap_host *host) {
		return &(new Plugin(host))->clapPlugin;
	}

	// Until CLAP 1.2.7 is properly released, we import the definitions from here
	using clap_plugin_webview = webview_gui::clap_plugin_webview;
	using clap_host_webview = webview_gui::clap_host_webview;
	
	const clap_host *host;
	// Extensions aren't filled out until `.pluginInit()`
	const clap_host_state *hostState = nullptr;
	const clap_host_audio_ports *hostAudioPorts = nullptr;
	const clap_host_note_ports *hostNotePorts = nullptr;
	const clap_host_params *hostParams = nullptr;
	const clap_host_webview *hostWebview = nullptr;

	uint32_t noteIdCounter = 0;
	struct OutputNote {
		int32_t noteId;
		double velocity;
		uint64_t timeSinceTrigger;
	};
	std::vector<OutputNote> outputNotes;
	using NoteManager = signalsmith::clap::NoteManager;
	NoteManager noteManager{512};
	double sampleRate = 1;
	static constexpr double noteTailSeconds = 1; // Notes might get sent expression events even after release - this determines how long after release we keep them in the list

	using Param = signalsmith::clap::Param;
	Param log2Rate{"log2Rate", "rate (log2)", 0x01234567, -2.0, 1.0, 4.0};
	Param regularity{"regularity", "regularity", 0x02468ACE, 0.0, 0.65, 1.0};
	Param velocityRand{"velocityRand", "velocity rand.", 0x12345678, 0.0, 0.5, 1.0};
	std::array<Param *, 3> params{&log2Rate, &regularity, &velocityRand};

	void resendAllUiState() {
		// Send everything
		for (auto *param : params) {
			param->sentUiState.clear();
		}
		sentWebviewState.clear();
	}

	ExampleNotePlugin(const clap_host *host) : host(host) {
		outputNotes.resize(noteManager.polyphony());
		log2Rate.formatFn = [](double value){
			char text[16] = {};
			std::snprintf(text, 15, "%.2f Hz", std::exp2(value));
			return std::string(text);
		};
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
	
	std::uniform_real_distribution<double> unitReal{0, 1};
	clap_process_status pluginProcess(const clap_process *process) {
		auto *eventsOut = process->out_events;

		noteManager.startBlock();
		
		double rateHz = std::exp2(log2Rate.value);
		double periodSamples = sampleRate/rateHz;
		double minPeriodSamples = periodSamples*regularity.value;
		double retriggerProb = 1/(periodSamples - minPeriodSamples + 1e-30);
		
		auto *eventsIn = process->in_events;
		uint32_t eventCount = eventsIn->size(eventsIn);
		uint32_t blockProcessedTo = 0;
		for (uint32_t i = 0; i <= eventCount; ++i) {
			auto *event = (i < eventCount) ? eventsIn->get(eventsIn, i) : nullptr;
			uint32_t eventTime = event ? event->time : process->frames_count;

			// Process up to the next event (or block end)
			while (blockProcessedTo < eventTime) {
				for (auto &note : noteManager) {
					if (note.released()) continue;
					auto &outNote = outputNotes[note.voiceIndex];
					if (outNote.timeSinceTrigger++ < minPeriodSamples) continue;
					
					if (unitReal(randomEngine) < retriggerProb) {
						outNote.timeSinceTrigger = 0;

						// Stop previous note
						clap_event_note noteEvent{
							.header={
								.size=sizeof(clap_event_note),
								.time=blockProcessedTo,
								.space_id=CLAP_CORE_EVENT_SPACE_ID,
								.type=CLAP_EVENT_NOTE_OFF,
								.flags=0
							},
							.note_id=outNote.noteId,
							.port_index=note.port,
							.channel=note.channel,
							.key=note.baseKey,
							.velocity=0
						};
						eventsOut->try_push(eventsOut, &noteEvent.header);

						// start new note
						noteEvent.header.type = CLAP_EVENT_NOTE_ON;
						// with a new ID
						outNote.noteId = int32_t(noteIdCounter++);
						noteEvent.note_id = outNote.noteId;
						if (noteIdCounter >= 0x80000000) noteIdCounter = 0;
						// and random velocity
						auto randVel = 0.5 + (unitReal(randomEngine) - 0.5)*velocityRand.value;
						noteEvent.velocity = outNote.velocity*randVel/(1 - outNote.velocity - randVel + 2*outNote.velocity*randVel);
						eventsOut->try_push(eventsOut, &noteEvent.header);
						// TODO: immediately send all note expression events
					}
				}
				blockProcessedTo++;
			}
			
			if (!event) {
				noteManager.processTo(process->frames_count);
				continue;
			}
			
			if (auto note = noteManager.wouldStart(event)) {
				noteManager.start(*note, eventsOut);
				auto &outNote = outputNotes[note->voiceIndex];
				outNote.velocity = note->velocity;
				outNote.timeSinceTrigger = 0;
				outNote.noteId = int32_t(noteIdCounter++);
				if (noteIdCounter >= 0x80000000) noteIdCounter = 0;

				// Randomise first velocity as well
				auto randVel = 0.5 + (unitReal(randomEngine) - 0.5)*velocityRand.value;
				note->velocity = note->velocity*randVel/(1 - note->velocity - randVel + 2*note->velocity*randVel);

				// Sent note-start
				clap_event_note noteEvent{
					.header={
						.size=sizeof(clap_event_note),
						.time=event->time,
						.space_id=CLAP_CORE_EVENT_SPACE_ID,
						.type=CLAP_EVENT_NOTE_ON,
						.flags=0
					},
					.note_id=outNote.noteId,
					.port_index=note->port,
					.channel=note->channel,
					.key=note->baseKey,
					.velocity=note->velocity
				};
				eventsOut->try_push(eventsOut, &noteEvent.header);
			} else if (auto note = noteManager.wouldRelease(event)) {
				noteManager.release(*note);
				auto &outNote = outputNotes[note->voiceIndex];

				// Sent note-end
				clap_event_note noteEvent{
					.header={
						.size=sizeof(clap_event_note),
						.time=event->time,
						.space_id=CLAP_CORE_EVENT_SPACE_ID,
						.type=CLAP_EVENT_NOTE_OFF,
						.flags=0
					},
					.note_id=outNote.noteId,
					.port_index=note->port,
					.channel=note->channel,
					.key=note->baseKey,
					.velocity=0
				};
				eventsOut->try_push(eventsOut, &noteEvent.header);
			} else {
				noteManager.processEvent(event, eventsOut);
				if (event->type == CLAP_EVENT_NOTE_EXPRESSION) {
					sendWithReplacedNoteId<clap_event_note_expression>(event, eventsOut, true);
				} else if (event->type == CLAP_EVENT_PARAM_VALUE) {
					sendWithReplacedNoteId<clap_event_param_value>(event, eventsOut);
				} else if (event->type == CLAP_EVENT_PARAM_MOD) {
					sendWithReplacedNoteId<clap_event_param_mod>(event, eventsOut);
				} else {
					eventsOut->try_push(eventsOut, event);
				}
			}
			processEvent(event);
		}

		for (auto &note : noteManager) {
			if (note.released() && note.ageAt(process->frames_count) > sampleRate*noteTailSeconds) {
				noteManager.stop(note, eventsOut);
			}
		}

		return CLAP_PROCESS_CONTINUE;
	}
	
	template<class ClapEvent>
	void sendWithReplacedNoteId(const clap_event_header *event, const clap_output_events *eventsOut, bool expandWildcards=false) {
		ClapEvent clapEvent = *(ClapEvent *)event;
		bool wildcard = (clapEvent.note_id == -1);
		if (wildcard && !expandWildcards) {
			eventsOut->try_push(eventsOut, &clapEvent.header);
			return;
		}
		for (auto &note : noteManager) {
			if (note.matchEvent(clapEvent, true)) {
				auto &outNote = outputNotes[note.voiceIndex];
				clapEvent.note_id = outNote.noteId;
				eventsOut->try_push(eventsOut, &clapEvent.header);
				if (!wildcard) break;
			}
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
			// Our webview helper defines this one
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
		resendAllUiState();
		host->request_callback(host);
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
		const char *relativeUrl = "/example-note-plugin/";
		if (uri) std::strncpy(uri, relativeUrl, uri_capacity);
		return std::strlen(relativeUrl);
	}
	
	bool webviewGetResource(const char *path, char *mediaType, uint32_t mediaTypeCapacity, const clap_ostream *stream);

	bool webviewReceive(const void *bytes, uint32_t length) {
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
			stateIsClean.clear();
		};
		
		Cbor cbor{(const unsigned char *)bytes, length};
		if (cbor.utf8View() == "ready") {
			resendAllUiState();
			webviewSendIfNeeded();
			return true;
		}
		
		cbor.forEachPair([&](Cbor key, Cbor value){
			auto keyString = key.utf8View();
			for (auto *param : params) {
				if (keyString == param->key) {
					updateParam(*param, value);
				}
			}
		});

		if (hostParams) hostParams->request_flush(host);
		pluginOnMainThread();

		return !cbor.error();
	}
	void webviewSendIfNeeded() {
		if (sentWebviewState.test_and_set()) return;

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
		
		webview.send(bytes.data(), bytes.size());
	}
	
	std::default_random_engine randomEngine{std::random_device{}()};
};
