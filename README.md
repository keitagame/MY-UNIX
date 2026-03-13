# Unix互換カーネル

x86_64 向けの Unix 互換カーネル実装です。  
GRUB2 (Multiboot2) でブート可能で、実機インストールや QEMU での動作確認ができます。  
Linux ELF バイナリ (gcc, bash, busybox 等) がそのまま動作します。

---

## アーキテクチャ概要

```
unixkernel/
├── arch/x86_64/
│   ├── boot.asm        # Multiboot2エントリ/LongMode移行/syscall_entry/IDTスタブ
│   ├── gdt.c           # GDT + TSS
│   └── idt.c           # IDT + PIC + PITタイマー + syscall(SYSCALL命令)
├── mm/
│   ├── pmm.c           # 物理メモリマネージャ(ビットマップ)
│   ├── vmm.c           # 仮想メモリ(4レベルページング/VMA)
│   └── heap.c          # カーネルヒープ(スラブ + free-list)
├── proc/
│   ├── process.c       # プロセス管理 fork/exit/wait/signal
│   ├── sched.c         # スケジューラ(優先度付きRR) + waitqueue
│   ├── elf.c           # ELF64ローダー
│   └── syscall_impl.c  # Linux互換syscall実装 (50+)
├── fs/
│   ├── vfs.c           # VFSコア + CPIOパーサー + initrd
│   └── tmpfs_devfs.c   # tmpfs / devfs / /dev/null,zero,random
├── include/
│   ├── types.h         # 基本型・マクロ・ポート操作
│   ├── kernel.h        # printk/panic/list/spinlock/atomic
│   ├── mm.h            # PMM/VMM/heap API
│   ├── process.h       # PCB/スケジューラ/ELF API
│   ├── fs.h            # VFS/vnode/file API
│   ├── syscall.h       # syscall番号定義
│   └── multiboot2.h    # Multiboot2構造体
├── main.c              # kernel_entry / printk / panic / 文字列関数 / TTY / pipe
├── kernel.ld           # リンカスクリプト (higher-half: 0xFFFFFFFF80000000)
└── Makefile
```

### 実装された機能

| カテゴリ | 内容 |
|----------|------|
| **ブート** | GRUB2 Multiboot2、higher-half カーネル (0xFFFFFFFF80000000) |
| **アーキテクチャ** | GDT/TSS/IDT、x86_64 PML4 4レベルページング、SYSCALL/SYSRET |
| **メモリ管理** | ビットマップPMM、COW対応VMM、スラブ+フリーリストヒープ |
| **プロセス** | fork/exec/exit/wait、シグナル(POSIX)、優先度付きスケジューラ |
| **ELFローダー** | ELF64 静的/PIE バイナリ、argv/envp/auxv スタック構築 |
| **ファイルシステム** | VFS、initrd(CPIO newc)、tmpfs、devfs |
| **デバイス** | /dev/null、/dev/zero、/dev/random、TTY(COM1+VGA) |
| **システムコール** | Linux x86_64 ABI 互換 50+ syscall |
| **I/O** | パイプ、ブロッキングI/O、ウェイトキュー |

---

## ビルド手順

### 必要なツール

```bash
# Ubuntu / Debian
sudo apt install gcc nasm binutils grub-pc-bin grub-efi-amd64-bin \
                 xorriso qemu-system-x86 gdb

# Arch Linux
sudo pacman -S gcc nasm binutils grub xorriso qemu gdb

# macOS (Homebrew + cross compiler)
brew install x86_64-elf-gcc nasm x86_64-elf-binutils xorriso qemu
# Makefileの CROSS := x86_64-elf- に変更
```

### ビルドとQEMU起動

```bash
# 1. ソースを展開
cd unixkernel/

# 2. initrd構造を作成 (BusyBoxを使う場合)
make setup-initrd

# BusyBox静的バイナリを入手
wget https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox
cp busybox initrd_src/bin/busybox
chmod +x initrd_src/bin/busybox

# BusyBoxコマンドのシンボリックリンク
cd initrd_src/bin
for cmd in sh bash ls cat echo cp mv rm mkdir rmdir ln chmod chown \
           mount umount grep sed awk find xargs ps kill sleep; do
    ln -sf busybox $cmd
done
cd ../..

# /sbin/init を作成
mkdir -p initrd_src/sbin
cat > initrd_src/sbin/init << 'EOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
echo "Unix Kernel - boot OK"
echo "Type 'exec sh' to start shell"
exec /bin/sh
EOF
chmod +x initrd_src/sbin/init

# 3. ビルド
make all

# 4. initrd 生成
make initrd

# 5. QEMUで起動 (シリアルコンソール)
make run-serial

# または ISO で起動
make iso
make run
```

---

## 実機インストール

### USB メモリへの書き込み (Live USB)

```bash
# ISOをビルド
make iso

# USBデバイスを確認 (必ず正しいデバイスを指定!)
lsblk

# USBに書き込む (例: /dev/sdb)
sudo dd if=build/unixkernel.iso of=/dev/sdb bs=4M status=progress
sync
```

### 既存のGRUB2環境への追加インストール

```bash
# カーネルとinitrdをコピー
sudo cp build/kernel.elf  /boot/unixkernel.elf
sudo cp build/initrd.cpio /boot/unixkernel-initrd.cpio

# /boot/grub/grub.cfg に追加
sudo tee -a /boot/grub/grub.cfg << 'EOF'
menuentry "Unix Kernel" {
    insmod multiboot2
    multiboot2 /boot/unixkernel.elf
    module2    /boot/unixkernel-initrd.cpio
    boot
}
EOF

# GRUBを更新
sudo update-grub
```

### EFI システムへのインストール

```bash
# EFIパーティションにGRUBをインストール
sudo grub-install --target=x86_64-efi \
    --efi-directory=/boot/efi \
    --bootloader-id=UnixKernel

sudo cp build/kernel.elf  /boot/unixkernel.elf
sudo cp build/initrd.cpio /boot/unixkernel-initrd.cpio

# /boot/grub/grub.cfg に上記エントリを追加
sudo update-grub
```

---

## initrd の構成

gcc/bash などの動的リンクバイナリを動かすには、動的ライブラリが必要です。

### 静的リンクのみで動かす (最小構成)

```
initrd_src/
├── sbin/init       (シェルスクリプトまたはstatic binary)
├── bin/
│   ├── busybox     (静的リンク busybox)
│   ├── sh -> busybox
│   └── ...
├── etc/
│   └── passwd
└── ...
```

### gcc / bash を動かす (動的リンク)

```bash
# musl-libc + musl-cross-make で静的リンクgccをビルドするか
# Alpine Linux の apk で musl ベースの gcc を取得

# または: glibc + ld-linux-x86-64.so.2 + 必要なsoをinitrdに含める
ldd /bin/bash | awk '{print $3}' | grep -v '^$' | while read lib; do
    cp --parents "$lib" initrd_src/
done
cp /lib64/ld-linux-x86-64.so.2 initrd_src/lib64/
```

---

## デバッグ

### GDB デバッグ

```bash
# 別ターミナルでQEMUをデバッグモードで起動
make debug

# GDBコマンド例
(gdb) info registers
(gdb) x/10i $rip
(gdb) break kernel_entry
(gdb) break proc_exit
(gdb) continue
```

### シリアルログ

起動時のログは COM1 (115200bps, 8N1) に出力されます:

```bash
# 実機の場合
screen /dev/ttyUSB0 115200

# minicom
minicom -b 115200 -D /dev/ttyUSB0
```

---

## 拡張方法

### 新しいシステムコールを追加

```c
// proc/syscall_impl.c に追加
static uint64_t sys_mysyscall(uint64_t a1, ...) {
    // 実装
    return 0;
}

// syscall_init() に登録
syscall_register(SYS_mynum, sys_myyscall);
```

### 新しいファイルシステムを追加

```c
// fs/myfs.c を作成
static filesystem_t g_myfs = {
    .name     = "myfs",
    .mount    = myfs_mount,
    .get_root = myfs_get_root,
};

// 初期化時に登録
vfs_register_fs(&g_myfs);
vfs_mount(device, "/mnt/myfs", "myfs", 0);
```

### 新しいデバイスドライバを追加

```c
// drivers/mydriver.c を作成
static const struct vnode_ops g_mydev_ops = {
    .read  = mydev_read,
    .write = mydev_write,
    .ioctl = mydev_ioctl,
};

static device_t g_mydev = {
    .name   = "mydev",
    .devno  = MAKEDEV(240, 0),
    .type   = VN_CHR,
    .ops    = &g_mydev_ops,
};

// drivers_init() から呼ぶ
dev_register(&g_mydev);
```

---

## ライセンス

MIT License - 自由に改変・再配布可能です。
