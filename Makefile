.PHONY: emsdk
help:
	@echo "\tmake clap-example-plugins\n\tmake vst3-example-plugins\n\tmake dev-example-plugins\n\nWCLAP with wasi-sdk:\n\tmake wasi-example-plugins\n\nWCLAP with Emscripten:\n\tmake emscripten-example-plugins"

clean:
	rm -rf out

out/build: CMakeLists.txt
	cmake . -B out/build -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=.. -G Xcode

clap-%: out/build
	cmake --build out/build --target $*_clap --config Release

vst3-%: out/build
	cmake --build out/build --target $*_vst3 --config Release

######## WCLAP with wasi-sdk

out/build-wasi: CMakeLists.txt
	cmake . -B out/build-wasi -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=../Release -DCMAKE_TOOLCHAIN_FILE=./wasi-sdk/share/cmake/wasi-sdk-pthread.cmake -DCMAKE_BUILD_TYPE=Release

wasi-%: out/build-wasi
	cmake --build out/build-wasi --target $*_wclap --config Release
	cd out/Release/$*.wclap/; tar --exclude=".*" -vczf ../$*.wclap.tar.gz *

####### Open a test project in REAPER #######

CURRENT_DIR := $(shell pwd)

%.RPP:
	mkdir -p `dirname "$@"`
	echo "<REAPER_PROJECT 0.1\n>" > "$@"

reaper-rescan-clap:
	rm -f ~/Library/Application\ Support/REAPER/reaper-clap-*.ini

dev-%: clap-%
	$(MAKE) out/REAPER/$*.RPP
	# Symlink the plugin
	pushd ~/Library/Audio/Plug-Ins/CLAP \
		&& rm -f "$*.clap" \
		&& ln -s "$(CURRENT_DIR)/out/Release/$*.clap"
	# Symlink the bundle's Resources directory
	pushd out/Release/$*.clap/Contents/; rm -rf Resources; ln -s "$(CURRENT_DIR)/resources" Resources
	/Applications/REAPER.app/Contents/MacOS/REAPER out/REAPER/$*.RPP

####### Emscripten #######
# This automatically installs the Emscripten SDK
# if the environment variable EMSDK is not already set

EMSDK ?= $(CURRENT_DIR)/emsdk
EMSDK_ENV = unset CMAKE_TOOLCHAIN_FILE; EMSDK_QUIET=1 . "$(EMSDK)/emsdk_env.sh";

emsdk:
	@ if ! test -d "$(EMSDK)" ;\
	then \
		echo "SDK not found - cloning from Github" ;\
		git clone https://github.com/emscripten-core/emsdk.git "$(EMSDK)" ;\
		cd "$(EMSDK)" && git pull && ./emsdk install latest && ./emsdk activate latest ;\
		$(EMSDK_ENV) emcc --check && python3 --version && cmake --version ;\
	fi

out/build-emscripten: emsdk
	$(EMSDK_ENV) emcmake cmake . -B out/build-emscripten -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=../Release -DCMAKE_BUILD_TYPE=Release

emscripten-%: out/build-emscripten
	$(EMSDK_ENV) cmake --build out/build-emscripten --target $*_wclap --config Release
	cd out/Release/$*.wclap/; tar --exclude=".*" -vczf ../$*.wclap.tar.gz *
