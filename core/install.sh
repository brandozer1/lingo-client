#!/bin/sh
# install.sh - puts the Onsay core on a Linux server: the program, its folder and settings, signing in, and a service
# that keeps it running.
#
#   curl -fsSL https://github.com/brandozer1/lingo-client/releases/latest/download/install.sh | sh
#                                                           downloads the latest release of the core
#   sh install.sh ./bin/linux-x64/onsay-core                or installs one already here, like from a checkout
#
# As root it is for the whole server: /usr/local/bin, /var/lib/onsay and a system service. Otherwise it is for this
# user: ~/.local/bin, ~/.onsay and a user service. Run it again to update the core: the settings and sign in are kept.
set -e

ONSAY_DOWNLOAD=https://github.com/brandozer1/lingo-client/releases/latest/download   # the latest release, see .github/workflows/release.yml

case "$(uname -m)" in
  x86_64 | amd64) arch=x64 ;;
  aarch64 | arm64) arch=arm64 ;;
  *) echo "The Onsay core runs on x86_64 and aarch64 servers, and this one is $(uname -m)."; exit 1 ;;
esac
if [ "$(id -u)" = 0 ]; then bin=/usr/local/bin home=/var/lib/onsay
else bin="$HOME/.local/bin" home="$HOME/.onsay"; fi
mkdir -p "$bin" "$home/lingos"

# The program, put beside the old one and then moved over it, so a running core is never half replaced.
if [ -n "$1" ]; then cp "$1" "$bin/onsay-core.new"
else curl -fsSL "$ONSAY_DOWNLOAD/onsay-core-linux-$arch" -o "$bin/onsay-core.new"; fi
chmod 755 "$bin/onsay-core.new"
mv "$bin/onsay-core.new" "$bin/onsay-core"

# Its settings, see README.md: what this server is called, its folder, and what its lingos may do.
settings="$home/core.json"
[ -f "$settings" ] || printf '{"name": "%s", "home": "%s", "allow": ["files.read", "net"]}\n' "$(uname -n)" "$home" > "$settings"
grep -q '"token"' "$settings" || "$bin/onsay-core" "$settings" --sign-in

# A service that starts it with the server and starts it again if it stops.
unit="[Unit]
Description=Onsay core
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=$bin/onsay-core $settings
WorkingDirectory=$home
Restart=on-failure
RestartSec=5

[Install]"
if ! command -v systemctl > /dev/null; then
  echo "Signed in. This server has no systemd, so start the core yourself: $bin/onsay-core $settings"
elif [ "$(id -u)" = 0 ]; then
  printf '%s\nWantedBy=multi-user.target\n' "$unit" > /etc/systemd/system/onsay.service
  systemctl daemon-reload
  systemctl enable onsay > /dev/null 2>&1
  systemctl restart onsay
  echo "Onsay is running on this server. Its log: journalctl -u onsay -f"
else
  mkdir -p "$HOME/.config/systemd/user"
  printf '%s\nWantedBy=default.target\n' "$unit" > "$HOME/.config/systemd/user/onsay.service"
  systemctl --user daemon-reload
  systemctl --user enable onsay > /dev/null 2>&1
  systemctl --user restart onsay
  echo "Onsay is running for $(id -un). Its log: journalctl --user -u onsay -f"
  echo "To keep it running after you log out: sudo loginctl enable-linger $(id -un)"
fi
