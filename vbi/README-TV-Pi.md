# Teletext on a real TV from a separate Raspberry Pi

This setup uses two Pis:

- **Server Pi**: runs `ttxserver` and the feeds, and writes the pages to `server/pages`.
- **TV Pi**: connected to the TV by composite video. It reads the server's pages over the network and broadcasts them with `ttxvbi` and [raspi-teletext](https://github.com/ali1234/raspi-teletext).

Tested on a Raspberry Pi Model B Rev 2 (32-bit Raspberry Pi OS) as the TV Pi.
**A Pi 5 cannot be used**: it lacks the VideoCore services raspi-teletext needs.

---

## 1. Video settings on the TV Pi

Edit `/boot/firmware/config.txt` (on older OS releases, `/boot/config.txt`):

```
#dtoverlay=vc4-kms-v3d      <- comment out: raspi-teletext needs the legacy display driver
sdtv_mode=2                 <- PAL composite (teletext is PAL only)
```

Reboot, then check that `/dev/vchiq` exists.

## 2. Build raspi-teletext

Newer Raspberry Pi OS releases no longer ship `libraspberrypi-dev`, so build the
VideoCore userland libraries first. They install to `/opt/vc`:

```bash
sudo apt install git cmake build-essential
git clone https://github.com/raspberrypi/userland
cd userland
./buildme              # 32-bit OS;  use ./buildme --aarch64 on a 64-bit OS
echo /opt/vc/lib | sudo tee /etc/ld.so.conf.d/00-vcms.conf
sudo ldconfig
```

On a Pi 1 this takes an hour or more. If the build is killed for lack of memory,
raise `CONF_SWAPSIZE` in `/etc/dphys-swapfile` to 1024.

```bash
cd ~
git clone https://github.com/ali1234/raspi-teletext
cd raspi-teletext
make
sudo ./tvctl on
./teletext             # built-in demo: press TEXT on the TV
```

## 3. Build ttxvbi

```bash
cd ~
git clone https://github.com/bartleph/Teletext
cd Teletext/vbi
g++ -O2 -Wno-psabi -o ttxvbi ttxvbi.cpp
```

`-Wno-psabi` only hides harmless GCC ABI notes on 32-bit ARM.

Install the three programs:

```bash
sudo cp ~/raspi-teletext/teletext ~/raspi-teletext/tvctl /usr/local/bin/
sudo cp ~/Teletext/vbi/ttxvbi /usr/local/bin/
```

## 4. Share the pages folder from the server Pi (NFS)

**On the server Pi** (path and case must match exactly; check with `realpath`):

```bash
sudo apt install nfs-kernel-server
echo "/home/pi/teletext/server/pages *(ro,sync,no_subtree_check)" | sudo tee -a /etc/exports
sudo exportfs -ra
sudo exportfs -v          # should list the share
```

**On the TV Pi** (replace `SERVER_IP` with the server Pi's address, from `hostname -I` on the server):

```bash
sudo apt install nfs-common
sudo mkdir -p /mnt/ttxpages
```

Add this line to `/etc/fstab`:

```
SERVER_IP:/home/pi/teletext/server/pages  /mnt/ttxpages  nfs  ro,_netdev,x-systemd.automount  0  0
```

```bash
sudo systemctl daemon-reload
sudo systemctl start mnt-ttxpages.automount
ls /mnt/ttxpages          # should list the .tti pages
```

## 5. Start automatically at boot

Copy `teletext.service` from this folder:

```bash
sudo cp ~/Teletext/vbi/teletext.service /etc/systemd/system/
sudo systemctl enable NetworkManager-wait-online.service
sudo systemctl daemon-reload
sudo systemctl enable --now teletext
systemctl status teletext     # should say active (running)
```

The service waits until the pages folder is reachable, so a slow network or a
server that is switched off at boot doesn't stop it. It comes on air once the
pages appear.

| Task | Command |
|---|---|
| Live log | `journalctl -u teletext -f` |
| Restart after rebuilding ttxvbi | `sudo install ttxvbi /usr/local/bin/ && sudo systemctl restart teletext` |
| Stop / start | `sudo systemctl stop teletext` / `start` |
| Disable at boot | `sudo systemctl disable teletext` |

## Troubleshooting

- **`cannot find -lbcm_host`** when building raspi-teletext: the userland libraries aren't installed (step 2).
- **`mount ... didn't pass remote address`**: `nfs-common` isn't installed on the TV Pi.
- **`access denied by server`**: the path in `/etc/exports` doesn't match exactly. Check the case, and use the path `realpath` gives.
- **TV page counter stops and later pages never arrive**: the service isn't running. Check `systemctl status teletext` and `journalctl -u teletext`. Older TVs keep received pages in memory, so earlier pages can still appear after the stream has stopped.
- **Check every page without a TV:**
  ```bash
  for f in /mnt/ttxpages/P*.tti; do p=$(basename "$f" .tti); p=${p#P}; ttxvbi -d /mnt/ttxpages -p "$p" >/dev/null 2>&1; r=$?; [ $r -ne 0 ] && echo "page $p -> exit $r"; done; echo "check finished"
  ```
