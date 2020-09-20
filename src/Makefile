CXX = g++
CXXFLAGS = -g -Wall -finstrument-functions -rdynamic -std=c++17

IDIR = ../include
ODIR = ../build
LIB_DIR = ../lib
INCS = -I$(IDIR) -L$(LIB_DIR)
LIBS = -lpthread -ldl

SOURCES := $(shell find . -name "*.cpp" -printf '%f\n')
_HEADERS  := $(shell find $(IDIR) -name "*.h" -printf '%f\n')

HEADERS = $(patsubst %,$(IDIR)/%,$(_HEADERS))

_OBJS = $(SOURCES:.cpp=.o)
OBJS = $(patsubst %,$(ODIR)/%,$(_OBJS))

EXECUTABLE = $(ODIR)/runDemo

.PHONY: all
all: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(ODIR)/%.o : %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@ $(INCS)

run:
	$(EXECUTABLE)

.PHONY: clean
clean:
	rm -f $(EXECUTABLE) $(OBJS)

cleanAll:
	rm -f $(ODIR)/*
