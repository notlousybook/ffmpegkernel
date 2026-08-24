# ffmpegkernel

**FFmpeg 9.0.1, decoding H.264 on bare-metal x86_64 — no Linux, no libc, no
bootloader — with SMP, pthreads and paced 1080p60 playback in a QEMU window.**

```
qemu-system-x86_64 -m 512 -kernel movie_mt.elf -initrd clip.mp4 \
    -enable-kvm -cpu host -smp 4 -vga none -device ramfb -display gtk
```

That single command boots a freestanding ELF through PVH, brings 3 APs online
via INIT/SIPI, runs FFmpeg's h264 decoder with real `pthread_create()` worker
threads on top of a from-scratch futex/threading layer, scales frames to the
ramfb framebuffer with libswscale, and presents them at the source frame rate
in an attached GTK window — looping forever.

## What's inside

| Piece | Path | Notes |
|---|---|---|
| PVH entry + boot | `boot/pvh.S`, `boot/metal_main.c` | QEMU `-kernel` direct boot, initrd via hvm_start_info or fw_cfg |
| Interrupts & timers | `boot/intr.c`, `boot/intr_asm.S` | IDT, PIC/PIT, LAPIC TSC-deadline one-shots |
| SMP bringup | `smp/apboot.c` | INIT-SIPI with **x2APIC support** (SeaBIOS leaves guests in x2APIC mode; legacy MMIO LAPIC writes are silently dropped — this is the classic "APs never come up under KVM" trap) |
| Threading | `smp/core.c`, `smp/futex.c`, `smp/pthread_impl.c` | spinlock futexes, pthread_create/mutex/cond over per-CPU idle loops |
| libc shim | `stubs/` | weak-symbol malloc (64B-aligned pool+freelist), write→COM1, clock_gettime/nanosleep on calibrated TSC, ioctl/iconv/drm stubs |
| Playback harness | `test_harness.c` | decode-ahead ring buffer: producer decodes as fast as it can, presenter pops frames on a fixed deadline grid — decode bursts land in the queue instead of stretching intervals |
| Display | `boot/render_fb.c` | ramfb up to 4092×2304 XRGB8888 via fw_cfg DMA |

## The clock story (the fun part)

Pacing looked perfect in code but the video hitched. Instrumentation showed
frame intervals of 31ms while raw cycle counts said 15ms. Root cause: every
TSC calibration source lied.

- CMOS RTC polling → measured an 87ms window as "one second" (11× error)
- PIT channel-2 gating → consistent ~2× error
- kvmclock pvclock pairs → correct only when vCPU exits happen; early boot
  barely exits, and the values track *virtual* time, which lags wall time
  under host load

The working recipe (`boot/metal_overrides.c`): try kvmclock median-of-5 with
forced VMEXITs (poke `0x3fd`) and a ±20% cluster check; fall back to two
consecutive CMOS seconds rollovers timestamped with rdtsc, bounded so a broken
RTC can't hang boot; accept only results inside a 2.8–4.2GHz plausibility
band; else use a probe-verified constant. Result: **3.5006GHz measured vs
3498.92MHz actual, stable across boots**, and frame pacing lands at
16.666ms average against a 16.667ms budget.

## Results

Headless, KVM, `-smp 4`, i3-4150 (2C/4T), loadavg ~2:

| Metric | Before | After |
|---|---|---|
| Frame interval avg | meaningless (fast clock) | 16.666 ms |
| Late frames / 182 | 180–182 | 0–15 |
| Worst inter-frame gap | seconds | 33 ms |
| 1080p60 decode | ~53k kcy/frame | same, absorbed by ring |
| 4K2160p60 decode | works headless, ~65 ms/frame | CPU-bound (host ffmpeg does 38fps too) |

## Build

Requirements: `gcc`, GNU `make`, `python3`, `curl`, `xz`; QEMU with KVM for
running. NASM not needed (FFmpeg built with `--disable-x86asm` path is fine;
enable it if you want SIMD).

```sh
./scripts/fetch-ffmpeg.sh     # downloads FFmpeg 9.0.1, configures, builds libs
make movie_mt.elf             # bare-metal playback image
make header VIDEO=clip.mp4    # optional: embed a clip as fallback payload
./run_qemu.sh play            # attached GTK window, KVM, -smp 4
./run_qemu.sh smp             # headless decode verification (DECODE_OK)
./run_qemu.sh tcg             # no-KVM machines
```

Pass any MP4 at runtime with `-initrd your.mp4` (harness reads it as a RAM
disk); the embedded header is only the fallback. Any H.264-in-MP4 works;
decode cost is driven by resolution/preset — `x264 -tune fastdecode -bf 0`
encodes play noticeably smoother on small hosts.

## Repo layout

```
boot/         entry, interrupts, timekeeping, framebuffer
smp/          AP bringup, futexes, pthread implementation
stubs/        freestanding libc shims (weak symbols)
scripts/      fetch-ffmpeg.sh, make-video-header.py
test_harness.c  decode loop, ring-buffer pacer, benchmarks
link.ld       static link layout (text @ 1MB)
pthread_inc/  minimal pthread headers used by the FFmpeg build
```

## License

This project's own code: **MIT** — see `LICENSE`.

FFmpeg itself is **not redistributed here**; `scripts/fetch-ffmpeg.sh` pulls
it from upstream. The configure line enables only LGPL components (no
`--enable-gpl`, no `--enable-nonfree`), so everything produced is
**LGPL-2.1-or-later**. The complete license text ships as
`COPYING.LGPLv2.1`. If you distribute binaries you build with this repo,
rebuilding from these sources satisfies LGPL §6 relinkability; see LICENSE
for details. Don't push copyrighted video files — `test.mp4` is gitignored.
