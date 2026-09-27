CXX      ?= clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -g -MMD -MP
BUILD    := build

# Pure modules (no CoreMIDI) are shared with the unit tests.
PURE_SRCS := src/json.cpp src/midi_msg.cpp src/summarize.cpp src/inventory.cpp src/verify_core.cpp
APP_SRCS  := $(PURE_SRCS) src/midi_io.cpp src/answers.cpp src/learn.cpp src/ledscan.cpp src/verify.cpp src/main.cpp
TEST_SRCS := $(PURE_SRCS) tests/test_main.cpp

APP_OBJS  := $(APP_SRCS:%.cpp=$(BUILD)/%.o)
TEST_OBJS := $(TEST_SRCS:%.cpp=$(BUILD)/%.o)

.PHONY: all test clean

all: $(BUILD)/ns7midi

$(BUILD)/ns7midi: $(APP_OBJS)
	$(CXX) $(CXXFLAGS) $^ -framework CoreMIDI -framework CoreFoundation -o $@

$(BUILD)/unit_tests: $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Isrc -c $< -o $@

# Unit tests for the pure code, then the end-to-end self-test: ledscan
# --dry-run and learn --replay --script. Neither opens a CoreMIDI connection.
test: $(BUILD)/unit_tests $(BUILD)/ns7midi
	$(BUILD)/unit_tests
	./tests/selftest.sh $(BUILD)/ns7midi

clean:
	rm -rf $(BUILD)

-include $(APP_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
