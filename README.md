# Signalsmith's CLAP Examples/Helpers

The CLAP API is neat and elegant, and can be used from C++ in a very bare-bones way.  Over time, I've started to prefer using this directly instead of using a framework to abstract things.  All this needs is a little template helper ([`pluginMethod()`](include/signalsmith-clap/cpp.h)) which lets you make plain-C function pointers to C++ methods.

This repo contains some example plugins using this approach, as well as demonstrating the webview extension from CLAP 1.2.7.  You can see the WebAssembly builds of these plugins running in a [demo browser host](https://signalsmith-audio.github.io/wasm-clap-browserhost/?module=plugin/example-plugins-wasm32.wclap.tar.gz).

There are also some additional helpers, and common dependencies which I use personally, so this repo can also be included in other project.  These helpers are all independent (so you can pick-and-choose), and none of them are top-level classes you *must* inherit from.

<p style="text-align:center">⚠️ WIP, only fully tested on MacOS and WebAssembly so far.</p>

## Code structure

The plugin-factory functions are defined in [`source/plugins.cpp`](source/plugins.cpp).  These are then collected into the `clap_entry` symbol in [`source/clap_entry.cpp`](source/clap_entry.cpp).  This two-part setup is used by the `make_clapfirst_plugins()` CMake function (see [`CMakeLists.txt`](CMakeLists.txt)) so it can hide all the symbols.

Each example plugin is in its own sub-directory, and they demonstrate slightly different things:

### Synth: sine-pluck

This synth has no dependencies aside from the CLAP headers, the `clap-wrappers` CMake tools (which can build VST3/others from the CLAP implementation), and the helpers in this repo.

It has note-in and audio in/out ports.  It has no UI, so will show the host's default sliders.  State is saved using a very basic string serialisation.

### Audio plugin: Chorus

This uses dependencies from `modules/`.

It has audio in/out ports, processing the audio using the chorus from [signalsmith-basics](https://github.com/Signalsmith-Audio/basics).  It implements `clap.gui` to provide native UIs using [webview-gui](https://github.com/geraintluff/webview-gui), and saves its state as [CBOR](https://github.com/geraintluff/cbor-walker).

### Note plugin: velocity randomiser

This uses the CLAP webview helper from [webview-gui](https://github.com/geraintluff/webview-gui?tab=readme-ov-file#clap-helper).  This helper lets your plugin implement the `clap.webview/3` extension, and then it implements `clap.gui` for you by hooking up the plugin's webview extension to a native webview.

Using the webview extension means the plugin can still show a UI when compiled to WebAssembly, since platform-native UI pointers can't be used from inside the WASM sandbox.  The resources for this webview UI are served from the plugin's memory (using `clap_plugin_webview.get_resource()`.

### Note plugin: piano display/input

This uses the same CLAP webview helper as the velocity randomiser, but with a more complex UI.

The webview resources are included in the plugin bundle, and referenced using `file:` URLs.  This only works for targets which have bundles.

## Building

This project builds using CMake, using [`clap-wrapper`](https://github.com/free-audio/clap-wrapper) to (optionally) produce VST3 plugins from the CLAP.

For WebAssembly/WASI, clap-wrapper supports both [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) and Emscripten, but I'd recommend wasi-sdk because it's just Clang plus some prebuilt system libraries.  Emscripten is more opinionated and requires extra config to stop it generating JS stuff which we don't need.

### Native builds

```sh
mkdir -p out
# Generate (or update) the build project
cmake -B out/build # -G Xcode
# Build the project
cmake --build out/build --target example-plugins_clap --config Release
```

If you're not familiar with CMake:

* The `example-plugins_clap` target will generate `example-plugins.clap` inside `build/` (or `build/Release/`)
* When generating the project, you can specify a particular back-end, e.g. `-G Xcode`.  If you're using the default one, it might not support multiple build configs, so specify `-DCMAKE_BUILD_TYPE=Release` when generating the build project
* I personally add `-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=..` when generating as well, which puts the output in `out/` instead of `out/build`

For personal convenience when developing on my Mac, I've included a `Makefile` which calls through to CMake.  It assumes a Mac system with Xcode and REAPER installed, so if you run `make dev-example-plugins` it will build the plugins and open REAPER to test them.

### WebAssembly (WASI-SDK)

With wasi-sdk, just point CMake at the appropriate toolchain when generating the project:

```sh
cmake . -B out/build-wasi -DCMAKE_TOOLCHAIN_FILE=wasi-sdk/share/cmake/wasi-sdk-pthread.cmake  -DCMAKE_BUILD_TYPE=Release
cmake --build out/build-wasi --target example-plugins_wclap --config Release
```

This will generate a directory including `module.wasm` (the actual binary) plus all the resources,  To distribute this as a WCLAP bundle, pack it up into a `.tar.gz`:

```sh
pushd path/to/example-plugins.wclap/;
tar --exclude=".*" -vczf ../example-plugins.wclap.tar.gz *
popd
```
 
I'd recommend using the `wasi-sdk-pthread.cmake` setup, but if your plugin doesn't spawn any threads of its own then it could use single-threaded configs.

### WebAssembly (Emscripten)

To build with Emscripten, set up the environment, and then use the `emcmake` helper to create the build project:

```sh
. "$(EMSDK)/emsdk_env.sh"
emcmake cmake . -B out/build-emscripten -DCMAKE_BUILD_TYPE=Release
cmake --build out/build-emscripten --target example-plugins_wclap --config Release
```

Emscripten doesn't have proper WASI-threads support, so while plugins built like this can be used from multiple threads (main/audio/etc.), if you try to _spawn_ your own threads it will throw an error.
