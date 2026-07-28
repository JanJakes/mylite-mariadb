# Ownerless Cross-Platform Filesystems

## Problem

The completed ownerless concurrency surface is intentionally limited to Linux
on validated local ext4, XFS, tmpfs, and overlay filesystems. macOS and Windows
builds must gain the same fail-closed ownerless lifecycle instead of exposing a
compile-only capability or relying on POSIX behavior that is absent on Windows.

The current directory gate also returns generic `MYLITE_ERROR` after preparing
the database directory when a filesystem is not allowlisted. Callers need a
stable, explicit result that distinguishes an unsupported filesystem from
corruption, I/O failure, lock contention, or invalid API use.

## Base And Source Findings

The implementation remains based on MariaDB 11.8.6,
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

Relevant upstream-derived source:

- `mariadb/mysys/my_mmap.c` implements POSIX `mmap()`/`msync()` and Windows
  `CreateFileMapping()`/`MapViewOfFile()`/`FlushViewOfFile()`.
- `mariadb/mysys/my_lock.c` implements POSIX record locks and Windows
  `LockFileEx()`/`UnlockFileEx()`, including nonblocking lock conflict.
- `mariadb/mysys/my_pread.c` implements positioned I/O on POSIX and Windows.
- `mariadb/mysys/my_sync.c` implements file synchronization and treats
  directory synchronization as a platform-dependent operation.
- `mariadb/libmysqld/CMakeLists.txt` builds the static embedded target as
  `libmariadbd.a` on Unix and `mysqlserver.lib` on Windows.
- `mariadb/cmake/libutils.cmake` merges MariaDB's component archives into that
  embedded target. The MSVC path must pass every component archive to
  `lib.exe`; merely attaching static target dependencies produces an
  incomplete carrier archive.

Relevant MyLite source:

- `packages/libmylite/src/ownerless_probe.cc` probes `MAP_SHARED`, byte-range
  locks, lock release on process exit, grow/remap, wait/wake, and process
  identity, but filesystem identification is Linux-only.
- `packages/libmylite/src/ownerless_process_registry.cc` derives process
  identity from Linux `/proc` start time and boot ID.
- `packages/libmylite/src/database.cc` exposes ownerless capabilities only on
  Linux and performs filesystem admission after database-directory
  preparation.
- `packages/libmylite/src/ownerless_page_log.cc` and
  `ownerless_tablespace_replay.cc` use POSIX file APIs directly.
- `docs/specs/ownerless-directory-platform-gate/specs.md` deliberately left
  macOS and Windows backends out of scope.

## Supported Boundary

This slice admits ownerless read/write and shared read-only modes on:

| Platform | Initially admitted filesystem | Required properties |
| --- | --- | --- |
| Linux | ext4, XFS, tmpfs, overlay | Existing release-gate proof |
| macOS | local APFS | Local device, record locks, shared mappings, process identity |
| Windows | local NTFS | Fixed/local volume, byte-range locks, shared mappings, process identity |

The platform names describe the CI-validated native desktop/server targets.
macOS validation uses Apple Clang and APFS. Windows validation uses 64-bit
Windows with the native CMake/MSVC toolchain and NTFS.

The following remain unsupported until separately validated:

- NFS, SMB/CIFS, AFP, WebDAV, and other remote filesystems;
- FUSE filesystems;
- HFS/HFS+, ReFS, FAT, and exFAT;
- unknown filesystem types and local filesystems not named above;
- 32-bit targets.

An ordinary exclusive MyLite open does not require ownerless filesystem
semantics and remains outside this gate.

## Public Error Contract

Add stable public result codes:

```c
MYLITE_UNSUPPORTED_FILESYSTEM
MYLITE_UNSUPPORTED_PLATFORM
```

`mylite_open()` returns `MYLITE_UNSUPPORTED_FILESYSTEM` when ownerless or
shared-readonly mode targets a known but unadmitted, remote, or unknown
filesystem. It returns `MYLITE_UNSUPPORTED_PLATFORM` when the build has no
ownerless backend for its OS/architecture.

These results are available even though a failed `mylite_open()` returns no
database handle. Probe failure on an otherwise admitted filesystem remains a
separate ownerless platform failure because it means the required runtime
semantics could not be proven.

For a new database, filesystem admission examines the nearest existing parent
before creating the MyLite directory. An unsupported filesystem therefore does
not leave a partially initialized database. For an existing database, the
directory itself is inspected.

## Platform Abstraction

First-party ownerless code gains a narrow internal OS layer for:

- opening, closing, sizing, syncing, and positioned file I/O;
- shared file mapping, unmapping, and flushing;
- nonblocking shared/exclusive byte-range locks;
- current process ID, process creation identity, and liveness;
- filesystem identity, volume identity, and local/remote classification;
- process spawning used by the cross-platform integration test.

Call sites use the explicit ownerless platform API rather than POSIX-name
preprocessor aliases. This keeps Windows portability shims from rewriting C++
standard-library members such as `std::istream::read`.

Linux uses its native POSIX interfaces. macOS uses POSIX file and mapping
interfaces, with one-byte ownerless range-lock records mapped to handle-scoped
`flock()` sidecar files because Darwin `F_SETLK` locks are process-scoped and
an unrelated close releases them. macOS process identity uses the kernel
process start timestamp, and boot identity uses the kernel boot time. Windows
uses native file mappings and byte-range locks; process identity uses
`GetProcessTimes()` creation time and liveness uses a process handle.

The shared coordination file format remains fixed-width and
platform-independent. No native pointer, handle, `std::atomic`, or OS structure
is persisted.

Linux keeps futex wait/wake. macOS and Windows may use the existing bounded
adaptive-backoff wait path because wake acceleration is a performance
property, not a correctness requirement. A future platform-specific fast wait
backend must be process-shared before it can replace the backoff path.

## Filesystem Admission

Add a structured internal filesystem probe with:

- platform kind;
- normalized filesystem kind and diagnostic name;
- stable volume/device identity;
- local versus remote classification;
- admitted versus unsupported result.

Detection uses:

- Linux `statfs().f_type` plus `st_dev`;
- macOS `statfs().f_fstypename` plus `st_dev`;
- Windows `GetVolumePathNameW()`, `GetDriveTypeW()`, and
  `GetVolumeInformationW()`.

The ownerless proof marker is advanced to include:

```text
format=2
platform=<linux|macos|windows>
filesystem=<ext4|xfs|tmpfs|overlay|apfs|ntfs>
volume_identity=<unsigned decimal>
required_primitives=1
process_identity=1
```

Legacy format-1 markers are reprobed. A marker is reusable only when platform,
filesystem kind, volume identity, required primitives, and process identity all
match the current directory.

## Windows Runtime Details

The Windows backend uses handle-scoped `LockFileEx()` locks. Locks are released
when the owning handle closes or the process exits. Shared file mappings are
backed by the coordination file through `CreateFileMapping()` and
`MapViewOfFile()`. `FlushViewOfFile()` plus `FlushFileBuffers()` supplies the
mapping/file durability boundary.

Windows normally opens writable InnoDB files without `FILE_SHARE_WRITE`,
enforcing MariaDB's single-server exclusion at the native file handle. While
MyLite's ownerless-managed file-lock policy is active, the patched InnoDB open
paths add `FILE_SHARE_WRITE` so independently coordinated processes can hold
the same native data files open. Ordinary MariaDB/MyLite opens retain the
upstream sharing mode; this exception is scoped to ownerless coordination.

Positioned reads and writes must remain 64-bit and must not depend on a shared
CRT file position. File and mapping wrappers preserve the existing ownerless
page-log ordering.

Windows child-process tests relaunch the same test executable with an explicit
child mode. They do not inherit a live `mylite_db` handle.

## macOS Runtime Details

APFS uses the existing POSIX shared mapping and file-sync paths. Darwin's
process-scoped `F_SETLK` contract is not used as an ownerless lock anchor.
Every current ownerless range is exactly one byte, so the macOS platform layer
maps each `(lock file, byte offset)` to a persistent sidecar file and applies
shared or exclusive `flock()` to its own descriptor. This preserves range
independence, last-close and process-exit release, and unrelated-close
isolation. Sidecars live beside the owning coordination file under the MyLite
directory and are safe to retain across clean close.

The process registry uses process start time rather than PID alone, preventing
PID reuse from making a stale slot appear live.

## Compatibility And Lifecycle Impact

SQL and native InnoDB formats are unchanged. The internal concurrency layout
may contain macOS range-lock sidecar directories; they are non-SQL
coordination state and remain within the MyLite database directory. A closed
directory remains portable between supported platforms subject to MariaDB
native file-format compatibility.

The change affects only ownerless/shared-readonly admission, platform
capabilities, internal coordination I/O, and diagnostics. Unsupported
filesystems fail before MariaDB startup and before ownerless runtime files are
created or reused.

No durable state is placed outside the MyLite directory. Platform probe scratch
files and their lock sidecars are created under the target directory and
removed before open continues.

## Build, Dependency, And Size Impact

- No new third-party dependency is introduced.
- The macOS embedded archive uses the existing Unix baseline build wrapper.
- The Windows embedded archive path and native system-library linkage are made
  explicit in CMake. Its build wrapper fetches and verifies WolfSSL commit
  `59f4fa568615396fbf381b073b220d1e8d61e4c2`, the bundled dependency gitlink
  pinned by MariaDB 11.8.6 and omitted from the initial source import.
- Platform-only code is selected at compile time.
- Final archive sizes are recorded per platform by CI; Linux size regression
  remains subject to the existing production measurement.

## Test Plan

1. Unit-test filesystem classification for every admitted and explicitly
   rejected type on all host builds.
2. Hook-test exact `MYLITE_UNSUPPORTED_FILESYSTEM` behavior before directory
   creation; prove ordinary opens ignore the ownerless-only hook.
3. Test proof format-2 creation, reuse, and invalidation on filesystem or volume
   identity change.
4. Run the complete existing Linux ownerless release gates.
5. Add a portable cross-process embedded smoke test that:
   - opens one directory from two independently launched processes;
   - performs concurrent InnoDB writes and peer reads;
   - closes/reopens and verifies durability;
   - kills a writer and verifies dead-process cleanup;
   - reports the detected admitted filesystem.
6. Run the smoke test on GitHub macOS/APFS and Windows/NTFS runners.
7. On macOS, run a focused platform-capability primitive gate and a focused SQL
   subset that does not depend on Linux-only namespace tools.
8. On Windows, run native platform primitive coverage plus the portable
   cross-process smoke test.
9. Run formatting, tidy/static checks where supported, production-build policy,
   `git diff --check`, and Linux archive measurement.

## Acceptance Criteria

- `MYLITE_CAP_OWNERLESS_RW` and `MYLITE_CAP_SHARED_READONLY` are exposed by
  embedded Linux, macOS, and 64-bit Windows builds.
- Linux release behavior remains green.
- macOS/APFS and Windows/NTFS each pass an actual two-process InnoDB
  write/read/reopen/dead-writer test in CI.
- Unsupported, remote, FUSE, and unknown filesystem classifications return
  `MYLITE_UNSUPPORTED_FILESYSTEM`.
- Unsupported filesystem rejection happens before creating a new database
  directory.
- A build without a supported ownerless platform backend returns
  `MYLITE_UNSUPPORTED_PLATFORM`, not `MYLITE_MISUSE`.
- Proof metadata cannot be reused across a platform, filesystem, or volume
  change.
- Docs and the compatibility matrix state the precise platform/filesystem
  boundary without implying network-filesystem support.

## Risks And Review Questions

- Windows and macOS do not provide Linux OFD locks with identical APIs.
  Handle/descriptor-close isolation must be proven rather than inferred.
- Filesystem names identify an admitted implementation family, not every mount
  option or storage appliance. The directory primitive probe remains mandatory.
- Cloud CI runners prove their host versions and filesystems; broader OS-version
  claims require additional matrix entries.
- Power-loss testing is distinct from process-crash testing. This slice
  preserves the existing durability protocol but does not claim a new
  hardware-power-loss certification.
