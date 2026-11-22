# MFS4 Phase 0 監査記録

- 監査日: 2026-11-22
- 対象: eclairdosesleekman/minix
- 対象branch: master
- 監査時HEAD: 8883d8e441bcb2ffcb8b95afc92e738f42629881
- 設計書の監査基準: 8a2e402230074c9dbb51149b3992ae9db93a4c37
- 設計書: MFS4 詳細設計書 v1.0（2026-11-22）
- 状態: **ソース監査は完了。Phase 0 の受け入れは未完了**（MFS3 golden testを対象MINIX環境で実行できていない）。

## 1. 結論

| Phase 0項目 | 結果 | 判定 |
|---|---|---|
| x86_64 PAGE_SIZE | 4096 bytes。MFSのinclude経路から確認 | 確定。設計の4 KiB blockと整合 |
| 64-bit ABI | off_t/time_tは64-bit、uid_t/gid_t/nlink_tは32-bit、dev_t/ino_tは64-bit。legacy MFS inodeは別幅 | 確定。disk codecは固定幅整数を使う |
| VFS capability flags | RES_64BITはread/write/getdents/truncateで消費。RES_HASPEEKはmmap経路で消費 | 確定。MFS4が両実装を満たした場合のみ返却する |
| device flush | bdev_ioctlはerrorを返せるが、LMFS/VFS/一部driverの上位経路でerrorが失われる | **未解決。durabilityのrelease blocker** |
| MFS3 behavior / golden | dirty mountのRO fallbackと既存image hash testを確認。testは実行できていない | **未完了** |

設計書24節の完了条件「PAGE_SIZE、VFS flag、flush APIとMFS3動作を確定」のうち、PAGE_SIZE・VFS flag・MFS3の現行動作はソースから確定した。一方、flush error propagationには実装上の欠落があり、MFS3 golden testの実行結果も未取得であるため、この記録をもってPhase 0完了とはしない。

## 2. branch / source drift

設計書の基準commitから監査時HEADまでは3 commit進んでいる。GitHub compareでは変更ファイルは次の2件のみで、今回の監査対象（MFS、VFS、libminixfs cache、block device API/storage drivers）に差分はない。

- minix/fs/ext4/README.md
- minix/fs/ext4/link.c

したがって今回の対象箇所は設計書のcommit固定監査をそのまま適用できる。以降は監査時HEADを新しいPhase 0基準とする。

## 3. PAGE_SIZE / build include chain

MFSの super.c は machine/param.h と machine/vmparam.h を直接includeする。amd64 vmparam.h は PAGE_SHIFT=12、PAGE_SIZE=(1 << PAGE_SHIFT) と定義するため、x86_64実効PAGE_SIZEは4096 bytesである。MFS read_super() は filesystem block size が PAGE_SIZE 未満の場合に拒否する。

**決定:** MFS4の4096-byte blockは現行x86_64のPAGE_SIZE条件を満たす。少なくとも現在のbranchではPAGE_SIZE理由によるformat変更は不要。

## 4. ABI / VFS capability

確認できたx86_64型幅:

| 型 | 定義/幅 |
|---|---|
| off_t | signed 64-bit（__int64_t） |
| time_t | signed 64-bit（__int64_t） |
| uid_t / gid_t | unsigned 32-bit |
| nlink_t | unsigned 32-bit |
| ino_t / dev_t | unsigned 64-bit |
| zone_t / block_t / bit_t | unsigned 32-bit |

fsdriver_nodeのfile sizeおよびread/write/truncate callbackはoff_tを使う。ただしこれだけでMFS内部またはMFS3 disk inodeが64-bit化されるわけではない。MFS3互換codecとMFS4 codecの分離、およびMFS3側のdown-conversion range checkが必要である。

VFS側の実際のcapability consumerは以下のとおり。

- RES_64BIT: REQ_READ/REQ_WRITEのposition、REQ_GETDENTSのposition、REQ_FTRUNCの範囲チェック。
- RES_HASPEEK: mmap用dupvm経路でpeek非対応filesystemを拒否。
- MFS現行mount callbackはRES_NOFLAGSを返す。

**決定:** MFS4で64-bit size/offset経路とpeek/bpeekの双方を実装・試験するまではRES_64BIT/RES_HASPEEKを返さない。MFS3の返却flagsは変更しない。x86_64 IPC messageはheader 8 bytes + payload 88 bytes = 96 bytesで、_ASSERT_messageによるcompile-time size assertionがある。protocol拡張時もこれを維持する。

## 5. device flush監査

MFS4のdurability要件は、LMFS bufferをdeviceへ書くだけでなく、DIOCFLUSHでdevice volatile write cacheまでflushすること。現行実装では、下層にはerrorを返すAPIがある一方、上位へ成功/失敗が届かない経路がある。

### 5.1 error propagationの欠落

1. bdev_ioctl()はintを返し、block driverのioctl statusを呼出側へ返す。
2. lmfs_flushdev() / lmfs_flushall()はvoid。lmfs_flushdev()はrw_scattered(..., WRITING)を呼ぶが結果を呼出側へ返さない。したがってMFS4のjournal barrierでhome/data cache write failureを確実に検出できない。
3. MFSのfs_sync()はdirty inodeをrw_inode()で書き、その後lmfs_flushall()を呼ぶ。fsdriverのfdr_sync callbackもvoid。
4. VFS do_fsync()はreq_sync()の戻り値を捨てている。FS側がREQ_SYNCでerrorを返すようになっても、現状のVFS fsync()はそれを呼出し元へ伝播しない。
5. VND driverのDIOCFLUSHはfsync(state.fd)の戻り値を無視してOKを返す。下位file sync失敗を隠してしまう。

**決定 / blocker:** MFS4 transactionを実装する前に、LMFS flushのerror-reporting barrier、FS sync callbackの戻り値、VFS do_fsync()のreq_sync error propagationを一体で設計・修正する。VNDもfsync errnoをblock ioctl statusへ変換して返す必要がある。これらが解決するまでdurability保証を宣言しない。

### 5.2 x86_64でbuild対象となるstorage driver

| driver | DIOCFLUSH監査結果 | MFS4 RW mount上の扱い |
|---|---|---|
| AHCI | 実装あり。write-cache flush commandを発行。device state / barrier flag / command結果を検査 | 成功を確認できるdeviceのみ可。非対応時はfail-closed |
| virtio-blk | 実装あり。VIRTIO_BLK_F_FLUSHなしはEOPNOTSUPP | featureなしならRW mount拒否 |
| at_wini | ATA FLUSH CACHE実装あり。ATAPIはEINVAL | 成功確認できるATA deviceのみ可 |
| FBD | 未知ioctlを下位block driverへ転送 | 下位driverの実装/結果に従う |
| filter | 未知ioctlはENOTTY | MFS4 RW mount不可（専用対応が必要） |
| VND | fsync()結果を無視してOKを返す | 修正とerror伝播試験が終わるまで不可 |
| floppy / memory | DIOCFLUSH実装を確認できず、専用ioctl以外は未対応 | MFS4 RW mount不可 |

x86_64 storage MakefileではAHCI、FBD、filter、virtio_blkが条件付きでbuildされ、VND、at_wini、floppyも列挙される。memory/ramdiskは追加の非永続media経路として扱う。

**実装方針:** MFS4 RW mount時に実deviceへのDIOCFLUSH probeを行い、EOPNOTSUPP/ENOTTY/その他errorを成功扱いしない。RO mountは別ポリシーで扱う。flush probeだけでは継続中のI/O error reporting欠落を補えないため、5.1の修正も必須。

## 6. MFS3 compatibility / golden tests

現行MFS3 sourceで確認した動作:

- read_super()はSUPER_V3を認識し、filesystem block sizeがPAGE_SIZE未満の場合は拒否する。
- RW mount要求時にMFSFLAG_CLEANが落ちていればdeviceをROで開き直し、read-only mountへfallbackする。
- RW mount開始時にclean flagを落とし、unmount時はsync後にclean flagを立てる。
- MFS mount callbackは現状RES_NOFLAGSを返す。

既存のMFS3 golden testはminix/tests/testmfs.sh。runnerのalltestsにはmfsが含まれ、rootから実行するとsu binでUID 3/GID 7として実行する。testmfs.sh自身もUID 3/GID 7を要求する。

ただし現行testmfs.shが検証するのは、proto入力から4096-byte block / 15000 blocks / 6000 inodesのimageを作り、その全imageのSHA-1が 98bcafa04cb1eb75b7add6c95eb587c37f5050e0 と一致すること。これはmkfs image生成のgolden testであり、設計書23.1が要求するmount/read/write/create/rename/link/unlink/truncate/stat/utime、disk byte layout、dirty mount RO fallbackを一通り検証するsuiteではない。

推奨実行コマンド（x86_64 MINIX test target内）:

    cd /usr/tests/minix-posix
    ./run -t mfs

**実行結果:** 未実行。今回の作業環境はLinux hostで、QEMUおよびMINIX用bmake/nbmakeが利用可能でないため、対象MINIX runtime上のtestmfs.shおよびMFS3 mount regressionを実行できなかった。成功したとは扱わない。

## 7. Phase 0 exit criteria / 次の作業

| 条件 | 状態 | 次の作業 |
|---|---|---|
| PAGE_SIZE確定 | PASS | 4 KiBを採用 |
| ABI / RES_64BIT / RES_HASPEEK消費経路確定 | PASS | MFS3 flags維持、MFS4対応後にのみcapability返却 |
| device flush APIがdurable/error-reporting可能 | FAIL | LMFS → MFS fsdriver → VFS → bdev/driverのerror伝播を修正。VNDを修正 |
| MFS3 golden regression実行 | NOT RUN | x86_64 MINIX/QEMU CIまたは実機で実行しログを保存 |
| MFS3動作・disk byte layout regression | PARTIAL | mount/read/write/namespace/truncate/stat/utime/dirty-RO fallback用golden testを追加 |

Phase 0は**部分完了**。PAGE_SIZEとVFS ABI監査の結果は確定したが、flush contractの不備とgolden runtime test未実行が残る。これらを解消・検証してからPhase 1（common inode + version codec分離）へ進む。

## 8. 参照ソース（監査時HEADに固定）

- [amd64 PAGE_SIZE](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/sys/arch/amd64/include/vmparam.h#L55-L57)
- [MFS super.c include / PAGE_SIZE check](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/fs/mfs/super.c#L19-L21)
- [MFS read_super block-size validation](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/fs/mfs/super.c#L286-L315)
- [system ABI types](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/sys/sys/types.h)
- [time_t width macro](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/sys/sys/common_ansi.h#L1-L5)
- [VFS capability flags](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/include/minix/vfsif.h)
- [VFS RES_64BIT read/write/getdents/trunc consumers](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/servers/vfs/request.c#L261-L327)
- [VFS RES_64BIT read/write consumer](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/servers/vfs/request.c#L833-L858)
- [VFS RES_HASPEEK mmap consumer and fsync](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/servers/vfs/misc.c#L297-L321)
- [fsdriver callback signatures](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/include/minix/fsdriver.h)
- [LMFS flushdev implementation](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/lib/libminixfs/cache.c#L1137-L1170)
- [bdev_ioctl synchronous error return](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/lib/libbdev/bdev.c#L354-L371)
- [DIOCFLUSH ioctl number](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/include/sys/ioc_disk.h#L11-L18)
- [MFS sync implementation](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/fs/mfs/misc.c)
- [MFS mount clean flag and returned flags](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/fs/mfs/mount.c#L28-L80)
- [VND DIOCFLUSH](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/drivers/storage/vnd/vnd.c#L489-L495)
- [AHCI DIOCFLUSH](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/drivers/storage/ahci/ahci.c#L2710-L2715)
- [virtio-blk flush support check](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/drivers/storage/virtio_blk/virtio_blk.c)
- [AT wini DIOCFLUSH](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/drivers/storage/at_wini/at_wini.c#L2313-L2326)
- [FBD ioctl forwarding](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/drivers/storage/fbd/fbd.c#L187-L214)
- [filter ioctl policy](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/drivers/storage/filter/main.c#L200-L229)
- [MFS3 golden test](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/tests/testmfs.sh)
- [MFS test runner registration](https://github.com/eclairdosesleekman/minix/blob/8883d8e441bcb2ffcb8b95afc92e738f42629881/minix/tests/run#L21-L35)

