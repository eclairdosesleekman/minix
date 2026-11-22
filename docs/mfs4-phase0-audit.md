# MFS4 Phase 0 監査記録（再監査）

- 監査日: 2026-11-22
- 対象: `eclairdosesleekman/minix`
- 対象branch: `master`
- 監査時HEAD: `c9ee52606c6beb1aa2e0f06947826223896da51f`
- 設計書: MFS4 詳細設計書 v1.0（2026-11-22）
- 設計書のソース監査基準: `8a2e402230074c9dbb51149b3992ae9db93a4c37`
- 状態: **静的ソース監査は完了。Phase 0の実行受け入れは未完了**（x86_64 MINIX上のMFS3 golden/runtime regression未実行）。

## 1. 結論

設計書24節のPhase 0完了条件は「PAGE_SIZE、VFS flag、flush APIとMFS3動作を確定」である。現在のmasterを基準に再監査し、前回残っていたLMFS→MFS/fsdriver→VFSのwriteback error伝播はPR #233でmasterに取り込まれたことを確認した。

| Phase 0項目 | 現状 | 判定 |
|---|---|---|
| x86_64 PAGE_SIZE / block size条件 | amd64 `PAGE_SHIFT=12`、`PAGE_SIZE=4096`。MFS3はblock size < PAGE_SIZEを拒否 | **PASS（静的）** |
| ABI / VFS capability flags | 型幅とRES_64BIT / RES_HASPEEKの実consumerを確認 | **PASS（静的）** |
| LMFS writeback error propagation | error-reporting APIとfsdriver callbackを追加済み。VFS sync/fsync/unmountまで伝播 | **PASS（静的）** |
| device DIOCFLUSH | bdev ioctlはstatusを返す。driverごとの対応・非対応条件を確認 | **PASS（静的契約の確定）** |
| MFS3 clean/dirty mount動作 | dirty RW要求のRO fallback、mount時dirty化、成功unmount時clean化を確認 | **PASS（ソース）** |
| MFS3 golden test実行 | 既存testmfs.shのimage hash testは未実行 | **NOT RUN** |
| MFS3 mount/read/write等の回帰 | 設計書23.1の統合回帰は未実行。現testmfs.shにも含まれない | **NOT RUN / GAP** |

**総合判定: Phase 0は静的監査部分を完了したが、受け入れは未完了。** 対象MINIX x86_64/QEMU環境で既存golden testとMFS3回帰を実行し、ログ・結果を保存するまでPhase 1へ進まない。

## 2. branch / source drift

設計書の監査基準commit `8a2e402230074c9dbb51149b3992ae9db93a4c37` と現行masterを比較すると、masterは14 commit aheadである。差分にはext4関連変更のほか、今回のPhase 0に関係するMFS、libminixfs cache、fsdriver、VFS、VNDの変更が含まれる。今回の報告は古い監査結果をそのまま再掲せず、`c9ee52606c6beb1aa2e0f06947826223896da51f` の現行ソースで再確認した。

PR #232（Phase 0 source audit）とPR #233（flush error propagation）はどちらもmasterへmerge済み。PR #233のmerge commitは上記HEADである。

## 3. PAGE_SIZE / MFS3 block-size gate

`sys/arch/amd64/include/vmparam.h` は `PAGE_SHIFT=12`、`PAGE_SIZE=(1 << PAGE_SHIFT)` と定義するため、amd64のcompile-time PAGE_SIZEは4096 bytesである。MFS `super.c` は `machine/param.h` と `machine/vmparam.h` をincludeし、MFS3 superblock読込み時に `s_block_size < PAGE_SIZE` を拒否する。さらに512-byte sectorの倍数、superblock/inodeが収まるblockであること等を検査する。

**決定:** 設計書の4 KiB blockは現行amd64 PAGE_SIZEと整合する。PAGE_SIZEのruntime値はMINIX上での実行確認をしていないが、対象arch headerのcompile-time定義は4096で確定した。

## 4. ABI / VFS flags

確認したx86_64型幅:

| 型 | 幅 |
|---|---:|
| `off_t` | signed 64-bit |
| `time_t` | signed 64-bit |
| `uid_t` / `gid_t` | 32-bit |
| `nlink_t` | unsigned 32-bit |
| `ino_t` / `dev_t` | unsigned 64-bit |
| `zone_t` / `block_t` / `bit_t` | unsigned 32-bit |
| `block64_t` | unsigned 64-bit |

OS ABIが64-bitでもMFS3 inodeは自動的に拡張されない。現行MFS in-memory inodeはmode/nlinks/uid/gidが各16-bit、sizeはsigned 32-bit、atime/mtime/ctimeはunsigned 32-bit、zone pointerは32-bitである。MFS3のdisk inodeもlegacy fixed-width formatのまま扱う。MFS3 codecへLP64 native structを持ち込んではならない。MFS4では固定幅little-endian codecを使い、MFS3への値変換は範囲検査してoverflowを拒否する。

VFS flag定義とconsumer:

- `RES_64BIT`: `req_readwrite_actual()`、`req_getdents_actual()`、`req_ftrunc()` がflagを見てoffset/range制限を切り替える。
- `RES_HASPEEK`: VFS `dupvm()` のmmap経路でpeek非対応filesystemを拒否する。
- 現行MFS3 `fs_mount()` は `RES_NOFLAGS` を返す。

**決定:** MFS3の返却flagsは変更しない。MFS4は64-bit read/write/getdents/truncateのend-to-end実装とpeek/bpeekの有効性を確認した後にのみ `RES_64BIT | RES_HASPEEK` を返す。statの結果幅はこのflag確認だけで保証されたとはみなさず、VFS/libc統合試験で別途確認する。

## 5. flush API / durability contract

### 5.1 LMFS writeback error propagation（PR #233 merge済み）

現行masterで確認した変更:

- `lmfs_flushall_err()`、`lmfs_flushdev_err()`、`lmfs_bflush_err()` がstatusを返す。block-size変更・buffer pool resizeにもerror-return variantがある。
- scattered writebackはdevice I/O error、short write、不正なtransfer countを検出する。完全に書けたblockのみclean化し、未完了bufferはdirtyのまま保持する。
- dirty bufferをevict/invalidateする前にwritebackし、失敗時はdirty dataを破棄しない。
- MFS `fs_sync()` はinodeを先にwritebackし、その後 `lmfs_flushall_err()` のerrorを返す。
- fsdriverにerror-return callback（sync/unmount/bflush）が追加され、MFS tableで使用する。
- VFS `do_sync()` / `do_fsync()` は `req_sync()` のerrorを呼出元へ返す。VFS unmountもREQ_UNMOUNT失敗時にmountをdetachしない。
- VNDのDIOCFLUSHは下位fileの `fsync()` errorを返す。

旧void callback/APIは互換wrapperとして残る。MFS4のdurability pathではerror-return variantを使用し、legacy void APIへ戻してはならない。

### 5.2 device flush ioctl

`minix/include/minix/bdev.h` の同期 `bdev_ioctl()` はintを返す。`minix/lib/libbdev/bdev.c` はdriver reply statusをそのまま呼出元に返す。従ってDIOCFLUSHはLMFS buffer writebackとは別レイヤーのbarrierとして利用できる。

MFS4 transaction barrierの必須順序は次のとおり。

1. 対象deviceのLMFS dirty bufferをerror-reporting APIでwritebackする。
2. `bdev_ioctl(fs_dev, DIOCFLUSH, ...)` を実行し、device volatile write cacheのflush完了を確認する。
3. どちらか一方でも失敗・未対応なら成功扱いせず、MFS4 RW mount/transactionをfail-closedにする。

PR #233はLMFS writeback errorを伝播させる変更であり、MFS3の通常syncへDIOCFLUSHを追加したものではない。既存MFS3をflush非対応mediaで新たに拒否しないため、MFS3の通常syncは従来どおりLMFS cache writebackまでである。MFS4は上記device barrierを別途実装する必要がある。

### 5.3 x86_64 storage driver matrix

| driver | DIOCFLUSHの現状 | MFS4 RWの扱い |
|---|---|---|
| AHCI | 対応。device state確認後 `gen_flush_wcache()`。command failureを返す | probeと各barrierの成功必須 |
| virtio-blk | 対応。hostに `VIRTIO_BLK_F_FLUSH` がなければ `EOPNOTSUPP` | featureなしはRW拒否 |
| at_wini | ATA FLUSH CACHE対応。ATAPIは `EINVAL` | 成功するATA deviceのみ |
| FBD | 未知ioctlを下位block driverへforwardしstatusを返す | 下位driver次第。forward成功を事前仮定しない |
| VND | 下位file `fsync()` のerrorを返す（PR #233） | fsync成功を必須 |
| filter | 未知ioctlは `ENOTTY` | 専用対応なしではRW拒否 |
| floppy | DIOCFLUSH handlerを確認できない | 対応扱いしない |
| memory / ramdisk | ioctlは `MIOCRAMSIZE` のみ受理し、それ以外は `EINVAL` | volatile mediaとしてdurability対象外 |

x86_64 storage MakefileはAHCI/FBD/filter/virtio-blk/VND/at_wini/floppyを列挙し、ramdisk/memoryも構成する。上表はsource-level auditであり、実デバイスでのflush動作・timeout・電源断耐性を実測したものではない。

**決定:** MFS4はmount時に実deviceへのDIOCFLUSH probeを実装し、非対応・未確認・I/O errorを成功扱いしない。flush実装があるdriverでも、実device/host featureで成功を確認できない限りRW durabilityを主張しない。

## 6. MFS3 current behavior / compatibility

現行MFS3 sourceから確認:

- superblock parserはMFS V3を認識する。filesystem block sizeがPAGE_SIZE未満ならrejectする。
- RW mount要求で `MFSFLAG_CLEAN` が落ちている場合、deviceをread-onlyで開き直し、警告を出してRO mountへfallbackする。
- RW mount開始時にclean flagを落としてsuperblockを書き戻す。
- unmountではinode/cache syncが成功した後にclean flagを書き戻す。writeback失敗時はunmountを中止する。clean marker write/close error時はdirty状態の復元を試み、復元不能ならread-onlyへfail-closedする。
- cache invalidationは正常なwriteback/close完了後にのみ行う。
- mount callbackは引き続き `RES_NOFLAGS` を返す。

MFS3のdisk inode/in-memory inodeは16/32-bit legacy widthであり、64-bit kernel ABIとは別である。特に現行MFS3は32-bit size/time limitを持つ。自動timestamp更新は `clock_time(NULL)` による秒単位であり、MFS3でnanosecond精度を仮定しない。MFS4のseconds/nanoseconds保持はMFS4 codec/operation実装時の要件とする。

## 7. MFS3 golden / regression test状況

現行 `minix/tests/testmfs.sh` はmkfs.mfs image生成のgolden testである。

- block size: 4096 bytes
- blocks: 15000
- inodes: 6000
- expected SHA-1: `98bcafa04cb1eb75b7add6c95eb587c37f5050e0`
- proto treeを生成し、mkfs.mfsでimageを作り、image全体のSHA-1を比較する。
- test runnerのalltestsに `mfs` が登録され、`./run -t mfs` で実行される。root実行時はscriptをbin userで起動し、testmfs.shはUID 3/GID 7を要求する。

**重要:** このtestはmkfs image byte-for-byte goldenの確認であり、MFS serverによるmount/read/write/create/rename/link/unlink/truncate/stat/utime、dirty mount RO fallback、disk inode/dirent/bitmapの実更新を検証しない。設計書23.1が要求するMFS3 regression一式は現テストに不足している。

### 実行結果

未実行。実行環境にMINIX x86_64 runtime、`bmake`/`nbmake`、QEMU、MINIXの`mkproto`/`mkfs.mfs`がなく、対象環境を起動してtest runnerを実行できなかった。GitHub Actions workflowも見当たらず、Actions runは0件であった。従ってgolden testがpassしたとは記録しない。

## 8. Phase 0 exit criteria / remaining work

| 条件 | 状態 | 必要作業 |
|---|---|---|
| amd64 PAGE_SIZE = 4096確定 | PASS（compile-time source） | MINIX build/runtime valueは実行環境で補完 |
| ABI / RES_64BIT / RES_HASPEEK consumer確定 | PASS（source audit） | MFS4実装後にend-to-end large-file/peek tests |
| LMFS writeback error reporting確定 | PASS（PR #233 merge済み） | MFS4でerror-return APIを使う |
| DIOCFLUSH API/driver差異確定 | PASS（source audit） | MFS4 mount probe /実device fault test |
| MFS3 clean/dirty mount動作確定 | PASS（source audit） | dirty image RO fallback testを実行 |
| testmfs.sh golden hash test | NOT RUN | x86_64 MINIX target上で `./run -t mfs` |
| 設計書23.1のMFS3操作・disk layout regression | GAP / NOT RUN | mount/read/write/namespace/truncate/stat/utime、disk bytes、overflow rejectの回帰試験を整備・実行 |
| build / QEMU validation | NOT RUN | amd64 cross-build、QEMU boot、flush error/unsupported device test |

### Phase 0の終了判定

静的監査により、設計書の前回未確定点のうちPAGE_SIZE、ABI/VFS flag、LMFS error propagation、DIOCFLUSHのdriver policy、MFS3 source behaviorは確定した。しかしPhase 0内容に含まれるMFS3 golden testを未実行であり、設計書23.1のmount/runtime regressionも実施できていない。

**Phase 0: PARTIAL / NOT ACCEPTED。** 上記のtarget regressionを通過し、ログをこの記録へ追記するまではPhase 1へ進まない。MFS4 disk formatの追加変更・common inode/codec実装はこの監査branchに含めない。

## 9. Source references (audit HEAD pinned)

- [amd64 PAGE_SIZE](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/sys/arch/amd64/include/vmparam.h)
- [MFS superblock parser / PAGE_SIZE check](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/fs/mfs/super.c)
- [system ABI types](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/sys/sys/types.h)
- [amd64 time_t base type](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/sys/sys/common_ansi.h)
- [MFS in-memory inode](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/fs/mfs/inode.h)
- [VFS capability flags](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/include/minix/vfsif.h)
- [VFS RES_64BIT consumers](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/servers/vfs/request.c)
- [VFS RES_HASPEEK / fsync](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/servers/vfs/misc.c)
- [MFS mount / clean flag / unmount](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/fs/mfs/mount.c)
- [MFS fs_sync](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/fs/mfs/misc.c)
- [MFS callback table](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/fs/mfs/table.c)
- [fsdriver status callbacks](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/include/minix/fsdriver.h)
- [LMFS cache writeback](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/lib/libminixfs/cache.c)
- [bdev ioctl API](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/lib/libbdev/bdev.c)
- [AHCI DIOCFLUSH](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/drivers/storage/ahci/ahci.c)
- [virtio-blk DIOCFLUSH](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/drivers/storage/virtio_blk/virtio_blk.c)
- [AT wini DIOCFLUSH](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/drivers/storage/at_wini/at_wini.c)
- [FBD ioctl forwarding](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/drivers/storage/fbd/fbd.c)
- [filter ioctl policy](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/drivers/storage/filter/main.c)
- [VND DIOCFLUSH](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/drivers/storage/vnd/vnd.c)
- [memory driver ioctl](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/drivers/storage/memory/memory.c)
- [x86_64 storage driver build list](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/drivers/storage/Makefile)
- [MFS3 golden image test](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/tests/testmfs.sh)
- [MFS test runner](https://github.com/eclairdosesleekman/minix/blob/c9ee52606c6beb1aa2e0f06947826223896da51f/minix/tests/run)
