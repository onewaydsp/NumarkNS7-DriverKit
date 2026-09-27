CXX      ?= clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -g
BUILD    := build

.PHONY: all clean

all: $(BUILD)/ns7midi

$(BUILD)/ns7midi: ns7midi.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) ns7midi.cpp -framework CoreMIDI -framework CoreFoundation -o $@

clean:
	rm -rf $(BUILD)
