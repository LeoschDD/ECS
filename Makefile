PROJECTNAME = ECS
OUTPUT_DIR = build

INCLUDE_DIRS = -Iinclude
LIB_DIRS = -Llib

LIBS = -lmingw32

OPTIMISATION = -O3
VERSION = -std=c++23

SRC = $(wildcard src/*.cpp)

default:
	g++ $(VERSION) $(SRC) -o $(OUTPUT_DIR)/$(PROJECTNAME) $(INCLUDE_DIRS) $(OPTIMISATION) $(LIB_DIRS) $(LIBS) 