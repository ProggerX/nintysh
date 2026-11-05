{ config, lib, pkgs, ... }:

let
  cfg = config.programs.nintysh;

  colorType = lib.types.strMatching "#?[0-9a-fA-F]{6}";
  color = value: "0x" + lib.removePrefix "#" value;
  mkColor = default: lib.mkOption { type = colorType; inherit default; };

  renderConfig = settings:
    lib.concatStringsSep "\n" ([
      "#define NINTYSH_FONT_FAMILY ${builtins.toJSON settings.font.family}"
      "#define NINTYSH_FONT_PX ${toString settings.font.size}"
      "#define NINTYSH_BAR_HEIGHT ${toString settings.bar.height}"
      "#define NINTYSH_BUTTON_Y ${toString settings.bar.buttonY}"
      "#define NINTYSH_BUTTON_HEIGHT ${toString settings.bar.buttonHeight}"
      "#define NINTYSH_MENU_WIDTH ${toString settings.launcher.width}"
      "#define NINTYSH_MENU_SIDEBAR ${toString settings.launcher.sidebarWidth}"
      "#define NINTYSH_MENU_ROW_HEIGHT ${toString settings.launcher.rowHeight}"
      "#define NINTYSH_MENU_SEARCH_HEIGHT ${toString settings.launcher.searchHeight}"
      "#define NINTYSH_MENU_MAX_HEIGHT ${toString settings.launcher.maxHeight}"
      "#define NINTYSH_ICON_SIZE ${toString settings.icons.size}"
      "#define NINTYSH_ICON_LOOKUP_SIZE ${toString settings.icons.lookupSize}"
      "#define NINTYSH_ICON_LOOKUP_SCALE ${toString settings.icons.lookupScale}"
      "#define NINTYSH_OSD_WIDTH ${toString settings.osd.width}"
      "#define NINTYSH_OSD_HEIGHT ${toString settings.osd.height}"
      "#define NINTYSH_OSD_TIMEOUT_MS ${toString settings.osd.timeoutMs}"
      "#define NINTYSH_CLIPBOARD_MAX_ENTRIES ${toString settings.clipboard.maxEntries}"
      "#define NINTYSH_CLIPBOARD_MAX_BYTES ${toString settings.clipboard.maxBytes}"
      "#define NINTYSH_CLIPBOARD_POLL_SECONDS ${toString settings.clipboard.pollSeconds}"
      "#define NINTYSH_CLIPBOARD_ROW_HEIGHT ${toString settings.clipboard.rowHeight}"
      "#define NINTYSH_CLOCK_TICK_SECONDS ${toString settings.updates.clockSeconds}"
      "#define NINTYSH_SYSTEM_POLL_SECONDS ${toString settings.updates.systemSeconds}"
      "#define NINTYSH_COLOR_FACE ${color settings.colors.face}"
      "#define NINTYSH_COLOR_HIGHLIGHT ${color settings.colors.highlight}"
      "#define NINTYSH_COLOR_LIGHT ${color settings.colors.light}"
      "#define NINTYSH_COLOR_SHADOW ${color settings.colors.shadow}"
      "#define NINTYSH_COLOR_DARK ${color settings.colors.dark}"
      "#define NINTYSH_COLOR_TEXT ${color settings.colors.text}"
      "#define NINTYSH_COLOR_TEXT_DISABLED ${color settings.colors.textDisabled}"
      "#define NINTYSH_COLOR_NAVY ${color settings.colors.selection}"
      "#define NINTYSH_COLOR_WHITE ${color settings.colors.white}"
      "#define NINTYSH_COLOR_BLACK ${color settings.colors.black}"
      "#define NINTYSH_COLOR_TEAL ${color settings.colors.teal}"
      "#define NINTYSH_COLOR_TITLE_TOP ${color settings.colors.titleTop}"
      "#define NINTYSH_COLOR_TITLE_BOTTOM ${color settings.colors.titleBottom}"
    ] ++ lib.optional (settings.osd.marginBottom != null)
      "#define NINTYSH_OSD_MARGIN_BOTTOM ${toString settings.osd.marginBottom}"
      ++ lib.optional (settings.extraConfig != "") settings.extraConfig);
in
{
  options.programs.nintysh = {
    enable = lib.mkEnableOption "nintysh Wayland shell";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package.nix {
        extraConfig = renderConfig cfg.settings;
        inherit (cfg) wrapperArgs;
      };
      defaultText = lib.literalExpression "nintysh configured by programs.nintysh.settings";
      description = "The nintysh package to install.";
    };

    autostart = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Start nintysh with the graphical session.";
    };

    wrapperArgs = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ ];
      description = "Extra arguments passed to the Nix binary wrapper.";
    };

    settings = lib.mkOption {
      type = lib.types.submodule {
        options = {
          extraConfig = lib.mkOption {
            type = lib.types.lines;
            default = "";
            description = "Extra preprocessor definitions written after the generated config.";
          };

          font = {
            family = lib.mkOption {
              type = lib.types.str;
              default = "Tahoma";
              description = "UI font family.";
            };
            size = lib.mkOption {
              type = lib.types.ints.positive;
              default = 11;
              description = "UI font size in pixels.";
            };
          };

          bar = {
            height = lib.mkOption {
              type = lib.types.ints.positive;
              default = 28;
              description = "Taskbar height in pixels.";
            };
            buttonY = lib.mkOption {
              type = lib.types.number;
              default = 3.0;
              description = "Vertical button offset inside the taskbar.";
            };
            buttonHeight = lib.mkOption {
              type = lib.types.number;
              default = 22.0;
              description = "Taskbar button height.";
            };
          };

          launcher = {
            width = lib.mkOption {
              type = lib.types.number;
              default = 252.0;
              description = "Launcher width.";
            };
            sidebarWidth = lib.mkOption {
              type = lib.types.number;
              default = 22.0;
              description = "Launcher sidebar width.";
            };
            rowHeight = lib.mkOption {
              type = lib.types.number;
              default = 22.0;
              description = "Launcher result row height.";
            };
            searchHeight = lib.mkOption {
              type = lib.types.number;
              default = 22.0;
              description = "Launcher search field height.";
            };
            maxHeight = lib.mkOption {
              type = lib.types.number;
              default = 420.0;
              description = "Maximum launcher height.";
            };
          };

          icons = {
            size = lib.mkOption {
              type = lib.types.ints.positive;
              default = 20;
              description = "Rendered icon size.";
            };
            lookupSize = lib.mkOption {
              type = lib.types.ints.positive;
              default = 48;
              description = "Icon theme lookup size.";
            };
            lookupScale = lib.mkOption {
              type = lib.types.ints.positive;
              default = 2;
              description = "Icon theme lookup scale.";
            };
          };

          osd = {
            width = lib.mkOption {
              type = lib.types.ints.positive;
              default = 220;
              description = "OSD width.";
            };
            height = lib.mkOption {
              type = lib.types.ints.positive;
              default = 58;
              description = "OSD height.";
            };
            timeoutMs = lib.mkOption {
              type = lib.types.ints.unsigned;
              default = 1400;
              description = "OSD timeout in milliseconds.";
            };
            marginBottom = lib.mkOption {
              type = lib.types.nullOr lib.types.ints.unsigned;
              default = null;
              description = "OSD bottom margin. Null derives it from bar.height.";
            };
          };

          clipboard = {
            maxEntries = lib.mkOption {
              type = lib.types.ints.positive;
              default = 30;
              description = "Maximum clipboard history entries.";
            };
            maxBytes = lib.mkOption {
              type = lib.types.ints.positive;
              default = 65536;
              description = "Maximum bytes stored per clipboard entry.";
            };
            pollSeconds = lib.mkOption {
              type = lib.types.ints.positive;
              default = 2;
              description = "Clipboard polling interval.";
            };
            rowHeight = lib.mkOption {
              type = lib.types.number;
              default = 20.0;
              description = "Clipboard row height.";
            };
          };

          updates = {
            clockSeconds = lib.mkOption {
              type = lib.types.ints.positive;
              default = 1;
              description = "Clock redraw interval.";
            };
            systemSeconds = lib.mkOption {
              type = lib.types.ints.positive;
              default = 5;
              description = "Volume, battery and network polling interval.";
            };
          };

          colors = {
            face = mkColor "#c0c0c0";
            highlight = mkColor "#ffffff";
            light = mkColor "#dfdfdf";
            shadow = mkColor "#808080";
            dark = mkColor "#000000";
            text = mkColor "#000000";
            textDisabled = mkColor "#808080";
            selection = mkColor "#000080";
            white = mkColor "#ffffff";
            black = mkColor "#000000";
            teal = mkColor "#008080";
            titleTop = mkColor "#000080";
            titleBottom = mkColor "#1084d0";
          };
        };
      };
      default = { };
      description = "Nintysh appearance and behaviour settings.";
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
