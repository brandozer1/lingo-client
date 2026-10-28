#!/bin/sh
# install.sh - puts the lingo client on a Linux server: the program, the onsay lingo, its folder and settings, and a
# service that keeps it running. It answers its own lingos on port 6969, with no Onsay account until it is signed in.
#
#   curl -fsSL https://github.com/brandozer1/lingo-client/releases/latest/download/install.sh | sh
#                                             downloads the latest release and installs it
#   curl -fsSL .../install.sh | sh -s -- --sign-in
#                                             the same, signed in to Onsay
#   sh install.sh ./bin/linux-x64/onsay-core  installs one already here, like from a checkout, with lingos/onsay.lingo
#                                             beside it
#
#   --sign-in   signs it in to Onsay from any device's browser, so Onsay can reach its lingos: onsay-core core.json call
#               onsay.sign_in, which can be run any time after
# As root it is for the whole server: /usr/local/bin, /var/lib/onsay and a system service. Otherwise it is for this
# user: ~/.local/bin, ~/.onsay and a user service. Run it again to update it: its settings, policy and sign in are kept.
# A lingo put in its lingos folder runs once started, and other computers reach it once shared (core/README.md).
set -e

ONSAY_DOWNLOAD=https://github.com/brandozer1/lingo-client/releases/latest/download   # the latest release, see .github/workflows/release.yml

signin= from=
for a in "$@"; do
  case "$a" in
    --sign-in) signin=1 ;;
    *) from="$a" ;;
  esac
done
case "$(uname -m)" in
  x86_64 | amd64) arch=x64 ;;
  aarch64 | arm64) arch=arm64 ;;
  *) echo "The lingo client runs on x86_64 and aarch64 servers, and this one is $(uname -m)."; exit 1 ;;
esac
if [ "$(id -u)" = 0 ]; then bin=/usr/local/bin home=/var/lib/onsay
else bin="$HOME/.local/bin" home="$HOME/.onsay"; fi
mkdir -p "$bin" "$home/lingos"

# The program, put beside the old one and then moved over it, so a running one is never half replaced.
if [ -n "$from" ]; then cp "$from" "$bin/onsay-core.new"
else curl -fsSL "$ONSAY_DOWNLOAD/onsay-core-linux-$arch" -o "$bin/onsay-core.new"; fi
chmod 755 "$bin/onsay-core.new"
mv "$bin/onsay-core.new" "$bin/onsay-core"

# The onsay lingo, which connects it to Onsay once it is signed in (../lingos/onsay/README.md).
if [ -n "$from" ]; then cp "$(dirname "$from")/lingos/onsay.lingo" "$home/lingos/onsay.lingo"
else curl -fsSL "$ONSAY_DOWNLOAD/onsay-linux-$arch.lingo" -o "$home/lingos/onsay.lingo"; fi

# Its settings, see core/README.md: its folder, which holds its lingos, its policy and everything it keeps. The onsay
# lingo runs, and keeps its sign in as its own setting.
settings="$home/core.json"
[ -f "$settings" ] || printf '{"home": "%s"}\n' "$home" > "$settings"
"$bin/onsay-core" "$settings" start onsay > /dev/null
if [ -n "$signin" ] && ! grep -q '"token":"' "$home/settings/onsay.json" 2> /dev/null; then "$bin/onsay-core" "$settings" call onsay.sign_in > /dev/null; fi

# A service that starts it with the server and starts it again if it stops.
unit="[Unit]
Description=Lingo client
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=$bin/onsay-core $settings
WorkingDirectory=$home
Restart=on-failure
RestartSec=5

[Install]"
if ! command -v systemctl > /dev/null; then
  echo "Installed. This server has no systemd, so start it yourself: $bin/onsay-core $settings"
elif [ "$(id -u)" = 0 ]; then
  printf '%s\nWantedBy=multi-user.target\n' "$unit" > /etc/systemd/system/onsay.service
  systemctl daemon-reload
  systemctl enable onsay > /dev/null 2>&1
  systemctl restart onsay
  echo "The lingo client is running on this server, on port 6969. Its log: journalctl -u onsay -f"
else
  mkdir -p "$HOME/.config/systemd/user"
  printf '%s\nWantedBy=default.target\n' "$unit" > "$HOME/.config/systemd/user/onsay.service"
  systemctl --user daemon-reload
  systemctl --user enable onsay > /dev/null 2>&1
  systemctl --user restart onsay
  echo "The lingo client is running for $(id -un), on port 6969. Its log: journalctl --user -u onsay -f"
  echo "To keep it running after you log out: sudo loginctl enable-linger $(id -un)"
fi
echo "Its lingos go in $home/lingos. Start one: $bin/onsay-core $settings start <lingo>"
echo "Let other computers reach it: $bin/onsay-core $settings share <lingo> public, or ip and an address"
echo "Sign it in to Onsay, so Onsay and your other devices can reach it: $bin/onsay-core $settings call onsay.sign_in"
