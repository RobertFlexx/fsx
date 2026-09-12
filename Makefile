.PHONY: all build test clean release release-gate
all: build

build:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j

test: build
	ctest --test-dir build --output-on-failure
	FSX=./build/fsx ./scripts/integration.sh

clean:
	rm -rf build build-* 

release: test
	strip build/fsx 2>/dev/null || true

release-gate:
	./scripts/release-gate.sh
