CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
LDLIBS   := -lcrypto -pthread

COMMON_OBJ  := common/proto.o common/sha1.o
TRACKER_OBJ := tracker/tracker.o tracker/state.o tracker/replica.o
CLIENT_OBJ  := client/client.o client/session.o client/share.o client/download.o

all: tracker/tracker client/client

tracker/tracker: $(TRACKER_OBJ) $(COMMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

client/client: $(CLIENT_OBJ) $(COMMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(TRACKER_OBJ:.o=.d) $(CLIENT_OBJ:.o=.d) $(COMMON_OBJ:.o=.d)

clean:
	rm -f $(TRACKER_OBJ) $(CLIENT_OBJ) $(COMMON_OBJ)
	rm -f $(TRACKER_OBJ:.o=.d) $(CLIENT_OBJ:.o=.d) $(COMMON_OBJ:.o=.d)
	rm -f tracker/tracker client/client
	rm -rf tracker_data_*

.PHONY: all clean
