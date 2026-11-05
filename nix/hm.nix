{ config, lib, pkgs, ... }:

let
  cfg = config.programs.nintysh;
in
{
  options.programs.nintysh = {
    enable = lib.mkEnableOption "nintysh Wayland shell";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ../nix/package.nix {
        inherit (cfg) extraConfig wrapperArgs;
      };
      defaultText = lib.literalExpression "nintysh configured by programs.nintysh";
      description = "The nintysh package to install.";
    };

    extraConfig = lib.mkOption {
      type = lib.types.lines;
      default = "";
      description = "Extra preprocessor definitions appended to config.user.h.";
    };

    wrapperArgs = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ ];
      description = "Extra arguments passed to the Nix binary wrapper.";
    };

    autostart = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Start nintysh with the graphical session.";
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package ];

    systemd.user.services.nintysh = lib.mkIf cfg.autostart {
      Unit = {
        Description = "nintysh Wayland shell";
        After = [ "graphical-session.target" ];
        PartOf = [ "graphical-session.target" ];
      };
      Service = {
        ExecStart = "${cfg.package}/bin/nintysh";
        Restart = "on-failure";
        RestartSec = 2;
      };
      Install.WantedBy = [ "graphical-session.target" ];
    };
  };
}
