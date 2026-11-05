{ config, lib, pkgs, ... }:

let
  cfg = config.programs.nintysh;

  colorType = lib.types.strMatching "#?[0-9a-fA-F]{6}";
  color = value: "0x" + lib.removePrefix "#" value;

  renderConfig = cfg:
    lib.concatStringsSep "\n" ([
      "#define NINTYSH_FONT_FAMILY ${builtins.toJSON cfg.font.family}"
      "#define NINTYSH_FONT_PX ${toString cfg.font.size}"
      "#define NINTYSH_BAR_HEIGHT ${toString cfg.bar.height}"
      "#define NINTYSH_BUTTON_Y ${toString cfg.bar.buttonY}"
      "#define NINTYSH_BUTTON_HEIGHT ${toString cfg.bar.buttonHeight}"
      "#define NINTYSH_MENU_WIDTH ${toString cfg.launcher.width}"
      "#define NINTYSH_MENU_SIDEBAR ${toString cfg.launcher.sidebarWidth}"
      "#define NINTYSH_MENU_ROW_HEIGHT ${toString cfg.launcher.rowHeight}"
      "#define NINTYSH_MENU_SEARCH_HEIGHT ${toString cfg.launcher.searchHeight}"
      "#define NINTYSH_MENU_MAX_HEIGHT ${toString cfg.launcher.maxHeight}"
      "#define NINTYSH_ICON_SIZE ${toString cfg.icons.size}"
      "#define NINTYSH_ICON_LOOKUP_SIZE ${toString cfg.icons.lookupSize}"
      "#define NINTYSH_ICON_LOOKUP_SCALE ${toString cfg.icons.lookupScale}"
      "#define NINTYSH_OSD_WIDTH ${toString cfg.osd.width}"
      "#define NINTYSH_OSD_HEIGHT ${toString cfg.osd.height}"
      "#define NINTYSH_OSD_TIMEOUT_MS ${toString cfg.osd.timeoutMs}"
      "#define NINTYSH_CLIPBOARD_MAX_ENTRIES ${toString cfg.clipboard.maxEntries}"
      "#define NINTYSH_CLIPBOARD_MAX_BYTES ${toString cfg.clipboard.maxBytes}"
      "#define NINTYSH_CLIPBOARD_POLL_SECONDS ${toString cfg.clipboard.pollSeconds}"
      "#define NINTYSH_CLIPBOARD_ROW_HEIGHT ${toString cfg.clipboard.rowHeight}"
      "#define NINTYSH_CLOCK_TICK_SECONDS ${toString cfg.updates.clockSeconds}"
      "#define NINTYSH_SYSTEM_POLL_SECONDS ${toString cfg.updates.systemSeconds}"
      "#define NINTYSH_COLOR_FACE ${color cfg.colors.face}"
      "#define NINTYSH_COLOR_HIGHLIGHT ${color cfg.colors.highlight}"
      "#define NINTYSH_COLOR_LIGHT ${color cfg.colors.light}"
      "#define NINTYSH_COLOR_SHADOW ${color cfg.colors.shadow}"
      "#define NINTYSH_COLOR_DARK ${color cfg.colors.dark}"
      "#define NINTYSH_COLOR_TEXT ${color cfg.colors.text}"
      "#define NINTYSH_COLOR_TEXT_DISABLED ${color cfg.colors.textDisabled}"
      "#define NINTYSH_COLOR_NAVY ${color cfg.colors.selection}"
      "#define NINTYSH_COLOR_WHITE ${color cfg.colors.white}"
      "#define NINTYSH_COLOR_BLACK ${color cfg.colors.black}"
      "#define NINTYSH_COLOR_TEAL ${color cfg.colors.teal}"
      "#define NINTYSH_COLOR_TITLE_TOP ${color cfg.colors.titleTop}"
      "#define NINTYSH_COLOR_TITLE_BOTTOM ${color cfg.colors.titleBottom}"
    ] ++ lib.optional (cfg.osd.marginBottom != null)
      "#define NINTYSH_OSD_MARGIN_BOTTOM ${toString cfg.osd.marginBottom}"
      ++ lib.optional (cfg.extraConfig != "") cfg.extraConfig);
in
{
  options.programs.nintysh = {
    enable = lib.mkEnableOption "nintysh Wayland shell";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package.nix {
        extraConfig = renderConfig cfg;
        inherit (cfg) wrapperArgs;
      };
      defaultText = lib.literalExpression "nintysh configured by programs.nintysh";
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
      face = lib.mkOption { type = colorType; default = "#c0c0c0"; };
      highlight = lib.mkOption { type = colorType; default = "#ffffff"; };
      light = lib.mkOption { type = colorType; default = "#dfdfdf"; };
      shadow = lib.mkOption { type = colorType; default = "#808080"; };
      dark = lib.mkOption { type = colorType; default = "#000000"; };
      text = lib.mkOption { type = colorType; default = "#000000"; };
      textDisabled = lib.mkOption { type = colorType; default = "#808080"; };
      selection = lib.mkOption { type = colorType; default = "#000080"; };
      white = lib.mkOption { type = colorType; default = "#ffffff"; };
      black = lib.mkOption { type = colorType; default = "#000000"; };
      teal = lib.mkOption { type = colorType; default = "#008080"; };
      titleTop = lib.mkOption { type = colorType; default = "#000080"; };
      titleBottom = lib.mkOption { type = colorType; default = "#1084d0"; };
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
