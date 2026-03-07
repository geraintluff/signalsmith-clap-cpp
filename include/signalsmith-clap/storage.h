#pragma once

#include "cbor-walker/cbor-walker.h"

#include <string>
#include <vector>

/* A template-based pattern for reading/writing state, inspired by the JSFX @serialize block

Every relevant (non-vector) class should have a templated `.state()` method:

	struct MyClass {
		double myVar = 2.5;
	
		template<class Storage>
		void state(Storage &storage) {
			storage("my-key", myVar);
			//...
			
			// optional fields, derived from actual state
			storage.extra("$type", "MyClass");
		}
	};

This storage object will either write the current values, or update them from an incoming state.  Keys are C strings (`const char *`), all keys are optional for incoming state, and unfamiliar keys are ignored.

This implementation uses CBOR (which has a compact representation for typed arrays, e.g. `std::vector<float>` or `std::vector<uint16_t>`).  The cbor-walker library comes with a matching JS decoder/encoder for web UIs, which maps these typed arrays to `Float32Array`/`Uint16Array`/etc.

Classes can also *optionally* have a `.uiState()` method, which is called when synchronising state with a web UI.
 */
namespace signalsmith { namespace storage {

// This is the API, but this particular implementation does nothing
struct StorageDummy {
	// Accepts all int and float types, strings, vectors, and any types with a `.state(storage)` method
	template<class V>
	void operator()(const char *, V &) {}

	// Accepts all of the above, plus `const char *` (raw C strings)
	template<class V>
	void extra(const char *, V &) {}
};

// Template-y magic to call `obj.uiState()` only if it exists
namespace _impl {
	template<class Storage, class Obj>
	void optionalUiStorage(Storage &storage, Obj &obj);
}

struct StorageCborWriter {
	StorageCborWriter(const signalsmith::cbor::CborWriter &writer, std::vector<unsigned char> *buffer=nullptr, bool wantsExtra=false) : cbor(writer), wantsExtra(wantsExtra) {
		if (buffer) buffer->resize(0);
		cbor.openMap();
	}
	StorageCborWriter(std::vector<unsigned char> &cborBuffer, bool wantsExtra=false) : StorageCborWriter(signalsmith::cbor::CborWriter(cborBuffer), &cborBuffer, wantsExtra) {}
	
	~StorageCborWriter() {
		cbor.close();
	}

	template<class V>
	void operator()(const char *key, V &value) {
		cbor.addUtf8(key);
		writeValue(value);
	}

	void extra(const char *key, const char *value) {
		cbor.addUtf8(key);
		cbor.addUtf8(value);
	}
	void extra(const char *key, char *value) {
		cbor.addUtf8(key);
		cbor.addUtf8(value);
	}
	template<class V>
	void extra(const char *key, V &value) {
		cbor.addUtf8(key);
		writeValue(value);
	}

private:
	signalsmith::cbor::CborWriter cbor;
	const bool wantsExtra = false;

#define STORAGE_BASIC_INT(V) \
	void writeValue(V &value) { \
		cbor.addInt(value); \
	}
	STORAGE_BASIC_INT(int64_t)
	STORAGE_BASIC_INT(uint64_t)
	STORAGE_BASIC_INT(int32_t)
	STORAGE_BASIC_INT(uint32_t)
	STORAGE_BASIC_INT(int16_t)
	STORAGE_BASIC_INT(uint16_t)
	STORAGE_BASIC_INT(int8_t)
	STORAGE_BASIC_INT(uint8_t)
#undef STORAGE_BASIC_INT

	void writeValue(float &value) {
		cbor.addFloat(value);
	}
	void writeValue(double &value) {
		cbor.addFloat(value);
	}
	void writeValue(bool &value) {
		cbor.addBool(value);
	}
	void writeValue(std::string &str) {
		cbor.addUtf8(str.c_str());
	}

	template<class Item>
	void writeValue(std::vector<Item> &array) {
		cbor.openArray(array.size());
		for (auto &item : array) {
			writeValue(item);
		}
	}

#define STORAGE_TYPED_ARRAY(T) \
	void writeValue(std::vector<T> &array) { \
		cbor.addTypedArray(array.data(), array.size()); \
	}
	STORAGE_TYPED_ARRAY(uint8_t)
	STORAGE_TYPED_ARRAY(int8_t)
	STORAGE_TYPED_ARRAY(uint16_t)
	STORAGE_TYPED_ARRAY(int16_t)
	STORAGE_TYPED_ARRAY(uint32_t)
	STORAGE_TYPED_ARRAY(int32_t)
	STORAGE_TYPED_ARRAY(uint64_t)
	STORAGE_TYPED_ARRAY(int64_t)
	STORAGE_TYPED_ARRAY(float)
	STORAGE_TYPED_ARRAY(double)
#undef STORAGE_TYPED_ARRAY

	template<class Obj>
	void writeValue(Obj &obj) {
		cbor.openMap();
		obj.state(*this);
		if (wantsExtra) {
			_impl::optionalUiStorage(*this, obj);
		}
		cbor.close();
	}
};

struct StorageCborReader {
	using Cbor = signalsmith::cbor::TaggedCborWalker;
	
	StorageCborReader(Cbor c, bool wantsExtra=false) : cbor(c), wantsExtra(wantsExtra) {
		if (cbor.isMap()) cbor = cbor.enter();
	}
	StorageCborReader(const std::vector<unsigned char> &v, bool wantsExtra=false) : StorageCborReader(Cbor(v), wantsExtra) {}
	
	template<class V>
	void operator()(const char *key, V &v) {
		if (filterKeyBytes != nullptr) {
			if (!keyMatch(key, filterKeyBytes, filterKeyLength)) return;
		}
		if (!cbor.isUtf8()) return; // We expect a string key
		// If we have a filter, we *should* be just in front of the appropriate key, but check anyway
		if (!keyMatch(key, (const char *)cbor.bytes(), cbor.length())) {
			return; // key doesn't match
		}
		cbor++;
		readValue(v);
	}

	template<class V>
	void extra(const char *key, const V &v) {}

private:
	Cbor cbor;
	const bool wantsExtra;
	const char *filterKeyBytes = nullptr;
	size_t filterKeyLength = 0;

	template<class Obj>
	void readValue(Obj &obj) {
		if (!cbor.isMap()) return;

		// This calls `obj.state()` multiple times, skipping all but a single key on each pass
		cbor = cbor.forEachPair([&](Cbor key, Cbor value){
			if (!key.isUtf8()) return;

			const char *fkb = filterKeyBytes;
			size_t fkl = filterKeyLength;

			// Temporarily set key, and scan the object for that property
			filterKeyBytes = (const char *)key.bytes();
			filterKeyLength = key.length();
			cbor = key; // `operator()` checks the key and then increments if matched, so it's pointing to the value
			obj.state(*this);
			if (wantsExtra) {
				_impl::optionalUiStorage(*this, obj);
			}

			filterKeyBytes = fkb;
			filterKeyLength = fkl;
		});
	}

#define STORAGE_BASIC_TYPE(V) \
	void readValue(V &v) { \
		v = V(cbor++); \
	}
	STORAGE_BASIC_TYPE(int64_t)
	STORAGE_BASIC_TYPE(uint64_t)
	STORAGE_BASIC_TYPE(int32_t)
	STORAGE_BASIC_TYPE(uint32_t)
	STORAGE_BASIC_TYPE(int16_t)
	STORAGE_BASIC_TYPE(uint16_t)
	STORAGE_BASIC_TYPE(int8_t)
	STORAGE_BASIC_TYPE(uint8_t)
	STORAGE_BASIC_TYPE(float)
	STORAGE_BASIC_TYPE(double)
	STORAGE_BASIC_TYPE(bool)
#undef STORAGE_BASIC_TYPE

	void readValue(std::string &v) {
		if (!cbor.isUtf8()) v.clear();
		v.assign((const char *)cbor.bytes(), cbor.length());
		++cbor;
	}
	
	template<class Item>
	void readVector(std::vector<Item> &array) {
		if (!cbor.isArray()) return;
		size_t length = 0;
		cbor = cbor.forEach([&](Cbor item, size_t index){
			length = index + 1;
			if (array.size() < length) array.resize(length);

			cbor = item;
			readValue(array[index]);
		});
		array.resize(length);
	}

	template<class Item>
	void readValue(std::vector<Item> &array) {
		readVector(array);
	}

#define STORAGE_TYPED_ARRAY(T) \
	void readValue(std::vector<T> &array) { \
		if (cbor.isTypedArray()) { \
			array.resize(cbor.typedArrayLength()); \
			cbor.readTypedArray(array); \
		} else { \
			readVector<T>(array); \
		} \
	}
	STORAGE_TYPED_ARRAY(uint8_t)
	STORAGE_TYPED_ARRAY(int8_t)
	STORAGE_TYPED_ARRAY(uint16_t)
	STORAGE_TYPED_ARRAY(int16_t)
	STORAGE_TYPED_ARRAY(uint32_t)
	STORAGE_TYPED_ARRAY(int32_t)
	STORAGE_TYPED_ARRAY(uint64_t)
	STORAGE_TYPED_ARRAY(int64_t)
	STORAGE_TYPED_ARRAY(float)
	STORAGE_TYPED_ARRAY(double)
#undef STORAGE_TYPED_ARRAY

	static bool keyMatch(const char *key, const char *filterKeyBytes, size_t filterKeyLength) {
		for (size_t i = 0; i < filterKeyLength; ++i) {
			if (key[i] != filterKeyBytes[i]) return false;
		}
		return key[filterKeyLength] == 0;
	}
};

namespace _impl {

	template<class Storage, class Obj, typename=void>
	void callUiStorage(Storage &storage, Obj &obj) {
		return obj.uiState(storage);
	}

	template<class Storage, class Obj, typename=void>
	struct UiStorage {
		static void optionalUiStorage(Storage &storage, Obj &obj) {}
	};

	// Specialisation, using SFINAE
	template<class Storage, class Obj>
	struct UiStorage<Storage, Obj, decltype(callUiStorage(std::declval<Storage &>(), std::declval<Obj &>()))> {
		static void optionalUiStorage(Storage &storage, Obj &obj) {
			obj.uiState(storage);
		}
	};

	template<class Storage, class Obj>
	void optionalUiStorage(Storage &storage, Obj &obj) {
		UiStorage<Storage, Obj>::optionalUiStorage(storage, obj);
	}
}

}} // namespace
