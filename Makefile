# Toxic City recomp - build skeleton.
# Usage:  make JAR=path/to/Spider-Man_-_Toxic_City_240x320_....jar
JAR      ?= game.jar
MIDLET   ?= GloftSPDN
CC       ?= gcc
SDL_SYS_LIBS =
ifeq ($(OS),Windows_NT)
SDL_CFLAGS ?= -I/ucrt64/include
SDL_LIBS ?= -lmingw32 -lSDL2main -lSDL2
SDL_SYS_LIBS += -lwinmm
else
SDL_CFLAGS ?= $(shell sdl2-config --cflags)
SDL_LIBS ?= $(shell sdl2-config --libs)
endif
CFLAGS   ?= -std=gnu11 -O2 -g -Iinclude -Igen -Iruntime -Wall -Wno-unused-label -Wno-unused-variable \
            -Wno-unused-but-set-variable -Wno-clobbered
GEN_SRC   = $(wildcard gen/*.c)
RT_SRC    = $(filter-out runtime/main_headless.c runtime/main_sdl.c,$(wildcard runtime/*.c))
OBJ       = $(GEN_SRC:.c=.o) $(RT_SRC:.c=.o)

.PHONY: gen check missing res headless sdl test clean
gen:
	python3 tools/j2c.py --jar $(JAR) --out gen --midlet $(MIDLET)

# Compile the generated C only (no runtime needed) - proves the translation is valid C.
check: gen
	@for f in gen/*.c; do $(CC) $(CFLAGS) -c $$f -o /dev/null || exit 1; done; echo "generated C compiles"

# Lists runtime symbols the game needs that runtime/ does not define yet.
missing: gen
	@mkdir -p build && for f in gen/*.c; do $(CC) $(CFLAGS) -c $$f -o build/$$(basename $$f).o; done
	@for f in runtime/*.c; do [ -e $$f ] && $(CC) $(CFLAGS) -c $$f -o build/rt_$$(basename $$f).o; done; true
	@nm -u build/*.o | awk '/ U /{print $$2}' | sort -u > build/undef.txt
	@nm --defined-only build/*.o | awk '{print $$3}' | sort -u > build/def.txt
	@comm -23 build/undef.txt build/def.txt | grep -E '^(rt_|jvm_|jgame_)' || echo "nothing missing"

# Resources: extract the JAR so getResourceAsStream can read them.
res:
	python3 -c "import zipfile,sys; zipfile.ZipFile('$(JAR)').extractall('res')"

headless: gen res
	$(CC) $(CFLAGS) -pthread $(GEN_SRC) $(RT_SRC) runtime/main_headless.c -lm -o toxiccity_headless
	mkdir -p shots && ./toxiccity_headless --res res --save save --seconds 6 --shot-dir shots --shot-ms 1000

# Playable build (Linux: libsdl2-dev; Windows: MSYS2 UCRT64 GCC + SDL2):
#   make sdl JAR=... && ./toxiccity --res res --save save --scale 3
sdl: gen res
	$(CC) $(CFLAGS) -pthread $(SDL_CFLAGS) $(GEN_SRC) $(RT_SRC) runtime/main_sdl.c -lm $(SDL_LIBS) $(SDL_SYS_LIBS) -o toxiccity

test:
	python3 tests/synth_test.py

clean:
	rm -rf gen/* build res save shots toxiccity toxiccity_headless
