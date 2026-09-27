CXX      ?= clang++
BASEFLAGS := -std=c++17 -Wall -Wextra -Werror -g
CXXFLAGS := $(BASEFLAGS) -fsanitize=address,undefined
SRC_DIR  := ../NumarkNS7Driver/Sources
BUILD    := build

SOURCES  := $(sort $(wildcard *.cpp))
HEADERS  := TestHarness.h TestFixtures.h $(SRC_DIR)/NS7Protocol.h
PROTO    := $(SRC_DIR)/NS7Protocol.h

# Coverage (restricted to NS7Protocol.h) via LLVM source-based coverage.
PROFDATA ?= xcrun llvm-profdata
LLVMCOV  ?= xcrun llvm-cov
COVBIN   := $(BUILD)/NS7ProtocolTests-cov

.PHONY: test coverage coverage-show clean

# Default: the whole suite under AddressSanitizer + UndefinedBehaviorSanitizer.
test: $(BUILD)/NS7ProtocolTests
	./$(BUILD)/NS7ProtocolTests

$(BUILD)/NS7ProtocolTests: $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) $(SOURCES) -o $@

# Line/region/branch coverage of NS7Protocol.h: summary table.
coverage: $(BUILD)/coverage.profdata
	$(LLVMCOV) report $(COVBIN) -instr-profile=$< -show-branch-summary $(PROTO)

# Annotated source listing with branch counts, uncovered parts highlighted.
coverage-show: $(BUILD)/coverage.profdata
	$(LLVMCOV) show $(COVBIN) -instr-profile=$< -show-branches=count \
		-show-instantiations=false $(PROTO)

$(BUILD)/coverage.profdata: $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(BASEFLAGS) -O0 -fprofile-instr-generate -fcoverage-mapping -I$(SRC_DIR) \
		$(SOURCES) -o $(COVBIN)
	LLVM_PROFILE_FILE=$(BUILD)/coverage.profraw ./$(COVBIN) > /dev/null
	$(PROFDATA) merge -sparse $(BUILD)/coverage.profraw -o $@

clean:
	rm -rf $(BUILD)
