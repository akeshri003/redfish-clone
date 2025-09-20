# Redis Clone Makefile

CXX = clang++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
TARGET = redis-clone
SOURCES = main.cpp resp_parser.cpp dispatcher.cpp
OBJECTS = $(SOURCES:.cpp=.o)

# Default target
all: $(TARGET)

# Build the main executable
$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS)

# Compile source files to object files
%.o: %.cpp resp_types.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(TARGET)

# Run the server (default port 6380)
run: $(TARGET)
	./$(TARGET)

# Run with custom port
run-port: $(TARGET)
	./$(TARGET) 6379

# Install dependencies (if needed)
install:
	@echo "No external dependencies required"

# Debug build
debug: CXXFLAGS += -g -DDEBUG
debug: $(TARGET)

# Release build
release: CXXFLAGS += -DNDEBUG
release: $(TARGET)

.PHONY: all clean run run-port install debug release
