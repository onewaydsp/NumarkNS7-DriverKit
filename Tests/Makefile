CXX      ?= clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g
SRC_DIR  := ../NumarkNS7Driver/Sources
BUILD    := build

.PHONY: test clean

test: $(BUILD)/NS7ProtocolTests
	./$(BUILD)/NS7ProtocolTests

$(BUILD)/NS7ProtocolTests: NS7ProtocolTests.cpp $(SRC_DIR)/NS7Protocol.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) NS7ProtocolTests.cpp -o $@

clean:
	rm -rf $(BUILD)
