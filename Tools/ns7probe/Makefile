CXX      ?= clang++
CXXFLAGS := -std=c++17 -fobjc-arc -Wall -Wextra -Werror -g
SRC_DIR  := ../../NumarkNS7Driver/Sources
BUILD    := build

.PHONY: all clean

all: $(BUILD)/ns7probe

$(BUILD)/ns7probe: ns7probe.mm $(SRC_DIR)/NS7Protocol.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) ns7probe.mm -framework Foundation -framework IOKit -framework IOUSBHost -o $@

clean:
	rm -rf $(BUILD)
