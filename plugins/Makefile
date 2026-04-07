.PHONY: build run clean

PLUGIN ?= fifo

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LIB_EXT := dylib
else
LIB_EXT := so
endif

# Accept either:
#   make run PLUGIN=gdsf
#   make run gdsf
#   make build gdsf
ifneq ($(filter build run,$(MAKECMDGOALS)),)
ifneq ($(word 2,$(MAKECMDGOALS)),)
PLUGIN := $(word 2,$(MAKECMDGOALS))
$(eval $(PLUGIN):;@:)
endif
endif

build:
	mkdir -p build && cd build/ && cmake -G Ninja .. && ninja

run: build
	cd build && ../../build/bin/cachesim ../../data/cloudPhysicsIO.vscsi vscsi pluginCache 1MB \
	  -e "plugin_path=libplugin_$(PLUGIN)_hooks.$(LIB_EXT)"

clean:
	rm -rf build
