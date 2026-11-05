{
  description = "nintysh - a Windows 98 flavoured Wayland shell for niri";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          nintysh = pkgs.stdenv.mkDerivation {
            pname = "nintysh";
            version = "0.1.0";

            src = pkgs.lib.cleanSourceWith {
              src = pkgs.lib.cleanSource ./.;
              filter = path: type:
                let name = baseNameOf path;
                in !(builtins.elem name [ "build" ".direnv" ]);
            };

            nativeBuildInputs = with pkgs; [
              just
              pkg-config
              wrapGAppsHook4
            ];

            buildInputs = with pkgs; [
              gtk4
              gtk4-layer-shell
              json-glib
            ];

            gappsWrapperArgs = [
              "--prefix" "PATH" ":" (pkgs.lib.makeBinPath [ pkgs.wl-clipboard ])
            ];

            buildPhase = ''
              runHook preBuild
              just build
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              just _install "$out"
              runHook postInstall
            '';

            meta = {
              description = "A Windows 98 style Wayland shell for niri";
              platforms = pkgs.lib.platforms.linux;
              mainProgram = "nintysh";
            };
          };
        in {
          default = nintysh;
          nintysh = nintysh;
        });

      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in {
          default = pkgs.mkShell {
            packages = with pkgs; [
              just
              pkg-config
              gtk4
              gtk4-layer-shell
              json-glib
              gdb
              niri
            ];
          };
        });

      formatter = forAllSystems (system:
        nixpkgs.legacyPackages.${system}.nixfmt-rfc-style);
    };
}
