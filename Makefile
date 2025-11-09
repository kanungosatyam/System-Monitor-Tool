CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra
SRC = src/main.cpp
TARGET = monitor

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)
