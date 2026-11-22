#!/bin/sh
# A private 1600x900 screen with real GPU access for WAR: labwc (headless wlroots compositor) + Xwayland.
# Xvfb has no GPU, so DXVK cannot present there; cage crashed on Xwayland window teardown, labwc does not.
#   usage: war-display.sh            (run detached: systemd-run --user --unit=war-display war-display.sh)
# Env: WAR_RENDER_NODE (default /dev/dri/renderD128, the compositor's GPU; the game picks its own via DXVK).
# Writes "DISPLAY WAYLAND_DISPLAY" to $XDG_RUNTIME_DIR/war-display.env once up.
# Screenshot: WAYLAND_DISPLAY=<w> grim out.png. Input: wlrctl (pointer) and wtype (keys); xdotool clicks are ignored.
# Needs: labwc, Xwayland, wlr-randr; for driving it: grim, wlrctl, wtype.
unset DISPLAY XAUTHORITY WAYLAND_DISPLAY
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=gles2
export WLR_RENDER_DRM_DEVICE=${WAR_RENDER_NODE:-/dev/dri/renderD128}
CFG=${XDG_RUNTIME_DIR:-/tmp}/war-labwc
mkdir -p "$CFG/labwc"
printf '<labwc_config><core><decoration>server</decoration></core></labwc_config>\n' > "$CFG/labwc/rc.xml"
cat > "$CFG/startup.sh" <<'EOF'
#!/bin/sh
wlr-randr --output HEADLESS-1 --custom-mode 1600x900
printf '%s %s\n' "$DISPLAY" "$WAYLAND_DISPLAY" > "${XDG_RUNTIME_DIR:-/tmp}/war-display.env"
EOF
chmod +x "$CFG/startup.sh"
export XDG_CONFIG_HOME=$CFG
exec labwc -s "$CFG/startup.sh"
