#!/bin/bash

set -ex -o pipefail

# Test parameters
DEVICE_MCU="/dev/ttyUSB3"
DEVICE_CONSOLE="/dev/ttyUSB2"
BAUD=115200
SLEEP_BEFORE_INPUT=60
INPUT_TIMEOUT=10
TIMEOUT=70

fatal() {
    echo "$(basename "$0"): $*" >&2
    exit 1
}

# Path declaration
[ -n "${TEST_BOARD:-}" ] || fatal "TEST_BOARD not set"
DTB="/dtb/${TEST_BOARD}.dtb"
[ -f "${DTB}" ] || fatal "missing ${DTB}"
TFTP_ROOT="/srv/tftp"
TFTP="${TFTP_ROOT}/${TEST_BOARD}"
SERIAL_LOG="smoke.serial"

# Preparing artifacts
mkdir -p "${TFTP}"
cp -f ./binaries/initrd.img "${TFTP}"/initrd.img
cp -f ./binaries/Image.gz "${TFTP}"/Image.gz
cp -f "${DTB}" "${TFTP}"/dt.dtb

# Making xen bootable
mkimage -A riscv -C none -T kernel -a 0x84000000 -e 0x84000000 -n "XEN" \
    -d ./binaries/xen "${TFTP}"/xen.uImage

echo "
tftpboot 0x84000000 ${TEST_BOARD}/xen.uImage
tftpboot 0x88000000 ${TEST_BOARD}/dt.dtb
tftpboot 0x808ef000 ${TEST_BOARD}/Image.gz
tftpboot 0x90400000 ${TEST_BOARD}/initrd.img
bootm 0x84000000 - 0x88000000
" > "${TFTP}"/boot.txt

# Creating boot.scr from boot.txt
mkimage -A riscv -T script -C none -n "OpenEmbedded-Sifive-hifive-premier-p550" \
    -d "${TFTP}"/boot.txt "${TFTP}"/boot.scr

# Setting up the board
echo "[INFO] Configuring UART"
stty -F "${DEVICE_CONSOLE}" ${BAUD} raw -echo
stty -F "${DEVICE_MCU}" ${BAUD} raw -echo

# Leave the board powered off on any exit path
trap 'echo "sompower-s 0" > "${DEVICE_MCU}"' EXIT

# Force power cycle
echo "sompower-s 0" > "${DEVICE_MCU}"
sleep 5

echo "sompower-s 1" > "${DEVICE_MCU}"
sleep 5

# ---------------------------------------------
# START continuous logging of console
# ---------------------------------------------
echo "[INFO] Starting console logger..."

timeout "$TIMEOUT"s cat "$DEVICE_CONSOLE" | tee "${SERIAL_LOG}" &
CONSOLE_PID=$!

# ---------------------------------------------
# Wait boot
# ---------------------------------------------
sleep "$SLEEP_BEFORE_INPUT"

# ---------------------------------------------
# Send command to SoC
# ---------------------------------------------
echo "[INFO] Sending 'ls' command"
echo "ls" > "$DEVICE_CONSOLE"

# ---------------------------------------------
# Wait for output of ls
# ---------------------------------------------
sleep "$INPUT_TIMEOUT"

# Stop logger cleanly
kill "$CONSOLE_PID" 2>/dev/null || true
wait "$CONSOLE_PID" || true

# Check boot message
if grep -q "Hello RISC-V World!" "${SERIAL_LOG}"; then
    echo "[INFO] Boot successful!"
else
    echo "[ERROR] Boot failed: no 'Hello RISC-V World!'"
    exit 1
fi
