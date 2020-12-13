# Find C++ Standard Library header files to exclude them from instrumentation
CPP_STD_INCLUDES=$(shell ( 	`gcc -print-prog-name=cc1plus` -v < /dev/null 2>&1 \
								| LC_ALL=C sed -ne '/starts here/,/End of/p' \
								| grep -o '/[^"]*' ; \
							`gcc -print-prog-name=cpp` -v < /dev/null 2>&1 \
								| LC_ALL=C sed -ne '/starts here/,/End of/p' \
								| grep -o '/[^"]*' ) \
						| cat | sort | uniq | tr '\n' ',' | sed 's/\(.*\),/\1 /' | xargs )

# Exclude tracing functions themselves from instrumentation
CPP_STD_INCLUDES := "${CPP_STD_INCLUDES},include/callStack.h,include/unwinder.h"
$(info $$CPP_STD_INCLUDES is [${CPP_STD_INCLUDES}])

CXX = g++
CXXFLAGS = -g -Wall -rdynamic -std=c++17 -finstrument-functions -finstrument-functions-exclude-file-list=$(CPP_STD_INCLUDES)

IDIR = ../include
ODIR = ../build
LIB_DIR = ../lib
INCS = -I$(IDIR) -L$(LIB_DIR)
LIBS = -lpthread -ldl -lbfd

SOURCES := $(shell find . -name "*.cpp" -printf '%f\n')
_HEADERS  := $(shell find $(IDIR) -name "*.h" -printf '%f\n')

HEADERS = $(patsubst %,$(IDIR)/%,$(_HEADERS))

_OBJS = $(SOURCES:.cpp=.o)
OBJS = $(patsubst %,$(ODIR)/%,$(_OBJS))

EXECUTABLE = $(ODIR)/runDemo

.PHONY: build
build: clean $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(ODIR)/%.o : %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@ $(INCS)

log_with_addr: CXXFLAGS += -DLOG_ADDR
log_with_addr: clean build

log_not_demangled: CXXFLAGS += -DLOG_NOT_DEMANGLED
log_not_demangled: clean build

run:
	$(EXECUTABLE)

.PHONY: clean
clean:
	rm -f $(EXECUTABLE) $(OBJS)

cleanAll:
	rm -f $(ODIR)/*
