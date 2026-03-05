#pragma once

#include "clap/plugin.h"
#include "clap/stream.h"

#include <string>
#include <vector>

namespace signalsmith { namespace clap {

// ---- pluginMethod(): make a plain-C function which calls a C++ method ----

template <typename T>
struct ClapPluginMethodHelper;

// Returns a plain-C function which calls a given C++ method
template<auto methodPtr>
auto pluginMethod() {
	using C = ClapPluginMethodHelper<decltype(methodPtr)>;
	return C::template callMethod<methodPtr>;
}

// Partial specialisation used to expand the method signature
template <class Object, typename Return, typename... Args>
struct ClapPluginMethodHelper<Return (Object::*)(Args...)> {
	// Templated static method which forwards to a method on the plugin
	template<Return (Object::*methodPtr)(Args...)>
	static Return callMethod(const clap_plugin *plugin, Args... args) {
		auto *obj = (Object *)plugin->plugin_data;
		return (obj->*methodPtr)(args...);
	}
};

// ---- pluginMemberMethod(): make a plain-C function which calls a C++ method on a plugin's field ----

template<typename T1, typename T2>
struct ClapPluginMemberMethodHelper;

// Returns a plain-C function which calls a C++ method on a member field
template<auto memberPtr, auto methodPtr>
auto pluginMemberMethod() {
	using C = ClapPluginMemberMethodHelper<decltype(memberPtr), decltype(methodPtr)>;
	return C::template callMemberMethod<memberPtr, methodPtr>;
}

template<class Plugin, class Object, typename Return, typename... Args>
struct ClapPluginMemberMethodHelper<Object Plugin::*, Return (Object::*)(Args...)> {
	// Templated static method which forwards to a method on a member
	template<Object Plugin::*memberPtr, Return (Object::*methodPtr)(Args...)>
	static Return callMemberMethod(const clap_plugin *plugin, Args... args) {
		auto *pObj = (Plugin *)plugin->plugin_data;
		Object &obj = pObj->*memberPtr;
		return (obj.*methodPtr)(args...);
	}
};


// ---- checks for a host extension ----

template<class HostExtension>
bool getHostExtension(const clap_host *host, const char *extId, const HostExtension *&hostExt) {
	hostExt = (const HostExtension *)host->get_extension(host, extId);
	return hostExt;
}

// ---- read/write strings or byte-vectors using CLAP stream(s) ----

template<class Container>
bool readAllFromStream(Container &byteContainer, const clap_istream *istream, size_t chunkBytes=1024) {
	while (1) {
		size_t index = byteContainer.size();
		byteContainer.resize(index + chunkBytes);
		int64_t result = istream->read(istream, (void *)&byteContainer[index], uint64_t(chunkBytes));
		if (result == chunkBytes) {
			continue;
		} else if (result >= 0) {
			byteContainer.resize(index + result);
			if (result == 0) return true;
		} else {
			return false;
		}
	}
}

inline bool writeAllToStream(const void *buffer, size_t length, const clap_ostream *ostream) {
	size_t index = 0;
	while (length > index) {
		int64_t result = ostream->write(ostream, (const void *)((size_t)buffer + index), uint64_t(length - index));
		if (result <= 0) return false;
		index += result;
	}
	return true;
}

template<class Container>
bool writeAllToStream(const Container &c, const clap_ostream *ostream) {
	return writeAllToStream((const void *)c.data(), c.size()*sizeof(c[0]), ostream);
}

}} // namespace
