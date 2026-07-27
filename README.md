# Peer-to-Peer Distributed File Sharing System

A group-based, tracker-coordinated P2P file sharing network written in C++. Two replicated tracker
servers hold all metadata — accounts, groups, and the piece map of every shared file — while file data
moves directly between clients. Downloads are assembled from 512 KB pieces pulled from multiple peers
in parallel, with SHA-1 verification at both the piece and whole-file level.

## Features

- **Redundant tracker pair** — two trackers replicate every state change to each other; the system
  stays fully functional as long as one is reachable, and a tracker that comes back online resyncs
  automatically.
- **Every client is both seeder and leecher** — uploading a file starts serving it immediately;
  finishing a download does the same.
- **Multi-peer, multi-file concurrent downloads** — each download pulls pieces from every available
  seeder in parallel via a worker-thread pool, and multiple files can download at once.
- **Integrity guaranteed end-to-end** — SHA-1 per 512 KB piece and for the assembled file; a piece is
  verified before it's ever written to disk, and a bad piece is silently retried from another peer.
- **Automatic client failover** — a client's tracker connection survives a tracker crash mid-session,
  including transparent re-authentication if it lands on a tracker it hasn't logged into yet.
- **Constant memory regardless of file size** — uploads, downloads, and hashing all stream one piece
  at a time; a 200 MB transfer holds ~15 MB resident.
- **Custom length-prefixed TCP protocol** — one framing format for client↔tracker, client↔client, and
  tracker↔tracker traffic, immune to partial reads/writes and segment boundaries.

## Architecture

```
                 replication (ordered, retried, self-healing)
      Tracker 0  <----------------------------------------->  Tracker 1
          ^                                                        ^
          |                 metadata / peer discovery               |
          +---------------------------+---------------------------+
                                       |
                    Client A  <================>  Client B
                              512 KB piece transfers
                          (each client seeds AND downloads)
```

```
System_Files/
├── Makefile
├── tracker_info.txt
├── common/
│   ├── proto.{h,cpp}     length-prefixed framing, socket helpers, timed connect
│   └── sha1.{h,cpp}      SHA-1 (OpenSSL EVP): one-shot, incremental, whole-file
├── tracker/
│   ├── state.{h,cpp}     data model, text encoding, atomic on-disk persistence
│   ├── replica.{h,cpp}   per-remote replication queues, retry/backoff, snapshot recovery
│   └── tracker.cpp       listener, request handlers, console
└── client/
    ├── share.{h,cpp}     file hashing, share registry, peer server, piece fetch
    ├── download.{h,cpp}  multi-peer, multi-worker download manager
    ├── session.{h,cpp}   tracker connection, failover, transparent re-login
    └── client.cpp        command loop
```

Each module owns one responsibility: `state` never touches a socket, `replica` never touches the data
model directly, `share`/`download` never talk to the tracker, and `client.cpp` only wires commands to
the modules that implement them.

## Build

**Requirements:** Linux, a C++17 compiler, and OpenSSL's development headers (`libcrypto`) for SHA-1.

```bash
sudo apt install build-essential libssl-dev     # Debian/Ubuntu
cd System_Files
make
```

Produces `tracker/tracker` and `client/client`. `make clean` removes binaries, object files, and
tracker data directories.

## Usage

`tracker_info.txt` lists one `IP:PORT` per line, in tracker-index order:

```
127.0.0.1:8000
127.0.0.1:8001
```

Start both trackers, then any number of clients:

```bash
./tracker/tracker tracker_info.txt 0        # terminal 1
./tracker/tracker tracker_info.txt 1        # terminal 2

./client/client 127.0.0.1:5000 tracker_info.txt   # terminal 3
./client/client 127.0.0.1:5001 tracker_info.txt   # terminal 4
```

A client's first argument is the address it listens on for **peer** traffic. If that address matches a
tracker endpoint in `tracker_info.txt`, it's instead read as "prefer this tracker" and a free peer port
is chosen automatically — so `./client/client 127.0.0.1:8000 tracker_info.txt` also works.

Type `help` in either program for its command list. The tracker console additionally accepts `status`,
`groups`, `files`, `save`, and `quit`. Closing a tracker's stdin does not shut it down — only `quit` or
a signal does.

### Commands

| Command | Description |
| --- | --- |
| `create_user <user_id> <password>` | Register an account |
| `login <user_id> <password>` | Authenticate and start a session |
| `logout` | End the session and stop sharing every file |
| `create_group <group_id>` | Create a group; you become its owner |
| `join_group <group_id>` | Request membership |
| `leave_group <group_id>` | Leave a group (withdraws your shared files from it) |
| `list_groups` | List all groups, owners, and member counts |
| `list_requests <group_id>` | List pending join requests (owner only) |
| `accept_request <group_id> <user_id>` | Approve a join request (owner only) |
| `upload_file <group_id> <file_path>` | Share a file with a group |
| `list_files <group_id>` | List files in a group with size and seeder count |
| `download_file <group_id> <file_name> <destination_path>` | Download a file |
| `show_downloads` | Show progress of this session's downloads |
| `stop_share <group_id> <file_name>` | Stop seeding a file |
| `quit` | Log out and exit |

Downloads always run in the background. A destination that is an existing directory receives the file
under its own name. `show_downloads` reports:

```
[D] [group_id] filename - 132/400 pieces      in progress
[C] [group_id] filename                       complete and verified
[X] [group_id] filename - <reason>            failed
```

### Example session

```bash
# alice — owner and first seeder
> create_user alice secret123
> login alice secret123
> create_group research
> upload_file research /path/to/dataset.zip
> list_requests research
bob
> accept_request research bob

# bob — joins, downloads, and becomes a second seeder automatically
> create_user bob secret456
> login bob secret456
> join_group research
> download_file research dataset.zip ./downloads/
> show_downloads
[C] [research] dataset.zip
```

If a tracker goes down mid-session, the next command a client issues prints
`(switched to tracker 127.0.0.1:8001)` and continues normally — no manual reconnection required.

## Network Protocol

All control traffic (tracker requests/replies and tracker-to-tracker sync) uses one length-prefixed
frame, so a short `send()`/`recv()` or a segment boundary mid-message is never a special case:

```
+------------------------+----------------------------+
| length (4B, network BO)| payload (length bytes)     |
+------------------------+----------------------------+
```

Every tracker reply opens with a status line — `OK` (optionally followed by payload lines) or
`ERR <code>` (`not_logged_in`, `not_member`, `not_owner`, `no_such_group`, `no_such_file`, `no_peers`,
`piece_count_mismatch`, `bad_request`, …).

**Client → Tracker**

```
REGISTER <user> <password>
LOGIN <user> <password> <peer>              LOGOUT <user> <peer>
CREATE_GROUP <user> <group>                 JOIN_GROUP <user> <group>
LEAVE_GROUP <user> <group>                  LIST_GROUPS <user>
LIST_REQUESTS <user> <group>                ACCEPT_REQUEST <user> <group> <target>
UPLOAD_FILE <user> <peer> <group> <name> <size> <npieces> <file_sha1> <piece_sha1>...
LIST_FILES <user> <group>                   GET_FILE <user> <group> <name>
ADD_PEER <user> <peer> <group> <name>       STOP_SHARE <user> <peer> <group> <name>
```

`<peer>` is a full `ip:port` or just a port; in the latter case the tracker fills in the address it
observes on the connection, so a client never needs to know its own routable IP.

**Client ↔ Client**

```
GETPIECE <group> <name> <index>   ->   OK <length>
                                       <length bytes of raw piece data>
```

The piece body isn't framed a second time — its length is already in the status line, so a 512 KB
piece costs one small header plus one contiguous transfer.

**Tracker ↔ Tracker**

```
SYNC <op> <args...>    apply one replicated state change
SNAPSHOT               return the full state, for recovery
```

## File Integrity and Piece Management

Files are split into 512 KB pieces (the last one shorter if the size doesn't divide evenly). Uploading
reads the file once, computing every piece's SHA-1 and the whole-file SHA-1 in the same pass — never
more than one piece resident in memory, so a 1 GB file costs the same ~512 KB as a 1 MB one.

Downloads pre-allocate the destination and write each piece straight to its final offset as soon as it
arrives, so pieces can land in any order with no reassembly buffer. Every piece is SHA-1 verified
*before* it's written; a piece that fails simply falls through to the next available peer, exactly as
if that peer had failed to answer. Once all pieces are in, the assembled file is hashed end-to-end and
compared against the uploader's whole-file digest before the download is reported complete.

Piece order is randomized per download, and each of up to 8 worker threads per download starts at a
different offset into the peer list — spreading concurrent requests across the whole swarm instead of
everyone hammering the same peer first.

## Tracker Replication and Failover

Every state change becomes a small, idempotent op — an absolute statement ("group G's owner is U", not
a delta) — queued per remote tracker and delivered by a dedicated worker thread:

- Ops stay queued until acknowledged, so a tracker that's down loses nothing; its backlog delivers in
  order once it returns.
- Delivery failures back off from 1s to 30s instead of spinning on a dead endpoint.
- On startup, or if a backlog ever grows past its bound, the worker pulls a full `SNAPSHOT` from the
  peer and merges it — the recovery path for a tracker that was offline while the system moved on.

Because ops are idempotent, a redelivered op or one that overlaps a snapshot can't corrupt the replica.
Clients hold up their end too: a request that can't reach the current tracker is retried against the
rest of the list, and a tracker that answers `ERR not_logged_in` (just restarted, or reached before a
login replicated) triggers a transparent re-login and replay.

State is written to `tracker_data_<n>/state.txt` after every change via temp-file + `rename()`, so a
crash mid-write can't leave a truncated snapshot.

## Concurrency and Error Handling

The tracker runs one thread per connection behind a single mutex guarding all in-memory state; since
requests are short, purely in-memory operations with no I/O held under the lock, one coarse lock is
both correct and fast. The client runs a peer-server thread per piece request plus a worker pool per
active download, with the share registry and download table each independently locked.

`SIGPIPE` is ignored and all sends use `MSG_NOSIGNAL`, so a peer disappearing mid-transfer fails one
request rather than the process. Every network-supplied value — ports, sizes, piece counts, hashes —
is parsed with non-throwing, range-checked converters and capped at 8 MB per message. Connections use a
real (non-blocking + `poll`) timeout, so an unreachable host fails in seconds rather than hanging.
Sockets and file descriptors are closed on every path, including error paths, with no manual
allocation lifetimes to manage — RAII throughout.

## Design Notes and Limitations

- Passwords are stored and transmitted in plain text; a client asserts its own identity and the
  tracker checks only that the account exists and is logged in. No session tokens, no transport
  encryption — designed for a trusted network.
- Logging out (or exiting) removes a client from every file it seeds; leaving a group withdraws files
  shared into it. A file's last seeder disappearing drops it from the catalogue.
- Each write rewrites the full state snapshot — simple and crash-safe, but O(state size) per change
  rather than an incremental log.
- IPv4 only; piece size fixed at 512 KB; 8 worker threads per download.
