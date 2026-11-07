{
  lib,
  stdenv,
  just,
  pkg-config,
  wrapGAppsHook4,
  gtk4,
  gtk4-layer-shell,
  json-glib,
  wl-clipboard,
  brightnessctl,
  grim,
  slurp,
  networkmanager,
  pam,
  extraConfig ? "",
  wrapperArgs ? [ ],
}:

stdenv.mkDerivation {
  pname = "nintysh";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = lib.cleanSource ../.;
    filter = path: type:
      let name = baseNameOf path;
      in !(builtins.elem name [ "build" "docs" ".direnv" ]);
  };

  nativeBuildInputs = [
    just
    pkg-config
    wrapGAppsHook4
  ];

  buildInputs = [
    gtk4
    gtk4-layer-shell
    json-glib
    pam
  ];

  postPatch = lib.optionalString (extraConfig != "") ''
    cat > src/config.user.h <<'NINTYSH_CONFIG_EOF'
    ${extraConfig}
    NINTYSH_CONFIG_EOF
  '';

  gappsWrapperArgs = [
    "--prefix" "PATH" ":"
    (lib.makeBinPath [ wl-clipboard brightnessctl grim slurp networkmanager ])
  ] ++ wrapperArgs;

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
    platforms = lib.platforms.linux;
    mainProgram = "nintysh";
  };
}
