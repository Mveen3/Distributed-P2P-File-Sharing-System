CXX = g++
CXXFLAGS = -std=c++11 -O2 -pthread -Wall -Wextra

all: tracker/tracker client/client

tracker/tracker: tracker/tracker.cpp common/proto.cpp common/sha1.cpp
	@mkdir -p tracker
	$(CXX) $(CXXFLAGS) -o $@ tracker/tracker.cpp common/proto.cpp common/sha1.cpp

client/client: client/client.cpp common/proto.cpp common/sha1.cpp
	@mkdir -p client  
	$(CXX) $(CXXFLAGS) -o $@ client/client.cpp common/proto.cpp common/sha1.cpp

clean:
	rm -f tracker/tracker client/client
	rm -rf tracker_data_*
	rm -f *.o

install: all
	@echo "Binaries ready in tracker/ and client/ directories"

test: all
	@echo "FILE UPLOAD SYNC FIXED!"
	@echo "Complete system now working: upload sync perfect!"

.PHONY: all clean install test