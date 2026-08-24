# Makefile — bare-metal FFmpeg validation image
#
#   make            build test_harness.elf (freestanding, static)
#   make run-user   validate under qemu-x86_64 (or natively as fallback)
#   make clean

FFDIR  := build/ffmpeg-9.0.1
CC     ?= gcc
CFLAGS := -O3 -g -ffreestanding -fno-stack-protector -fno-PIC -fno-math-errno -ffast-math \
          -fno-asynchronous-unwind-tables -fno-strict-aliasing -mno-red-zone -march=x86-64-v3 \
          -I. -Istubs -I$(FFDIR) -Wall -Wno-unused-parameter -DNDEBUG

LIBS := $(FFDIR)/libavformat/libavformat.a \
        $(FFDIR)/libavcodec/libavcodec.a \
        $(FFDIR)/libavutil/libavutil.a

OBJS := test_harness.o stubs/syscall_shim.o stubs/tlsf.o stubs/libm_shim.o stubs/scanf_shim.o

BOOT_OBJS := boot/pvh.o boot/metal_main.o boot/metal_overrides.o \
             boot/intr_asm.o boot/intr.o \
             smp/core.o smp/futex.o smp/pthread_impl.o smp/apboot.o

smp/tramp.bin: smp/tramp.S smp/tramp.ld
	gcc -c $< -o smp/tramp.o
	ld -T smp/tramp.ld -o $@ smp/tramp.o

QUIET_FLAGS := $(CFLAGS) -DBENCH_QUIET
LDFLAGS := -O3 -march=x86-64-v3
test_harness_quiet.elf: $(OBJS) $(LIBS) link.ld
	gcc $(LDFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -Wl,--no-warn-rwx-segments \
	    -o $@ $(OBJS) $(LIBS) -lgcc -lz -lbz2

test_harness_quiet.o: test_harness.c stubs/shim.h test_mp4.h
	$(CC) $(QUIET_FLAGS) -c $< -o $@

kernel_quiet.elf: test_harness_quiet.o stubs/syscall_shim.o stubs/tlsf.o stubs/libm_shim.o stubs/scanf_shim.o $(BOOT_OBJS) $(LIBS) link.ld
	gcc $(LDFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -Wl,--no-warn-rwx-segments \
	    -o $@ test_harness_quiet.o stubs/syscall_shim.o stubs/tlsf.o stubs/libm_shim.o stubs/scanf_shim.o \
	    $(BOOT_OBJS) $(LIBS) -lgcc -lz -lbz2

MOVIE_OBJS := test_harness_fb.o boot/render_fb.o $(BOOT_OBJS) \
              stubs/syscall_shim.o stubs/tlsf.o stubs/libm_shim.o stubs/scanf_shim.o
SWSCALE := build/ffmpeg-9.0.1/libswscale/libswscale.a
ALL_LIBS := $(LIBS) $(SWSCALE) $(LIBS)

movie.elf: test_harness_fb.o boot/render_fb.o $(BOOT_OBJS) \
           stubs/syscall_shim.o stubs/tlsf.o stubs/libm_shim.o stubs/scanf_shim.o \
           $(LIBS) $(SWSCALE) link.ld
	gcc $(LDFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -Wl,--no-warn-rwx-segments \
	    -o $@ $(MOVIE_OBJS) $(ALL_LIBS) -lgcc -lz -lbz2

# multithreaded build: FFmpeg libs rebuilt with HAVE_THREADS=1
# (the single-threaded originals live on as ffmpeg-9.0.1-st)
FFDIR_T  := build/ffmpeg-9.0.1
LIBS_T   := $(FFDIR_T)/libavformat/libavformat.a \
            $(FFDIR_T)/libavcodec/libavcodec.a \
            $(FFDIR_T)/libavutil/libavutil.a
SWSCALE_T := $(FFDIR_T)/libswscale/libswscale.a

movie_mt.elf: test_harness_fb.o boot/render_fb.o smp/tramp.bin $(BOOT_OBJS) \
              stubs/syscall_shim.o stubs/tlsf.o stubs/libm_shim.o stubs/scanf_shim.o \
              $(LIBS_T) $(SWSCALE_T) link.ld
	gcc $(LDFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -Wl,--no-warn-rwx-segments \
	    -o $@ test_harness_fb.o boot/render_fb.o $(BOOT_OBJS) \
	    stubs/syscall_shim.o stubs/tlsf.o stubs/libm_shim.o stubs/scanf_shim.o \
	    $(LIBS_T) $(SWSCALE_T) $(LIBS_T) -lgcc -lz -lbz2

test_harness_fb.o: test_harness.c stubs/shim.h test_mp4.h
	$(CC) $(CFLAGS) -DFB_PLAYBACK -c $< -o $@

all: kernel.elf

kernel.elf: $(OBJS) $(BOOT_OBJS) $(LIBS) link.ld
	gcc $(LDFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -Wl,--no-warn-rwx-segments \
	    -Wl,-Map,kernel.map -o $@ $(OBJS) $(BOOT_OBJS) $(LIBS) -lgcc -lz -lbz2

%.o: %.c stubs/shim.h test_mp4.h link.ld
	$(CC) $(CFLAGS) -c $< -o $@

boot/%.o: boot/%.S
	$(CC) $(CFLAGS) -c $< -o $@

boot/%.o: boot/%.C
	$(CC) $(CFLAGS) -c $< -o $@

boot/%.o: boot/%.c
	$(CC) $(CFLAGS) -c $< -o $@

smp/%.o: smp/%.c smp/kern.h
	$(CC) $(CFLAGS) -c $< -o $@

test_harness.elf: $(OBJS) $(LIBS) link.ld
	gcc $(LDFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -Wl,--no-warn-rwx-segments \
	    -o $@ $(OBJS) $(LIBS) -lgcc -lz -lbz2

run-user: all
	./run_qemu.sh user

# fetch + configure + build FFmpeg 9.0.1 static libs (LGPL; see README)
ffmpeg-libs:
	./scripts/fetch-ffmpeg.sh

# embed an mp4 as the fallback header: make header VIDEO=clip.mp4
header:
	python3 scripts/make-video-header.py $(VIDEO) test_mp4.h

clean:
	rm -f *.o boot/*.o smp/*.o stubs/*.o pthread_inc/*.o \
	      *.elf kernel.map ref_bench smp/tramp.bin

.PHONY: all run-user clean ffmpeg-libs header
