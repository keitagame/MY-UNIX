# ============================================================
# Makefile - Unix互換カーネル ビルドシステム
# ============================================================

# ターゲットアーキテクチャ
ARCH     := x86_64
CROSS    :=
CC       := $(CROSS)gcc
AS       := $(CROSS)nasm
LD       := $(CROSS)ld
AR       := $(CROSS)ar
OBJCOPY  := $(CROSS)objcopy
GRUB_MK  := grub-mkrescue

# 出力ファイル
KERNEL   := kernel.elf
ISO      := unixkernel.iso
INITRD   := initrd.cpio

# ディレクトリ
BUILD    := build
ISODIR   := $(BUILD)/iso
ISOBOOT  := $(ISODIR)/boot
ISOGRUB  := $(ISOBOOT)/grub

# ============================================================
# コンパイルフラグ
# ============================================================
CFLAGS := \
    -std=c11 \
    -ffreestanding \
    -fno-stack-protector \
    -fno-builtin \
    -fno-pie \
    -fno-pic \
    -mno-red-zone \
    -mno-sse \
    -mno-sse2 \
    -mno-avx \
    -m64 \
    -mcmodel=kernel \
    -O2 \
    -Wall \
    -Wextra \
    -Wno-unused-parameter \
    -Wno-implicit-fallthrough \
    -I. \
    -Iinclude \
    -g

ASFLAGS := -f elf64 -g -F dwarf

LDFLAGS := \
    -T kernel.ld \
    -nostdlib \
    -n \
    -z max-page-size=0x1000

# ============================================================
# ソースファイル
# ============================================================
C_SRCS := \
    main.c \
    arch/x86_64/gdt.c \
    arch/x86_64/idt.c \
    mm/pmm.c \
    mm/vmm.c \
    mm/heap.c \
    proc/process.c \
    proc/sched.c \
    proc/elf.c \
    proc/syscall_impl.c \
    fs/vfs.c \
    fs/tmpfs_devfs.c

ASM_SRCS := \
    arch/x86_64/boot.asm

C_OBJS   := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS))
ASM_OBJS := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS))
ALL_OBJS := $(ASM_OBJS) $(C_OBJS)

# ============================================================
# メインターゲット
# ============================================================
.PHONY: all clean iso run run-serial debug install check-tools

all: check-tools $(BUILD)/$(KERNEL)

# ============================================================
# ツール確認
# ============================================================
check-tools:
	@which nasm     > /dev/null 2>&1 || (echo "ERROR: nasm not found. Install: apt install nasm" && exit 1)
	@which $(CC)    > /dev/null 2>&1 || (echo "ERROR: gcc not found" && exit 1)
	@which $(LD)    > /dev/null 2>&1 || (echo "ERROR: ld not found" && exit 1)
	@echo "Build tools OK"

# ============================================================
# ディレクトリ作成
# ============================================================
$(BUILD)/arch/x86_64:
	mkdir -p $@
$(BUILD)/mm:
	mkdir -p $@
$(BUILD)/proc:
	mkdir -p $@
$(BUILD)/fs:
	mkdir -p $@
$(BUILD):
	mkdir -p $@

# ============================================================
# コンパイル
# ============================================================
$(BUILD)/arch/x86_64/%.o: arch/x86_64/%.asm | $(BUILD)/arch/x86_64
	@echo "  AS   $<"
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD)/%.o: %.c | $(BUILD)
	@echo "  CC   $<"
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# ============================================================
# リンク
# ============================================================
$(BUILD)/$(KERNEL): $(ALL_OBJS) kernel.ld
	@echo "  LD   $@"
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS)
	@echo "  SIZE:"
	@size $@

# ============================================================
# initrd 生成
# ============================================================
initrd: $(BUILD)/$(INITRD)

$(BUILD)/$(INITRD): initrd_src/
	@echo "  CPIO $(INITRD)"
	@mkdir -p $(BUILD)
	cd initrd_src && find . | cpio --quiet -H newc -o > ../$(BUILD)/$(INITRD)
	@ls -lh $(BUILD)/$(INITRD)

# ============================================================
# ISO イメージ作成
# ============================================================
iso: $(BUILD)/$(KERNEL) $(BUILD)/$(INITRD)
	@echo "  ISO  $(ISO)"
	@mkdir -p $(ISOGRUB)
	cp $(BUILD)/$(KERNEL)  $(ISOBOOT)/kernel.elf
	cp $(BUILD)/$(INITRD)  $(ISOBOOT)/initrd.cpio
	@cat > $(ISOGRUB)/grub.cfg << 'EOF'
	set timeout=3
	set default=0

	menuentry "Unix Kernel" {
	multiboot2 /boot/kernel.elf
	module2    /boot/initrd.cpio
	boot
	}

	menuentry "Unix Kernel (serial debug)" {
	multiboot2 /boot/kernel.elf console=ttyS0
	module2    /boot/initrd.cpio
	boot
	}
	EOF
	$(GRUB_MK) -o $(BUILD)/$(ISO) $(ISODIR)
	@echo "  ISO created: $(BUILD)/$(ISO)"
	@ls -lh $(BUILD)/$(ISO)

# ============================================================
# QEMU 実行
# ============================================================
QEMU       := qemu-system-x86_64
QEMU_FLAGS := \
    -machine q35 \
    -cpu qemu64 \
    -m 512M \
    -serial stdio \
    -display none \
    -no-reboot \
    -no-shutdown

QEMU_KVM_FLAGS := \
    -machine q35,accel=kvm \
    -cpu host \
    -m 512M \
    -serial stdio \
    -display none \
    -no-reboot

run: iso
	$(QEMU) $(QEMU_FLAGS) -cdrom $(BUILD)/$(ISO)

run-kvm: iso
	$(QEMU) $(QEMU_KVM_FLAGS) -cdrom $(BUILD)/$(ISO)

run-serial: $(BUILD)/$(KERNEL) $(BUILD)/$(INITRD)
	$(QEMU) $(QEMU_FLAGS) \
	    -kernel $(BUILD)/$(KERNEL) \
	    -initrd $(BUILD)/$(INITRD)

# GDB デバッグ
debug: $(BUILD)/$(KERNEL) $(BUILD)/$(INITRD)
	$(QEMU) $(QEMU_FLAGS) \
	    -kernel $(BUILD)/$(KERNEL) \
	    -initrd $(BUILD)/$(INITRD) \
	    -s -S &
	gdb $(BUILD)/$(KERNEL) \
	    -ex "target remote :1234" \
	    -ex "break kernel_entry" \
	    -ex "continue"

# ============================================================
# 実機インストール (注意: デバイスを正しく指定すること!)
# ============================================================
# 使い方: make install DEVICE=/dev/sdX
DEVICE ?= /dev/sdX

install: iso
	@echo "WARNING: This will overwrite $(DEVICE)!"
	@echo "Press Ctrl+C to cancel, Enter to continue..."
	@read dummy
	@echo "Installing GRUB to $(DEVICE)..."
	grub-install --target=x86_64-efi --efi-directory=/boot/efi \
	    --boot-directory=$(ISOBOOT) $(DEVICE) || \
	grub-install --target=i386-pc --boot-directory=$(ISOBOOT) $(DEVICE)
	@cp $(ISOBOOT)/kernel.elf  /boot/unixkernel.elf
	@cp $(ISOBOOT)/initrd.cpio /boot/unixkernel-initrd.cpio
	@echo "Add to /boot/grub/grub.cfg:"
	@echo '  menuentry "Unix Kernel" {'
	@echo '    multiboot2 /boot/unixkernel.elf'
	@echo '    module2    /boot/unixkernel-initrd.cpio'
	@echo '    boot'
	@echo '  }'

# ============================================================
# initrd_src ディレクトリのセットアップ
# ============================================================
setup-initrd:
	@echo "Setting up initrd directory structure..."
	@mkdir -p initrd_src/{bin,sbin,lib,lib64,usr/{bin,lib},etc,dev,proc,sys,tmp,root,home,var/{log,run}}
	@echo "#!/bin/sh" > initrd_src/init
	@echo "mount -t proc proc /proc" >> initrd_src/init
	@echo "mount -t sysfs sys /sys" >> initrd_src/init
	@echo "echo 'Unix Kernel booted!'" >> initrd_src/init
	@echo "exec /bin/sh" >> initrd_src/init
	@chmod +x initrd_src/init
	@echo "initrd_src/ created. Add your binaries (busybox etc.) to initrd_src/bin/"
	@echo ""
	@echo "Quick start with BusyBox:"
	@echo "  cp /path/to/busybox initrd_src/bin/busybox"
	@echo "  (cd initrd_src/bin && ln -s busybox sh && ln -s busybox ls && ...)"

# ============================================================
# 統計・チェック
# ============================================================
stats: $(BUILD)/$(KERNEL)
	@echo "=== Kernel Statistics ==="
	@wc -l $(C_SRCS) $(ASM_SRCS) | tail -1
	@size $(BUILD)/$(KERNEL)
	@echo "Section sizes:"
	@$(OBJCOPY) --only-section=.text   -O binary /dev/null /dev/null 2>/dev/null || true
	nm $(BUILD)/$(KERNEL) | grep -E " [Tt] " | sort -k1 | head -20

# シンボルマップ出力
map: $(BUILD)/$(KERNEL)
	nm $(BUILD)/$(KERNEL) | sort > $(BUILD)/kernel.map
	@echo "Symbol map: $(BUILD)/kernel.map"

# ============================================================
# クリーン
# ============================================================
clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf initrd_src

# ============================================================
# ヘルプ
# ============================================================
help:
	@echo "Unix互換カーネル Makefile"
	@echo ""
	@echo "主要ターゲット:"
	@echo "  all          カーネルELFをビルド"
	@echo "  iso          ISOイメージ作成 (要grub-mkrescue)"
	@echo "  run          QEMUで実行 (要QEMU + ISO)"
	@echo "  run-serial   QEMUで直接起動 (ISO不要)"
	@echo "  debug        GDBデバッグ"
	@echo "  setup-initrd initrd_srcディレクトリを作成"
	@echo "  stats        ビルド統計"
	@echo "  install      実機にインストール (DEVICE=/dev/sdX)"
	@echo "  clean        ビルドファイル削除"
	@echo ""
	@echo "BusyBoxで最小initrdを作る例:"
	@echo "  make setup-initrd"
	@echo "  cp busybox-static initrd_src/bin/busybox"
	@echo "  (cd initrd_src/bin && for cmd in sh ls cat echo mount; do ln -sf busybox \$\$cmd; done)"
	@echo "  make initrd"
	@echo "  make run-serial"
