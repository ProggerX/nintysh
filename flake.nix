{
  description = "nintysh - a Windows 98 flavoured Wayland shell for niri";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    home-manager = {
      url = "github:nix-community/home-manager";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      imports = [
        inputs.flake-parts.flakeModules.modules
      ];

      systems = [ "x86_64-linux" "aarch64-linux" ];

      perSystem = { pkgs, ... }:
        let
          nintysh = pkgs.callPackage ./nix/package.nix { };
        in {
          packages.default = nintysh;
          packages.nintysh = nintysh;

          devShells.default = pkgs.mkShell {
            packages = with pkgs; [
              just
              pkg-config
              gtk4
              gtk4-layer-shell
              json-glib
              gdb
              niri
              wl-clipboard
            ];
          };

          formatter = pkgs.nixfmt-rfc-style;
        };

      flake = {
        lib.mkNintysh = pkgs: { extraConfig ? "", wrapperArgs ? [ ] }:
          pkgs.callPackage ./nix/package.nix {
            inherit extraConfig wrapperArgs;
          };

        modules.homeManager = {
          default = ./nix/hm.nix;
          nintysh = ./nix/hm.nix;
        };
      };
    };
}
