#!/usr/bin/env bash
# 完整 arm64 虚拟机启动检查（tool69 上是 TCG 软件模拟，不是 KVM 加速）
#
# 为什么不是 KVM：宿主是 x86_64，/dev/kvm 只能加速同架构客户机；aarch64 客户机
# 在 x86 上只能 TCG 全系统模拟，启动很慢。这里只验证「完整 arm64 Linux 能起来、
# 能登录、能联网」，前端层验证另走宿主/arm64-chroot 的 RetroArch（见 README.txt）。
set -uo pipefail
source /opt/mca/env.sh
V=$MCA_VM
IMG="$V/debian-12-genericcloud-arm64.qcow2"
PUB=$(cat /root/.ssh/id_ed25519_mca.pub)
LOG="$V/vm.log"
SERIAL="$V/serial.log"

cd "$V"
cat > user-data <<EOF
#cloud-config
users:
  - name: debian
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys:
      - $PUB
ssh_pwauth: false
hostname: mca-vm
EOF
cat > meta-data <<EOF
instance-id: mca-vm-1
local-hostname: mca-vm
EOF
genisoimage -output seed.iso -volid cidata -joliet -rock user-data meta-data \
    > /dev/null 2>&1 || { echo "SEED_ISO_FAIL"; exit 2; }

# 用副本启动，云镜像本体保持只读可复用
[ -f run.qcow2 ] || qemu-img create -f qcow2 -F qcow2 -b "$IMG" run.qcow2 > /dev/null

qemu-system-aarch64 \
    -M virt -cpu cortex-a57 -m 2048 -smp 2 \
    -bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
    -drive if=none,id=hd,file=run.qcow2,format=qcow2 \
    -device virtio-blk-pci,drive=hd,romfile= \
    -drive if=none,id=seed,file=seed.iso,format=raw \
    -device virtio-blk-pci,drive=seed,romfile= \
    -netdev user,id=net,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net,romfile= \
    -display none -serial "file:$SERIAL" -monitor none \
    > "$LOG" 2>&1 &
QPID=$!
echo "QEMU_PID=$QPID"

SSH="ssh -i /root/.ssh/id_ed25519_mca -p 2222 -o BatchMode=yes \
     -o ConnectTimeout=3 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
     debian@127.0.0.1"
ok=0
for i in $(seq 1 100); do
    if $SSH true > /dev/null 2>&1; then ok=1; break; fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 5
done
echo "SSH_READY=$ok  (等待约 $((i * 5)) 秒)"
if [ "$ok" = 1 ]; then
    echo "== guest info =="
    $SSH 'uname -m; . /etc/os-release; echo "$PRETTY_NAME"; nproc;
          ip -4 addr show scope global | grep inet | head -2;
          systemctl is-system-running 2>/dev/null || true' 2>&1 | head -12
    echo "== retroarch 包可见性（不安装） =="
    $SSH 'apt-cache policy retroarch 2>/dev/null | head -3' 2>&1 | head -4
    $SSH 'sudo poweroff' > /dev/null 2>&1 || true
fi
kill "$QPID" > /dev/null 2>&1 || true
wait "$QPID" 2>/dev/null || true
echo "== serial tail =="
tail -12 "$SERIAL" 2>/dev/null
echo "VM_BOOT_DONE"
