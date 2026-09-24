# Runs inside the new root image (see mkrootfs.sh).  MIRROR and PKGS are set.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

cat > /etc/apt/sources.list <<EOF
deb $MIRROR noble main universe
deb $MIRROR noble-updates main universe
EOF
apt-get update -q
apt-get install -y -q --no-install-recommends $PKGS linux-tools-generic

# Ubuntu's /usr/bin/perf looks for a binary matching the running kernel;
# any recent perf works with 6.16.5.
perf_bin=$(ls -d /usr/lib/linux-tools/*/perf | sort -V | tail -1)
ln -sf "$perf_bin" /usr/local/bin/perf

# mdtest from IOR 4.0.0
cd /tmp
wget -q -O ior.tar.gz https://github.com/hpc/ior/archive/refs/tags/4.0.0.tar.gz
mkdir ior && tar -xzf ior.tar.gz -C ior --strip-components=1
cd ior
./bootstrap >/dev/null
./configure --prefix=/usr/local >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
cd / && rm -rf /tmp/ior /tmp/ior.tar.gz
command -v mdtest >/dev/null

# The Python of Fig. 11's A2, A4, A5 and A6.
bash /tmp/install_python.sh /opt/splinefs-ae-py
rm -f /tmp/install_python.sh

echo splinefs-vm > /etc/hostname
echo '/dev/vda / ext4 defaults,noatime 0 1' > /etc/fstab
passwd -d root >/dev/null
mkdir -p /etc/systemd/system/serial-getty@ttyS0.service.d
cat > /etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --keep-baud 115200,57600,38400,9600 %I $TERM
EOF
sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config
systemctl enable splinefs-ae.service ssh >/dev/null 2>&1
systemctl disable apt-daily.timer apt-daily-upgrade.timer e2scrub_all.timer \
    fstrim.timer motd-news.timer >/dev/null 2>&1 || true
apt-get clean
