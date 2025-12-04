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
- Unix-like system (macOS, Linux)

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

**Access via Command Line:**
```bash
curl http://localhost:8080/index.html
curl -O http://localhost:8080/ship.jpg
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

### List All Records

**Via Web Browser:**
1. Navigate to `http://localhost:8080/mdb-list`
2. View all database records in a table
3. Each record shows:
   - Record ID
   - Name
   - Message
   - Edit and Delete buttons

**Via Command Line:**
```bash
curl http://localhost:8080/mdb-list
```

### Add New Record

**Via Web Browser:**
1. Navigate to `http://localhost:8080/mdb-add`
2. Fill in the form:
   - **Name**: Maximum 15 characters
   - **Message**: Maximum 23 characters
3. Click "Add"
4. You will be redirected to the list page showing your new record

**Via Command Line:**
```bash
curl -X POST http://localhost:8080/mdb-add \
  -d "name=JohnDoe&msg=Hello World" \
  -H "Content-Type: application/x-www-form-urlencoded"
```

**Field Limits:**
- Name: 15 characters maximum
- Message: 23 characters maximum
- Fields are required

### Edit Existing Record

**Via Web Browser:**
1. Navigate to `http://localhost:8080/mdb-list`
2. Click "Edit" next to the record you want to modify
3. Modify the name and/or message fields
4. Click "Update"
5. You will be redirected to the list page with updated data

**Via Command Line:**
```bash
curl -X POST http://localhost:8080/mdb-update \
  -d "id=1&name=UpdatedName&msg=Updated Message" \
  -H "Content-Type: application/x-www-form-urlencoded"
```

### Delete Record

**Via Web Browser:**
1. Navigate to `http://localhost:8080/mdb-list`
2. Click "Delete" next to the record you want to remove
3. Confirm deletion in the popup dialog
4. You will be redirected to the list page with the record removed

**Via Command Line:**
```bash
curl -X POST http://localhost:8080/mdb-delete \
  -d "id=1" \
  -H "Content-Type: application/x-www-form-urlencoded"
```

### Using the HTTP Client

The HTTP client can download files from any HTTP server.

**Basic Usage:**
```bash
cd clientserv
./http-client <hostname> <port> <filepath>
```

**Examples:**
```bash
# Download index.html from local server
./http-client localhost 8080 /index.html

# Download search form
./http-client localhost 8080 /mdb-lookup

# Download search results
./http-client localhost 8080 "/mdb-lookup?key=test"
```

**Output:**
- Files are saved in the current directory
- Filename is extracted from the URL path
- HTTP status is displayed on stderr
- Exits with error code if download fails

## Device-Specific Instructions

### macOS

**Terminal Setup:**
1. Open Terminal.app
2. Navigate to project directory: `cd ~/Desktop/HTTP-server-and-client-programming`
3. Build: `make`
4. Run servers as described above

**Browser Testing:**
- Safari: Open `http://localhost:8080/mdb-lookup`
- Chrome: Open `http://localhost:8080/mdb-lookup`
- Firefox: Open `http://localhost:8080/mdb-lookup`

**Port Conflicts:**
If ports are in use:
```bash
# Check what's using port 8080
lsof -i :8080

# Check what's using port 9999
lsof -i :9999

# Kill processes if needed
kill -9 <PID>
```

### Linux

**Terminal Setup:**
1. Open terminal emulator
2. Navigate to project directory
3. Build: `make`
4. Run servers as described above

**Browser Testing:**
- Firefox: `http://localhost:8080/mdb-lookup`
- Chrome/Chromium: `http://localhost:8080/mdb-lookup`

**Firewall:**
If connection refused, check firewall:
```bash
sudo ufw status
sudo ufw allow 8080
sudo ufw allow 9999
```

### Remote Access (Network Devices)

To access from other devices on the same network:

**1. Find Your IP Address:**
```bash
# macOS/Linux
ifconfig | grep "inet "

# Or
ip addr show
```

**2. Start Servers:**
```bash
# Database server (same as before)
cd searchdb
./mdb-lookup-server mdb-cs3157 9999

# HTTP server - bind to all interfaces
cd network_programming
./http-server 8080 html 0.0.0.0 9999
```

**3. Access from Other Devices:**
- From another computer: `http://<your-ip>:8080/mdb-lookup`
- From mobile device: `http://<your-ip>:8080/mdb-lookup`

**Note:** Ensure firewall allows connections on ports 8080 and 9999.

### Mobile Device Testing

**On Same Network:**
1. Find server computer's IP address
2. Start servers as described above
3. On mobile device browser, navigate to: `http://<server-ip>:8080/mdb-lookup`

**On Localhost (Development):**
- Use `localhost` or `127.0.0.1` only works on the same machine
- For mobile testing, use the server's network IP address

## API Endpoints

### GET Endpoints

| Endpoint | Description |
|----------|-------------|
| `/mdb-lookup` | Search form page |
| `/mdb-lookup?key=<term>` | Search results for term |
| `/mdb-list` | List all records with edit/delete options |
| `/mdb-add` | Add record form |
| `/mdb-edit?id=<num>` | Edit form for record number |
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

- **File Format:**
  - Fixed-size binary records
  - No header or metadata
  - Records stored sequentially

## Error Handling

### Common HTTP Status Codes

- **200 OK** - Request successful
- **302 Found** - Redirect (after add/update/delete)
- **400 Bad Request** - Invalid input or malformed request
- **403 Forbidden** - Directory access without trailing slash
- **404 Not Found** - File or record not found
- **413 Payload Too Large** - File exceeds 100MB limit
- **414 URI Too Long** - Request URI too long
- **500 Internal Server Error** - Server-side error
- **501 Not Implemented** - Unsupported HTTP method
- **503 Service Unavailable** - Database server unavailable

### Troubleshooting

**Database Server Won't Start:**
```bash
# Check if port is in use
lsof -i :9999

# Check database file exists
ls -la searchdb/mdb-cs3157

# Check file permissions
chmod 644 searchdb/mdb-cs3157
```

**HTTP Server Won't Start:**
```bash
# Check if port is in use
lsof -i :8080

# Verify database server is running first
telnet localhost 9999

# Check web root directory exists
ls -la network_programming/html/
```

**No Database Results:**
```bash
# Test database server directly
echo "test" | nc localhost 9999

# Check database has records
cd searchdb
./mdb-lookup-server mdb-cs3157 9999
# Look for "Loaded X records" message
```

**Connection Refused:**
- Ensure database server starts before HTTP server
- Check firewall settings
- Verify `localhost` resolves correctly
- Try using `127.0.0.1` instead of `localhost`

**Records Not Appearing After Add:**
- Check server logs for errors
- Verify database file permissions (must be writable)
- Ensure database server has write access to directory
- Check disk space

## Stopping Servers

**Graceful Shutdown:**
- Press `Ctrl+C` in each terminal running a server

**Force Stop:**
```bash
pkill -f mdb-lookup-server
pkill -f http-server
```

**Check Running Servers:**
```bash
ps aux | grep -E "mdb-lookup-server|http-server" | grep -v grep
```

## Security Notes

- Input validation prevents buffer overflows
- Path traversal protection prevents directory escape
- HTML escaping prevents XSS attacks
- File size limits prevent resource exhaustion
- Connection timeouts prevent hanging connections

**For Production Use:**
- Add authentication/authorization
- Use HTTPS instead of HTTP
- Implement rate limiting
- Add logging to files
- Use proper file permissions
- Consider using a reverse proxy

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

## Examples

### Complete Workflow Example

```bash
# Terminal 1: Start database server
cd searchdb
./mdb-lookup-server mdb-cs3157 9999

# Terminal 2: Start HTTP server
cd network_programming
./http-server 8080 html localhost 9999

# Terminal 3: Test operations
# Search
curl "http://localhost:8080/mdb-lookup?key=hello"

# Add record
curl -X POST http://localhost:8080/mdb-add \
  -d "name=TestUser&msg=Test Message"

# List all
curl http://localhost:8080/mdb-list

# Download file
cd clientserv
./http-client localhost 8080 /index.html
```

### Browser Workflow

1. Open browser: `http://localhost:8080/mdb-lookup`
2. Search for "test" → See results
3. Click "List All" link → See all records
4. Click "Add New" → Fill form → Submit → See new record
5. Click "Edit" on a record → Modify → Update → See changes
6. Click "Delete" on a record → Confirm → See record removed

## Performance Notes

- Database loads once at startup (fast queries)
- Static files served efficiently in chunks
- Connection pooling for backend (persistent connection)
- Automatic reconnection on backend failures
- File size limit: 100MB per file

## License

This is an educational project for network and database programming.

## Support

For issues or questions:
1. Check server logs for error messages
2. Verify all prerequisites are met
3. Ensure ports are not in use
4. Check file permissions
5. Review troubleshooting section above

