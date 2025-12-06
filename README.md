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
- Handles dynamic database queries via web interface
- Supports GET and POST methods
- Provides full CRUD operations for database records

### 2. Database Lookup Server (`searchdb/mdb-lookup-server`)
- Loads database into memory at startup
- Handles search, add, update, delete, and list operations
- Persists changes to disk automatically
- Supports multiple concurrent connections

### 3. HTTP Client (`clientserv/http-client`)
- Downloads files from HTTP servers
- Supports HTTP/1.0 protocol
- Saves files locally with proper naming

## Building the Project

### Prerequisites
- GCC compiler
- Make
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
make clean       # Remove all build artifacts
```

## Running the System

### Step 1: Start Database Server

Open Terminal 1:

```bash
cd searchdb
./mdb-lookup-server mdb-cs3157 9999
```

The server will display:
```
Loaded records from database
```

Keep this terminal open.

### Step 2: Start HTTP Server

Open Terminal 2:

```bash
cd network_programming
./http-server 8080 html localhost 9999
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
- Case-sensitive substring matching
- Returns all matching records with record numbers



**Terminal Setup:**
1. Open Terminal.app
2. Navigate to project directory: `cd ~/Desktop/HTTP-server-and-client-programming`
3. Build: `make`

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

The database file (`mdb-cs3157`) uses a binary format:

- **Record Structure:**
  - `name[16]` - 16 bytes for name field
  - `msg[24]` - 24 bytes for message field
  - Total: 40 bytes per record


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
