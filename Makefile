BUILD_DIR  := build
CMAKE_OPTS := -S . -B $(BUILD_DIR) -DBUILD_TESTS=ON

.PHONY: all clean test

all: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) -j

$(BUILD_DIR)/Makefile: CMakeLists.txt src/*.cpp src/*.h tests/unit/*.cpp
	mkdir -p $(BUILD_DIR)
	cmake $(CMAKE_OPTS)

clean:
	rm -rf $(BUILD_DIR)

test: all
	cmake --build $(BUILD_DIR) --target unit_tests -j
	ctest --test-dir $(BUILD_DIR) --output-on-failure
