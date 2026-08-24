#!/usr/bin/env bash
# ============================================================================
# run_qemu.sh - boot & verify the freestanding FFmpeg decode harness
#
#   ./run_qemu.sh user    [BINARY] [INITRD]   hosted binary under qemu-x86_64
#                                             or native fallback
#   ./run_qemu.sh kvm     [KERNEL] [INITRD]   bare metal, KVM accelerated (1 cpu)
#   ./run_qemu.sh tcg     [KERNEL] [INITRD]   bare metal, pure emulation
#   ./run_qemu.sh smp     [KERNEL] [INITRD]   bare metal, KVM + -smp 4 (threaded)
#   ./run_qemu.sh play                       attached GTK window, KVM + -smp 4
#   ./run_qemu.sh bench                       quick benchmark sweep
#
# Mode-specific defaults:
#   user     -> test_harness_quiet.elf / test_harness.elf
#               (weak hosted-syscall write/_exit: safe under Linux)
#   kvm/tcg  -> kernel_quiet.elf / kernel.elf
#   smp/play -> movie_mt.elf (frame-threaded 1080p60 playback)
#               (strong COM1/isa-debug-exit overrides: NEVER run natively!)
# INITRD defaults to test.mp4.
#
# Exit status: 0 if serial log contains DECODE_OK.
# ============================================================================
set -u
MODE="${1:-kvm}"
BIN="${2:-}"
INITRD="${3:-test.mp4}"
pick() { [ -f "$2" ] && echo "$2" || echo "$3"; }
case "$MODE" in
  user)          BIN="${BIN:-$(pick x test_harness_quiet.elf test_harness.elf)}" ;;
  kvm|tcg|bench) BIN="${BIN:-$(pick x kernel_quiet.elf kernel.elf)}" ;;
  smp|play)     BIN="${BIN:-$(pick x movie_mt.elf movie_mt.elf)}" ;;
esac
KERNEL="$BIN"
LOG="$(mktemp /tmp/ffkm-serial.XXXXXX)"

fail() { echo "FAIL: $*" >&2; exit 1; }
[ -f "$KERNEL" ] || fail "kernel '$KERNEL' not found (make all)"
[ -f "$INITRD" ] || fail "initrd '$INITRD' not found"

QCOMMON=(-m 512 -kernel "$KERNEL" -initrd "$INITRD"
         -device isa-debug-exit,iobase=0xf4,iosize=1
         -display none -no-reboot)
# playback modes use the ramfb device (1920x1080 surface)
PLAY=(-vga none -device ramfb)

case "$MODE" in
  user)
    # The default shim write()/_exit() do Linux syscalls, so the SAME binary
    # runs under qemu-x86_64 or natively. Console output = stdout.
    if command -v qemu-x86_64 >/dev/null; then
        qemu-x86_64 "$KERNEL" | tee "$LOG"
    else
        echo "qemu-x86_64 missing; running natively (equivalent validation)"
        ./"$KERNEL" > "$LOG"
    fi
    ;;
  kvm)
    command -v qemu-system-x86_64 >/dev/null || fail "install qemu-system-x86_64"
    timeout 300 qemu-system-x86_64 -enable-kvm -cpu host "${QCOMMON[@]}" \
        -serial file:"$LOG" >/dev/null 2>&1 || true
    ;;
  smp)
    command -v qemu-system-x86_64 >/dev/null || fail "install qemu-system-x86_64"
    timeout 300 qemu-system-x86_64 -enable-kvm -cpu host -smp 4 "${PLAY[@]}" \
        "${QCOMMON[@]}" -serial file:"$LOG" >/dev/null 2>&1 || true
    ;;
  play)
    command -v qemu-system-x86_64 >/dev/null || fail "install qemu-system-x86_64"
    echo "Launching attached GTK display (KVM, -smp 4). Close the window to stop."
    qemu-system-x86_64 -enable-kvm -cpu host -smp 4 "${PLAY[@]}" \
        -device isa-debug-exit,iobase=0xf4,iosize=1 \
        -display gtk -serial stdio
    exit 0
    ;;
  tcg)
    timeout 1800 qemu-system-x86_64 "${PLAY[@]}" "${QCOMMON[@]}" \
        -serial file:"$LOG" >/dev/null 2>&1 || true
    ;;
  bench)
    echo "== ours, native freestanding =="
    ./test_harness_quiet.elf 2>/dev/null | grep -aE 'bench|DECODE_OK' || true
    echo "== reference: same libs + glibc =="
    ./ref_bench 2>/dev/null | grep -aE 'bench|REF_DECODE_OK' || true
    echo "== ours, bare metal under KVM =="
    ./run_qemu.sh kvm >/dev/null 2>&1
    grep -aE 'bench|DECODE_OK' "$LOG" || true
    echo "(serial log: $LOG)"
    exit 0
    ;;
  *) fail "usage: $0 {user|kvm|tcg|bench} [KERNEL] [INITRD]" ;;
esac

echo "--- serial tail ---"
tail -6 "$LOG"
grep -aq "DECODE_OK" "$LOG" && { echo "PASS: DECODE_OK"; exit 0; }
fail "no DECODE_OK in serial log"
