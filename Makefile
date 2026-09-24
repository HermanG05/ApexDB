CXX = clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -Iinclude
LDFLAGS += -pthread

SRCDIR := src
BUILDDIR := build
BINDIR := bin

CORE_SOURCES := \
	avl.cpp hashtable.cpp heap.cpp thread_pool.cpp zset.cpp \
	protocol.cpp kv_store.cpp sharding.cpp tcp_rpc.cpp server_loop.cpp

CORE_OBJECTS := $(addprefix $(BUILDDIR)/,$(CORE_SOURCES:.cpp=.o))

.PHONY: all clean test integration

all: $(BINDIR)/kv-server $(BINDIR)/kv-shard $(BINDIR)/kv-proxy $(BINDIR)/kv-client \
     $(BINDIR)/test_avl $(BINDIR)/test_heap $(BINDIR)/test_sharding $(BINDIR)/test_store $(BINDIR)/test_protocol

$(BINDIR) $(BUILDDIR):
	mkdir -p $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BINDIR)/kv-server: $(CORE_OBJECTS) $(BUILDDIR)/server.o | $(BINDIR)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(BINDIR)/kv-shard: $(CORE_OBJECTS) $(BUILDDIR)/shard_main.o | $(BINDIR)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(BINDIR)/kv-proxy: $(CORE_OBJECTS) $(BUILDDIR)/proxy.o | $(BINDIR)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(BINDIR)/kv-client: $(CORE_OBJECTS) $(BUILDDIR)/client.o | $(BINDIR)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(BINDIR)/test_avl: tests/test_avl.cpp $(CORE_OBJECTS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -UNDEBUG $^ -o $@ $(LDFLAGS)

$(BINDIR)/test_heap: tests/test_heap.cpp $(CORE_OBJECTS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -UNDEBUG $^ -o $@ $(LDFLAGS)

$(BINDIR)/test_sharding: tests/test_sharding.cpp $(CORE_OBJECTS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -UNDEBUG $^ -o $@ $(LDFLAGS)

test: $(BINDIR)/test_avl $(BINDIR)/test_heap $(BINDIR)/test_sharding $(BINDIR)/test_store $(BINDIR)/test_protocol
	$(BINDIR)/test_avl
	$(BINDIR)/test_heap
	$(BINDIR)/test_sharding
	$(BINDIR)/test_store
	$(BINDIR)/test_protocol

clean:
	rm -rf $(BUILDDIR) $(BINDIR)

# Optional FLTK UI
FLTK_CXXFLAGS ?= $(shell fltk-config --cxxflags 2>/dev/null)
FLTK_LDFLAGS := $(shell fltk-config --ldflags 2>/dev/null)

ifneq ($(FLTK_CXXFLAGS),)
all: $(BINDIR)/kv-ui

$(BUILDDIR)/ui.o: $(SRCDIR)/ui.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(FLTK_CXXFLAGS) -MMD -MP -c $< -o $@

$(BINDIR)/kv-ui: $(BUILDDIR)/ui.o $(CORE_OBJECTS) | $(BINDIR)
	$(CXX) $^ -o $@ $(FLTK_LDFLAGS) $(LDFLAGS)
endif

$(BINDIR)/test_store: tests/test_store.cpp $(CORE_OBJECTS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -UNDEBUG $^ -o $@ $(LDFLAGS)

$(BINDIR)/test_protocol: tests/test_protocol.cpp $(CORE_OBJECTS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -UNDEBUG $^ -o $@ $(LDFLAGS)

integration: all
	python3 tests/integration.py --bin-dir $(BINDIR)

-include $(wildcard $(BUILDDIR)/*.d)
