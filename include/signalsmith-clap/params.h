#pragma once

#include "./cpp.h"

#include "clap/events.h"
#include "clap/ext/params.h"

#include <atomic>
#include <functional>
#include <initializer_list>

namespace signalsmith { namespace clap {

/** A parameter object which can send gesture/value events back to the host when needed.

It can also track whether its value has been sent to the UI or not, but doesn't specify how that should be done.
*/
struct Param {
	double value = 0;
	clap_param_info info;
	const char *formatString = "%.2f";
	std::function<std::string(double)> formatFn;
	const char *key; // useful when debugging, or when an integer key is awkward
	
	// User interactions which we need to send as events to the host
	std::atomic_flag sentValue = ATOMIC_FLAG_INIT;
	std::atomic_flag sentGestureStart = ATOMIC_FLAG_INIT;
	std::atomic_flag sentGestureEnd = ATOMIC_FLAG_INIT;

	// Value change which we might need to send to the UI
	std::atomic_flag sentUiState = ATOMIC_FLAG_INIT;

	Param(const char *key, const char *name, clap_id paramId, double min, double initial, double max) : key(key), value(initial) {
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
	Param(const Param &other) = delete;
	
	void invalidate() {
		sentUiState.clear();
		sentValue.clear();
	}
	
	void setValueFromEvent(const clap_event_param_value &paramEvent) {
		value = paramEvent.value;
		sentUiState.clear();
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

/** A collection of Parameters */
struct ParamManager {

	ParamManager & add() {
		return *this;
	}
	template<class... Others>
	ParamManager & add(Param &param, Others ...others) {
		paramList.push_back(&param);
		return add(others...);
	}
	
	void init(const clap_host *host) {
		getHostExtension(host, CLAP_EXT_PARAMS, hostParams);
	}
	
	void clearUiState() {
		for (auto *param : paramList) {
			param->sentUiState.clear();
		}
	}

	void sendEvents(const clap_output_events *eventsOut) {
		for (auto *param : paramList) {
			param->sendEvents(eventsOut);
		}
	}
	
	bool processEvent(const clap_event_header *event) {
		if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return false;
		if (event->type == CLAP_EVENT_PARAM_VALUE) {
			auto &eventParam = *(const clap_event_param_value *)event;
			if (eventParam.cookie) {
				// if provided, it's the parameter
				auto *param = (Param *)eventParam.cookie;
				param->setValueFromEvent(eventParam);
			} else {
				// Otherwise, match the ID
				for (auto *param : paramList) {
					if (eventParam.param_id == param->info.id) {
						param->setValueFromEvent(eventParam);
						break;
					}
				}
			}
			return true;

		}
		return false;
	}
	
	template<auto memberPtr>
	const void * ext() {
		static const clap_plugin_params ext{
			.count=pluginMemberMethod<memberPtr, &ParamManager::paramsCount>(),
			.get_info=pluginMemberMethod<memberPtr, &ParamManager::paramsGetInfo>(),
			.get_value=pluginMemberMethod<memberPtr, &ParamManager::paramsGetValue>(),
			.value_to_text=pluginMemberMethod<memberPtr, &ParamManager::paramsValueToText>(),
			.text_to_value=pluginMemberMethod<memberPtr, &ParamManager::paramsTextToValue>(),
			.flush=pluginMemberMethod<memberPtr, &ParamManager::paramsFlush>(),
		};
		return &ext;
	}

	uint32_t paramsCount() {
		return uint32_t(paramList.size());
	}

	bool paramsGetInfo(uint32_t index, clap_param_info *info) {
		if (index >= paramList.size()) return false;
		*info = paramList[index]->info;
		return true;
	}
	
	bool paramsGetValue(clap_id paramId, double *value) {
		for (auto *param : paramList) {
			if (param->info.id == paramId) {
				*value = param->value;
				return true;
			}
		}
		return false;
	}
	
	bool paramsValueToText(clap_id paramId, double value, char *text, uint32_t textCapacity) {
		for (auto *param : paramList) {
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
		return false; // not supported (yet)
	}
	
	void paramsFlush(const clap_input_events *eventsIn, const clap_output_events *eventsOut) {
		uint32_t eventCount = eventsIn->size(eventsIn);
		for (uint32_t i = 0; i < eventCount; ++i) {
			auto *event = eventsIn->get(eventsIn, i);
			processEvent(event);
			eventsOut->try_push(eventsOut, event);
		}
		sendEvents(eventsOut);
	}

private:
	std::vector<Param *> paramList;
	const clap_host_params *hostParams = nullptr;
};

}} // namespace
