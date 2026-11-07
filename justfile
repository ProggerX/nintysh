build_dir := "build"
bin := "nintysh"
cc := env_var_or_default("CC", "cc")
pkg_deps := "gtk4 gtk4-layer-shell-0 json-glib-1.0"

default: build

build:
    mkdir -p {{build_dir}}
    {{cc}} -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra $(pkg-config --cflags {{pkg_deps}}) src/main.c src/win98.c src/niri.c src/mpris.c src/bar.c src/control.c -o {{build_dir}}/{{bin}} $(pkg-config --libs {{pkg_deps}}) -lm

run: build
    exec ./{{build_dir}}/{{bin}}

check: build

install prefix="/usr/local": build
    install -Dm755 {{build_dir}}/{{bin}} "{{prefix}}/bin/{{bin}}"

_install prefix:
    install -Dm755 {{build_dir}}/{{bin}} "{{prefix}}/bin/{{bin}}"

clean:
    rm -rf {{build_dir}}

nix-build:
    nix build .#

nix-run:
    nix run .#

dev:
    nix develop

fmt:
    nix fmt
