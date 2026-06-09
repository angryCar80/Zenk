default:
	mkdir -p build
	gcc -Wall -Wextra main.c json.c buffer.c user.c lsp.c -o build/main -lSDL3 -lSDL3_ttf -lm -lpthread

run:
	./build/main
