default:
	mkdir -p build
	gcc -Wall -Wextra main.c buffer.c user.c -o build/main -lSDL3 -lSDL3_ttf -lm

run:
	./build/main
