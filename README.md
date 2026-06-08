# Zenk — a minimal code editor

A vim-like code editor built from scratch in C with SDL3.
Its made to code in C and C++.

and am tired of these editors, the big support of AI from every new editor
with the hard process of configuring neovim and emacs

## Features

- Vim-style modes: NORMAL, INSERT, COMMAND, VISUAL
- Modal cursor shapes: block (NORMAL), green bar blink (INSERT)
- `h/j/k/l` movement, `x` delete, `dd` delete line
- `i` insert, `o` open line, `:` commands (`:w`, `:q`, `:wq`)
- Arrow keys, Home/End, PageUp/PageDown
- Line numbers, status bar with mode/position
- File I/O via command line argument

## Build

```bash
make
```

## Usage

```bash
make run
```

## Dependencies
- SDL3
- SDL3_ttf
- gcc, make

## Todos:
- [ ] Add Visual Mode with highlighting
- [ ] Add more Vim keymaps
- [ ] Add relative line number
- [ ] Integrate LSP (clangd) for the editor with a formater
- [ ] Have the usableity like tmux and
- [ ] Make Worknig With multi buffers better
- [ ] Add Settings And Themes
