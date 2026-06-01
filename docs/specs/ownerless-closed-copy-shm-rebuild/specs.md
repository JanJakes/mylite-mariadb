# Ownerless Closed-Copy SHM Rebuild

## Problem Statement

Ownerless coordination keeps volatile live state in
`concurrency/mylite-concurrency.shm`. The file is inside the MyLite database
directory so closed-directory copies naturally carry it, but copied shared
memory must not be trusted as live coordination state for the copied directory.

The ownerless cross-process design already requires stale copied `.shm` rebuild
coverage. This slice makes that boundary explicit and tested: a closed
directory copy must reopen read/write even when the copied `.shm` header looks
otherwise valid, and the opener must rebuild the volatile segments before
allocating a process slot.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/os/os0file.cc:340` implements InnoDB file locking
  with `fcntl(F_SETLK)` and reports another `mariadbd` process as a sharing
  hazard. MyLite ownerless mode cannot rely on process-local InnoDB state being
  copied safely.
- `mariadb/storage/innobase/srv/srv0start.cc:1304` starts InnoDB through
  process-local runtime initialization. The copied MyLite `.shm` file must be
  treated as MyLite volatile coordination, not as part of MariaDB durable
  storage.
- `packages/libmylite/src/database.cc:4937` opens and prepares ownerless
  `.shm`, `.wal`, and `.ckpt` files under the database directory before the
  embedded runtime starts.
- `packages/libmylite/src/database.cc:5224` validates the `.shm` header and
  rebuilds volatile segments when layout, state, or live-process evidence is
  inconsistent.
- `packages/libmylite/src/database.cc:6834` validates the mapped `.shm` bytes
  before attaching them to the runtime.

## Design

Bind the `.shm` header to the actual shared-memory file identity by storing the
current `st_dev` and `st_ino` values from `fstat()` in reserved header bytes.
Header layout validation compares those values against the file being opened.
If the database directory was copied, the copied `.shm` file has a different
device/inode pair even though the database UUID still matches, so the opener
rebuilds `.shm` from durable metadata, WAL, and checkpoint state.

This is intentionally a volatile-state rule. The database UUID still survives
closed-directory copies, and native MariaDB/InnoDB files remain opaque durable
storage inside the copied directory. Open-directory copies remain unsupported
until a backup protocol coordinates reader pins, checkpoints, and page-version
retention.

## Compatibility Impact

Closed-directory copies become safer and more explicit: read/write reopen
discards copied `.shm` state even when the copied header is otherwise clean.
Read-only opens may still return busy if stale ownerless state requires
read/write recovery first, matching the existing shared read-only policy.

There is no SQL compatibility change. The slice only affects embedded
directory-lifecycle and ownerless shared-memory validation.

## Database-Directory Impact

No new files are added. The existing `concurrency/mylite-concurrency.shm`
header uses two previously reserved 64-bit fields for file identity. The header
format version is bumped so older `.shm` files rebuild rather than being
mistaken for current layout.

## Native Storage Impact

No native MariaDB storage format changes are made. The rebuild path may replay
ownerless page-version records into native tablespace files through the
existing no-live read/write recovery path before the copied `.shm` is attached.

## Public API Impact

No public API changes are made.

## Binary Size Impact

The product change is a few header fields and validation checks. No dependency
or profile-size change is introduced.

## Test And Verification Plan

- Update `mylite_embedded_open_close_test` shared-memory assertions to match
  the current 12-segment `.shm` layout, including the page-version pin
  registry.
- Add a closed-directory copy test that creates durable InnoDB data, copies the
  closed `.mylite` directory, opens the copy read/write, proves the copied
  `.shm` recovery generation advanced, and verifies the durable rows remain
  readable and writable.
- Run the focused embedded lifecycle test and the relevant CTest label.
- Run ownerless primitive/open-close-adjacent checks and static formatting
  checks before committing.

## Acceptance Criteria

- Copied closed directories reopen read/write with a rebuilt `.shm` header
  whose file identity matches the copied file.
- Copied durable SQL data remains readable and writable after the rebuild.
- Open/close layout assertions validate every current ownerless `.shm`
  segment.
- Compatibility and ownerless-concurrency docs record the closed-copy behavior
  and the continued open-copy limitation.

## Risks And Unresolved Questions

- Device/inode identity proves ordinary local closed-directory copies, but it
  is not a backup protocol and does not make open-directory copies safe.
- Network filesystems remain outside the ownerless support claim unless later
  filesystem probes prove compatible mmap, locking, and identity semantics.
