# HTTP Server and Client Programming Project

A complete HTTP server and client system with database integration, supporting static file serving, dynamic database queries, and full CRUD operations through a web interface.

## System Architecture

```
Web Browser/HTTP Client
        ↓
   HTTP Server (port 8080)
        ↓
Database Lookup Server (port 9999)
        ↓
   Database File (mdb-cs3157)
```

## Components

### 1. HTTP Server (`network_programming/http-server`)
- Serves static HTML, images, and other files
- Confines static reads to regular files beneath the configured web root
- Handles dynamic database queries via web interface
- Supports GET and POST methods
- Provides full CRUD operations for database records

### 2. Database Lookup Server (`searchdb/mdb-lookup-server`)
- Loads database into memory at startup
- Handles search, add, update, delete, and list operations
- Gives every record a stable 64-bit ID
- Persists each mutation atomically before reporting success
- Provides structured `LIST2`/`SEARCH2` rows for the HTTP integration while
  retaining the original human-readable commands
- Processes one client connection at a time

### 3. HTTP Client (`clientserv/http-client`)
- Downloads files from HTTP servers
- Supports HTTP/1.0 protocol
- Writes through a same-directory temporary file and publishes only complete
  downloads
- Refuses to overwrite an existing destination

## Building the Project

### Prerequisites
- GCC compiler
- Make
- Python 3 (for the regression suite)
- Unix-like system

### Build All Components

From the project root directory:

```bash
make
```

This builds:
- `clientserv/http-client` - HTTP client
- `network_programming/http-server` - HTTP server
- `searchdb/mdb-lookup-server` - Database server

### Build Individual Components

```bash
make client      # Build HTTP client only
make server      # Build HTTP server only
make database    # Build database server only
make test        # Build and run the isolated regression suite
make clean       # Remove all build artifacts
```

## Running the System

### Step 1: Start Database Server

Open Terminal 1:

```bash
cp searchdb/mdb-cs3157 /tmp/http-mdb-manual-test.mdb
./searchdb/mdb-lookup-server /tmp/http-mdb-manual-test.mdb 9999
```

Use a disposable copy because successful CRUD requests persist changes. Keep
this terminal open.

### Step 2: Start HTTP Server

Open Terminal 2:

```bash
./network_programming/http-server 8080 network_programming/html localhost 9999
```

The server will start and connect to the database server. Keep this terminal open.

### Step 3: Access the Web Interface

Open your web browser and navigate to:

- **Search Database**: `http://localhost:8080/mdb-lookup`
- **List All Records**: `http://localhost:8080/mdb-list`
- **Add New Record**: `http://localhost:8080/mdb-add`
- **Static Content**: `http://localhost:8080/index.html`

## Usage Guide

### Static File Serving

The HTTP server serves static files from the `network_programming/html/` directory.
Path components are opened relative to that directory without following
symbolic links. Directories are served only through their own `index.html`,
and special files such as FIFOs and devices are rejected.

**Access via Browser:**
```
http://localhost:8080/index.html
http://localhost:8080/ship.jpg
http://localhost:8080/crew.jpg
```

**Supported File Types:**
- HTML files (`.html`) - Content-Type: `text/html`
- JPEG images (`.jpg`) - Content-Type: `image/jpeg`
- PNG images (`.png`) - Content-Type: `image/png`
- GIF images (`.gif`) - Content-Type: `image/gif`
- Other files - Content-Type: `application/octet-stream`

### Database Search

**Via Web Browser:**
1. Navigate to `http://localhost:8080/mdb-lookup`
2. Enter a search term in the form
3. Click submit or press Enter
4. View matching records in an HTML table

**Via Command Line:**
```bash
# Simple search
curl "http://localhost:8080/mdb-lookup?key=hello"

# Search with URL encoding
curl "http://localhost:8080/mdb-lookup?key=test%20world"
```

**Search Behavior:**
- Searches both name and message fields
- Case-insensitive substring matching
- Returns all matching records with stable record IDs

### HTTP Client Downloads

Run the client from the directory where the downloaded basename should be
created:

```bash
mkdir -p /tmp/http-client-download
cd /tmp/http-client-download
/path/to/HTTP-server-and-client-programming/clientserv/http-client \
  localhost 8080 /ship.jpg
```

The client keeps partial content in a hidden `.<name>.part.*` file, requires a
complete response-header block, validates any declared `Content-Length`,
flushes and syncs the result, and then publishes it without replacing an
existing file. A malformed, failed, or truncated transfer leaves no final
output.

## API Endpoints

| Endpoint | Description |
|----------|-------------|
| `/mdb-lookup` | Search form page |
| `/mdb-list` | List all records with edit/delete options |
| `/mdb-add` | Add record form |
| `/index.html` | Static HTML page |
| `/ship.jpg` | Static image file |
| `/crew.jpg` | Static image file |

### POST Endpoints

| Endpoint | Form Fields | Description |
|----------|-------------|-------------|
| `/mdb-add` | `name`, `msg` | Add new record |
| `/mdb-update` | `id`, `name`, `msg` | Update existing record |
| `/mdb-delete` | `id` | Delete record by ID |

## Database Format

The database server reads both the original 40-byte legacy record format and
the versioned `MDB2` format.

Legacy records contain:

- `name[16]`
- `msg[24]`

Legacy files are not changed by searches or list operations. On the first
successful mutation, the server migrates the file atomically to `MDB2`.

`MDB2` uses explicit little-endian serialization:

- 28-byte header: 8-byte magic, 32-bit version, 64-bit next ID, and
  64-bit record count
- 48-byte records: 64-bit stable ID, `name[16]`, and `msg[24]`

IDs are monotonic, survive restarts, and are not reused after deletion.
ADD, UPDATE, and DELETE use clone–persist–swap transactions: the proposed
state is written, flushed, synced, and atomically renamed before it replaces
the live in-memory state. A persistence failure leaves both memory and disk
unchanged.

The HTTP server reads records with the internal `LIST2` and `SEARCH2`
commands. Each response row contains `id`, `name`, and `message` as three
tab-separated fields followed by a blank-line terminator. Stored control
bytes, including tabs and newlines, are rejected, so printable characters
such as `}` are preserved without ambiguous parsing. The original `LIST` and
`SEARCH` output remains available to direct backend clients. Because older
backend binaries do not know the structured commands, upgrade/restart the HTTP
server and database server together.

## Testing

Run the safe test suite from the project root:

```bash
make test
```

`./test_system.sh` is a compatibility wrapper for the same command. Tests
create temporary database files and web roots, choose unused loopback ports,
terminate only their own child processes, and exit nonzero on failure. They
never mutate `searchdb/mdb-cs3157`. Coverage includes exact HTML escaping,
structured row framing, symlink escape denial, and non-destructive client
download failures.


## File Structure

```
HTTP-server-and-client-programming/
├── README.md                    # This file
├── Makefile                     # Root build file
├── test_system.sh              # Automated test script
├── clientserv/
│   ├── http-client             # HTTP client binary
│   ├── http-client.c           # HTTP client source
│   └── Makefile                # Client build file
├── network_programming/
│   ├── http-server             # HTTP server binary
│   ├── http-server.c           # HTTP server source
│   ├── Makefile                # Server build file
│   └── html/                   # Web root directory
│       ├── index.html          # Static HTML page
│       ├── ship.jpg            # Static image
│       └── crew.jpg            # Static image
└── searchdb/
    ├── mdb-lookup-server       # Database server binary
    ├── mdb-lookup-server.c     # Database server source
    ├── mdb.h                   # Database record definition
    ├── mdb.c                   # Database utilities
    ├── mylist.h                # Linked list header
    ├── mylist.c                # Linked list implementation
    ├── mdb-cs3157              # Database file (binary)
    └── Makefile                # Database server build file
```


### Browser Workflow

1. Open browser: `http://localhost:8080/mdb-list`
2. Search for "test" → See results
3. Click "List All" link → See all records
4. Click "Add New" → Fill form → Submit → See new record
5. Click "Edit" on a record → Modify → Update → See changes
6. Click "Delete" on a record → Confirm → See record removed
