{
  description = "nintysh - a Windows 98 flavoured Wayland shell for niri, Sway and Hyprland";

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
              brightnessctl
              grim
              slurp
              networkmanager
              bluez
              pam
              wayland
              gdk-pixbuf
              cava
            ];
          };

          formatter = pkgs.nixfmt;
        };

      flake = let
        # Hydra builds this jobset on x86_64-linux. Keeping the variants here
        # means the jobset is flat and does not contain aarch64 jobs.
        hydraPkgs = inputs.nixpkgs.legacyPackages.x86_64-linux;
        nintysh = hydraPkgs.callPackage ./nix/package.nix { };
        mkNintysh = args: hydraPkgs.callPackage ./nix/package.nix args;
      in {
        hydraJobs = {
          # The unmodified package.
          nintysh = nintysh;

          # A configuration using the same settings the Home Manager module
          # would generate, plus a wrapper argument.
          nintysh-custom = mkNintysh {
            extraConfig = ''
              #define NINTYSH_FONT_FAMILY "MS Sans Serif"
              #define NINTYSH_FONT_PX 12
              #define NINTYSH_BAR_HEIGHT 30
              #define NINTYSH_ICON_SIZE 24
              #define NINTYSH_CLIPBOARD_MAX_ENTRIES 50
              #define NINTYSH_NOTIFICATION_POPUP_WIDTH 380
              #define NINTYSH_COLOR_FACE 0xC0C0C0
              #define NINTYSH_COLOR_NAVY 0x000080
              #define NINTYSH_SCREENSHOT_REGION_COMMAND "grim -g \"$(slurp)\" - | wl-copy"
            '';
            wrapperArgs = [ "--set" "NINTYSH_COMPOSITOR" "niri" ];
          };

          # Scaled-up build for HiDPI displays.
          nintysh-highdpi = mkNintysh {
            extraConfig = ''
              #define NINTYSH_FONT_PX 16
              #define NINTYSH_BAR_HEIGHT 42
              #define NINTYSH_BAR_SCALE 1.5
              #define NINTYSH_PANEL_SCALE 1.5
              #define NINTYSH_BUTTON_Y 4.5
              #define NINTYSH_BUTTON_HEIGHT 33.0
              #define NINTYSH_ICON_SIZE 30
              #define NINTYSH_ICON_LOOKUP_SIZE 64
              #define NINTYSH_MENU_WIDTH 378.0
              #define NINTYSH_MENU_SIDEBAR 33.0
              #define NINTYSH_MENU_ROW_HEIGHT 33.0
              #define NINTYSH_MENU_SEARCH_HEIGHT 33.0
              #define NINTYSH_MENU_MAX_HEIGHT 630.0
              #define NINTYSH_OSD_WIDTH 330
              #define NINTYSH_OSD_HEIGHT 87
              #define NINTYSH_CLIPBOARD_ROW_HEIGHT 51.0
              #define NINTYSH_NOTIFICATION_ROW_HEIGHT 51.0
            '';
          };

          # A compact configuration with the bar on top.
          nintysh-compact = mkNintysh {
            extraConfig = ''
              #define NINTYSH_FONT_PX 10
              #define NINTYSH_BAR_HEIGHT 24
              #define NINTYSH_BAR_POSITION 1
              #define NINTYSH_BUTTON_Y 2.0
              #define NINTYSH_BUTTON_HEIGHT 20.0
              #define NINTYSH_ICON_SIZE 16
              #define NINTYSH_MENU_WIDTH 224.0
              #define NINTYSH_MENU_ROW_HEIGHT 20.0
              #define NINTYSH_MENU_SEARCH_HEIGHT 20.0
              #define NINTYSH_TASKBAR_APPS 0
            '';
          };
        };

        lib.mkNintysh = pkgs: { extraConfig ? "", wrapperArgs ? [ ] }:
          pkgs.callPackage ./nix/package.nix {
            inherit extraConfig wrapperArgs;
          };

        overlays.default = final: prev: {
          nintysh = final.callPackage ./nix/package.nix { };
        };

        modules.homeManager = {
          default = ./nix/hm.nix;
          nintysh = ./nix/hm.nix;
        };

        homeModules.default = ./nix/hm.nix;
        homeManagerModules.default = ./nix/hm.nix;
      };
    };
}
