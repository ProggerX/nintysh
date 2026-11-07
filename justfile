build_dir := "build"
bin := "nintysh"
bin_path := build_dir + "/" + bin
cc := env_var_or_default("CC", "cc")
pkg_deps := "gtk4 gtk4-layer-shell-0 json-glib-1.0 gdk-pixbuf-2.0 wayland-client"
pam_cflags := env_var_or_default("NINTYSH_PAM_CFLAGS", "")
pam_libs := env_var_or_default("NINTYSH_PAM_LIBS", "-lpam")
sources := "src/main.c src/win98.c src/niri.c src/mpris.c src/auth.c src/gamma.c src/gamma-protocol.c src/wallpaper.c src/bar.c src/control.c"

default: build

build:
    @mkdir -p {{build_dir}}
    @if [ ! -e {{bin_path}} ] || [ -n "$(find src justfile -newer {{bin_path}} -print -quit 2>/dev/null)" ]; then \
        {{cc}} -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra {{pam_cflags}} $(pkg-config --cflags {{pkg_deps}}) {{sources}} -o {{bin_path}} $(pkg-config --libs {{pkg_deps}}) {{pam_libs}} -lm; \
    fi

run: build
    exec ./{{bin_path}}

check: build

install prefix="/usr/local": build
    install -Dm755 {{bin_path}} "{{prefix}}/bin/{{bin}}"

_install prefix:
    install -Dm755 {{bin_path}} "{{prefix}}/bin/{{bin}}"

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
