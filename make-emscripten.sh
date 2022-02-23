#!/bin/bash

rm release/other/pt2-clone &> /dev/null

echo Compiling, please wait...
emcc -s USE_SDL=2 \
	-s USE_PTHREADS=1 \
	-s ALLOW_MEMORY_GROWTH=1 \
	-s EXPORTED_RUNTIME_METHODS=[ccall,FS] \
	-pthread \
	--embed-file ./release/other/protracker.ini@protracker.ini \
	--embed-file ./mods@./mods \
	--shell-file ./src/pt2_shell.html \
	-o release/emscripten/pt2-clone.html \
	-DNDEBUG src/gfx/*.c src/*.c -lm -Wall -Wno-unused-result -Wc++-compat -Wshadow -Winit-self -Wextra -Wunused -Wunreachable-code -Wredundant-decls -Wswitch-default -march=native -mtune=native -O3
rm src/gfx/*.o src/*.o &> /dev/null

echo Done. The generated output can be found in \'release/emscripten\' if everything went well. Use \'python3 wasm-server.py\' to test locally.
