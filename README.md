# Distributed Peer-to-Peer File Sharing System

## Table of Contents
1. [Overview](#overview)
2. [System Architecture](#system-architecture)
3. [Compilation Instructions](#compilation-instructions)
4. [Execution Instructions](#execution-instructions)
5. [Usage Guide](#usage-guide)
6. [Complete Testing Example](#complete-testing-example)
7. [Network Protocol Design](#network-protocol-design)
8. [Key Algorithms](#key-algorithms)
9. [Data Structures](#data-structures)
10. [Implementation Features](#implementation-features)
11. [Testing Procedures](#testing-procedures)
12. [Assumptions and Limitations](#assumptions-and-limitations)
13. [Troubleshooting](#troubleshooting)

## Overview

This project implements a distributed peer-to-peer (P2P) file sharing system with multiple tracker support, automatic failover, and real-time synchronization. The system allows users to create groups, share files within those groups, and download files directly from other peers in a decentralized manner.

### Key Features
- Multi-tracker architecture with automatic failover
- Real-time tracker synchronization ensuring data consistency
- Peer-to-peer file transfers with integrity verification
- Group-based access control with owner privileges
- Persistent data storage across system restarts
- Concurrent downloads with piece-based file transfer
- SHA-1 based integrity checking for files and pieces

## System Architecture

### High-Level Architecture

```
┌─────────────┐    ┌─────────────┐
│  Tracker 0  │◄──►│  Tracker 1  │  (Synchronized)
│  (Primary)  │    │  (Backup)   │
└─────┬───────┘    └─────┬───────┘
      │                  │
      ▼                  ▼
┌─────────────┐    ┌─────────────┐
│   Client A  │◄──►│   Client B  │  (P2P Transfer)
│    (Peer)   │    │    (Peer)   │
└─────────────┘    └─────────────┘
```

### Component Architecture

**Tracker Components:**
- User Management: Authentication and user state management
- Group Management: Group creation, membership, and access control
- File Registry: Metadata storage for shared files
- Synchronization Engine: Real-time data replication between trackers
- Network Interface: Client request handling and inter-tracker communication

**Client Components:**
- Command Interface: User interaction and command processing
- File Manager: File hashing, piece generation, and integrity verification
- Peer Server: Serves file pieces to other peers
- Download Manager: Coordinates piece downloads from multiple peers
- Tracker Interface: Communication with tracker servers and failover handling

## Compilation Instructions

### Prerequisites
- Operating System: Linux/Unix (tested on Ubuntu, CentOS)
- Compiler: C++11
- Dependencies: pthread library

### Build Process

```bash
# Extract and navigate to source directory
cd System_Files/

# Clean and compile
make clean && make all

# Verify compilation
ls tracker/tracker client/client
```

### Expected Output
```
g++ -std=c++11 -O2 -pthread -Wall -Wextra -o tracker/tracker tracker/tracker.cpp common/proto.cpp common/sha1.cpp
g++ -std=c++11 -O2 -pthread -Wall -Wextra -o client/client client/client.cpp common/proto.cpp common/sha1.cpp
```

## Execution Instructions

### 1. Configure Trackers

Edit `tracker_info.txt` to specify tracker endpoints:
```
127.0.0.1:8000
127.0.0.1:8001
```

### 2. Start Trackers

```bash
# Terminal 1: Primary tracker
./tracker/tracker tracker_info.txt 0

# Terminal 2: Backup tracker
./tracker/tracker tracker_info.txt 1
```

### 3. Start Clients

```bash
# Terminal 3: First client
./client/client 127.0.0.1:8000 tracker_info.txt

# Terminal 4: Second client (optional)
./client/client 127.0.0.1:8000 tracker_info.txt
```

## Usage Guide

### User Management Commands

**Create User:**
```bash
create_user <username> <password>
```

**Login:**
```bash
login <username> <password>
```

**Logout:**
```bash
logout
```

### Group Management Commands

**Create Group:**
```bash
create_group <groupname>
```

**Join Group:**
```bash
join_group <groupname>
```

**List Groups:**
```bash
list_groups
```

**List Join Requests (Owner Only):**
```bash
list_requests <groupname>
```

**Accept Join Request (Owner Only):**
```bash
accept_request <groupname> <username>
```

**Leave Group:**
```bash
leave_group <groupname>
```

### File Operation Commands

**Upload File:**
```bash
upload_file <groupname> <filepath>
```

**List Files in Group:**
```bash
list_files <groupname>
```

**Download File:**
```bash
download_file <groupname> <filename> <destination>

# Background download
download_file <groupname> <filename> <destination> &
```

**Show Download Status:**
```bash
show_downloads
```

**Stop Sharing File:**
```bash
stop_share <groupname> <filename>
```

**Quit Client:**
```bash
quit
```

## Complete Testing Example

This section demonstrates a complete testing workflow using professional placeholder names.

### Step 1: System Infrastructure Setup

```bash
# Terminal 1: Start primary tracker
./tracker/tracker tracker_info.txt 0

# Terminal 2: Start backup tracker  
./tracker/tracker tracker_info.txt 1

# Terminal 3: Start client
./client/client 127.0.0.1:8000 tracker_info.txt
```

### Step 2: User Registration and Authentication

```bash
create_user user1 password123
login user1 password123

create_user user2 password456
login user2 password456

create_user user3 password789
login user3 password789
```

### Step 3: Group Management

```bash
# User1 creates and manages group
create_group project_team
list_groups

# Other users request to join
join_group project_team

# Owner checks and accepts requests
list_requests project_team
accept_request project_team user2
accept_request project_team user3
```

### Step 4: File Upload Operations

```bash
# Upload various file types
upload_file project_team /path/to/document.txt
upload_file project_team /path/to/presentation.pdf
upload_file project_team /path/to/archive.zip
upload_file project_team /path/to/video.mp4

# Verify uploads
list_files project_team
```

Expected output:
```
document.txt
presentation.pdf
archive.zip
video.mp4
```

### Step 5: File Download Operations

```bash
# Download files from other peers
download_file project_team document.txt /downloads/
download_file project_team presentation.pdf /downloads/
download_file project_team archive.zip /downloads/

# Test background download
download_file project_team video.mp4 /downloads/ &

# Monitor download progress
show_downloads
```

Expected output:
```
[C] project_team document.txt
[C] project_team presentation.pdf
[D] project_team video.mp4 - 15/32
```

### Step 6: File Sharing Management

```bash
# Stop sharing specific files
stop_share project_team document.txt
stop_share project_team presentation.pdf

# Verify files removed from sharing
list_files project_team
```

### Step 7: Group Exit and Session Management

```bash
# Leave group and logout
leave_group project_team
logout
quit
```

### Step 8: Multi-Tracker Failover Testing

1. Perform operations with both trackers running
2. Kill primary tracker (Ctrl+C in Terminal 1)
3. Continue operations - client automatically switches to backup

Expected behavior:
```
> download_file project_team document.txt /downloads/
Switched to tracker: 127.0.0.1:8001
[C] project_team document.txt
```

## Network Protocol Design

### Message Format
All messages use length-prefixed format:
```
┌─────────────┬─────────────────────┐
│   Length    │      Payload        │
│  (4 bytes)  │   (Length bytes)    │
│ (Big Endian)│   (UTF-8 String)    │
└─────────────┴─────────────────────┘
```

### Client-Tracker Protocol

**Authentication:**
```
REGISTER <username> <password>
LOGIN <username> <password>
```

**Group Management:**
```
CREATE_GROUP <username> <groupname>
JOIN_GROUP <username> <groupname>
LIST_GROUPS
ACCEPT_REQUEST <groupname> <user> <owner>
LEAVE_GROUP <username> <groupname>
```

**File Operations:**
```
UPLOAD_META <group> <filename> <size> <pieces> <sha1> <peer_addr> <owner> <hashes...>
LIST_FILES <groupname> <username>
GET_FILE_PEERS <group> <filename> <username>
```

### Peer-to-Peer Protocol

**File Piece Request:**
```
GETPIECE <filename> <piece_index>
```

**Response:**
```
OK
<piece_length> (4 bytes, network order)
<piece_data> (piece_length bytes)
```

### Tracker Synchronization Protocol

```
SYNC <operation> <parameters...>

Examples:
SYNC REGISTER user1 password123
SYNC CREATE_GROUP user1 project_team
SYNC UPLOAD_META project_team file.txt 1024 2 sha1hash peer_addr user1 hash1 hash2
```

## Key Algorithms

### 1. File Hashing Algorithm
- Files divided into 512KB pieces
- SHA-1 hash computed for each piece
- File hash = SHA-1 of concatenated piece hashes
- Ensures integrity verification at multiple levels

### 2. Tracker Synchronization Algorithm
- Real-time replication of all state changes
- Execute locally → Save immediately → Sync to peers
- Asynchronous synchronization for performance
- Automatic retry with exponential backoff

### 3. Download Management Algorithm
- Multi-peer concurrent downloads (max 8 threads)
- Batch processing for optimal resource usage
- SHA-1 verification of each downloaded piece
- Automatic retry on piece download failure
- Round-robin peer selection strategy

### 4. Automatic Failover Algorithm
- Connection timeout detection
- Automatic tracker list iteration
- Seamless client switching to backup tracker
- Zero data loss during failover

## Data Structures

### Tracker Data Structures

**User Structure:**
```cpp
struct User { 
    string pass;        // User password
    bool logged;        // Login status
};
```
Storage: `unordered_map<string, User>` for O(1) lookup

**File Structure:**
```cpp
struct File { 
    string group, filename, owner, sha; 
    uint64_t size; 
    vector<string> piece_sha;  // Piece hashes
    set<string> peers;         // Available peers
};
```
Storage: `unordered_map<string, File>` with compound key

**Group Structure:**
```cpp
unordered_map<string, pair<string, set<string>>> groups;
// group_name -> (owner, members_set)
```

### Client Data Structures

**Download Status:**
```cpp
struct DownloadStatus {
    string group, filename, dest;
    atomic<int> remaining;      // Thread-safe counters
    vector<int> have;           // Piece bitmap
    mutex m;                    // Synchronization
};
```

## Implementation Features

### Core Features
- Multi-user authentication with session management
- Group-based access control with owner privileges
- SHA-1 integrity verification for all file transfers
- Multi-tracker architecture with real-time synchronization
- Automatic failover with zero data loss
- Concurrent piece downloads from multiple peers
- Complete data persistence across system restarts

### Advanced Features
- Thread-safe concurrent operations
- Efficient memory management with bounded buffers
- Network timeout handling with retry mechanisms
- Comprehensive error recovery and logging
- Scalable architecture supporting multiple concurrent users

## Testing Procedures

### 1. Basic Functionality Testing
Test all user operations, group management, and file operations with single tracker setup.

### 2. Multi-Tracker Synchronization Testing
Verify real-time data replication between multiple trackers for all operations.

### 3. Failover Testing
Test automatic client switching during tracker failures with data consistency verification.

### 4. Concurrent Download Testing
Verify multi-peer downloads with integrity checking and performance measurement.

### 5. Stress Testing
Load testing with multiple concurrent clients and file operations.

### 6. Data Persistence Testing
Verify data recovery after tracker crashes and restarts.

## Assumptions and Limitations

### Assumptions
- Reliable TCP network within local environment
- POSIX-compliant file system
- Trusted network environment
- IPv4 addressing scheme
- Sufficient system resources for concurrent operations

### Limitations
- Passwords stored in plaintext (no encryption)
- No network communication encryption
- Fixed 512KB piece size
- Maximum 8 concurrent downloads per client
- O(n) synchronization cost between trackers
- No automatic peer discovery mechanism

### Known Issues
- Potential race conditions during concurrent group operations
- Limited retry mechanisms for network failures
- Manual intervention required for split-brain scenarios

## Troubleshooting

### Common Compilation Issues
```bash
# Missing pthread library
make CXXFLAGS="-std=c++11 -O2 -pthread -Wall -Wextra"

# Missing header files
ls common/proto.h common/sha1.h
```

### Runtime Issues
```bash
# Port already in use
netstat -tulpn | grep :8000

# Permission denied
chmod 755 . && mkdir -p tracker_data_0

# Connection refused
ps aux | grep tracker
```

### Network Issues
```bash
# Firewall blocking P2P ports
sudo ufw allow 20000:35000/tcp

# Tracker synchronization problems
grep "Synced to tracker" tracker_output.log
```

### Debug Mode
```bash
# Enable verbose logging
make CXXFLAGS="-std=c++11 -g -pthread -Wall -Wextra -DDEBUG"

# Monitor system resources
top -p $(pidof tracker)
```

This documentation provides comprehensive information for building, deploying, testing, and maintaining the distributed P2P file sharing system with professional examples and complete technical specifications.
