# TFTP Transfer System

Implementation of a TFTP client/server architecture in C based on RFC 1350.

This project provides a complete file transfer solution over UDP with support for:

- File upload and download (PUT / GET)
- RRQ / WRQ request handling
- DATA / ACK / ERROR packets
- Timeout and retransmission management
- Concurrent sessions
- Large file extension (BIGFILE mode)
- Experimental WINDOWSIZE support
- Multi-threaded and select()-based server versions

---

## Project Overview

The objective of this project was to implement a reliable TFTP transfer system while preserving compatibility with the original protocol.

The implementation includes:

### Client

- File download (GET)
- File upload (PUT)
- Timeout detection
- Automatic retransmission
- Large file support through BIGFILE mode
- File size verification before transfer

### Server

Two implementations are available:

#### select()-based server

Uses `select()` to monitor activity on sockets and manage multiple sessions without blocking.

Features:

- Simultaneous session handling
- Timeout verification
- Low CPU usage
- Session state management

#### Multi-threaded server

Each client request creates a dedicated thread.

Features:

- One thread per client
- Independent UDP sockets
- Concurrent transfers
- Mutex synchronization
- File locking system

---

## Supported TFTP Messages

| Message | Description |
|----------|-------------|
| RRQ | Read Request |
| WRQ | Write Request |
| DATA | File data transfer |
| ACK | Packet acknowledgment |
| ERROR | Error reporting |

---

## Reliability Mechanisms

Since UDP does not guarantee packet delivery, several mechanisms were implemented:

- Timeout management
- Automatic retransmission
- Session monitoring
- Retry limits
- Lost packet handling

Retransmission limits:

Server:
- 3 attempts

Client:
- 5 attempts

---

## BIGFILE Extension

Standard TFTP limits block numbering.

A custom extension called **BIGFILE** was introduced:

Standard mode:

- Header size: 4 bytes
- Limited transfer size

BIGFILE mode:

- Header size: 6 bytes
- 32-bit block identifiers
- Support for larger transfers

Transfers above 65 KB require BIGFILE activation.

---

## Concurrency Handling

The project supports concurrent accesses using:

### Mutex synchronization

```c
pthread_mutex_t
```

Used to protect:

- file creation
- write operations
- shared resources

### File locking

Temporary `.lock` files prevent simultaneous modifications of the same resource.

Example:

```
file.txt.lock
```

---

## Technologies

Language:

- C

Libraries:

- pthread
- sockets API
- select()
- UDP networking

Tools:

- Wireshark
- GCC
- Linux environment

---

## Testing

Validation scenarios included:

- small file transfers
- large file transfers
- packet loss simulation
- timeout handling
- concurrent transfers
- BIGFILE activation
- interoperability checks

Network traces were analysed with Wireshark.

---

## Architecture

```text
Client
      ↓
UDP communication
      ↓
Server
      ↓
Session manager
      ↓
File system
```

Concurrent mode:

```text
Client 1 → Thread 1
Client 2 → Thread 2
Client 3 → Thread 3
```

---

## Future Improvements

Planned work:

- Full WINDOWSIZE implementation
- Sliding window support
- Performance optimisation
- Transfer statistics
- Logging system
- CLI improvements

---

## Academic Context

Project developed during the third year of Computer Science degree.

Focus areas:

- Network programming
- UDP communication
- Concurrent programming
- Synchronization
- Protocol implementation
- Reliability mechanisms

---

## Authors

Thomas Augendre  
Rania Bentabe
