# Complete CRUD Operations Testing Guide

This guide shows you how to test all database operations: Search, List, Add, Update, and Delete.

## Automated Regression Test

Run the complete isolated suite first:

```bash
make test
```

The suite builds all three programs, uses random loopback ports, creates a
temporary web root and database for every test, and terminates only the
processes it starts. It does not modify `searchdb/mdb-cs3157`.

The regression suite also verifies exact HTML escaping, printable delimiter
characters such as `}`, structured backend framing (including legacy empty
fields), static-path symlink confinement, and atomic client failure cleanup.

## Manual-Test Prerequisites

To avoid changing the repository's database while testing mutations, make a
disposable copy from the project root:

```bash
cp searchdb/mdb-cs3157 /tmp/http-mdb-manual-test.mdb
```

1. **Start the Database Server** (Terminal 1):
```bash
./searchdb/mdb-lookup-server /tmp/http-mdb-manual-test.mdb 9999
```

2. **Start the HTTP Server** (Terminal 2):
```bash
./network_programming/http-server 8080 network_programming/html localhost 9999
```

Restart both processes after rebuilding; an already-running process continues
to execute the old binary.

## Testing All Operations

### 1. **SEARCH (Lookup) - `/mdb-lookup`**

**Via Web Browser:**
- Navigate to: `http://localhost:8080/mdb-lookup`
- Enter a search term in the form (e.g., "test", "hello", "world")
- Click "Submit"
- **Expected:** Table showing matching records, or "ENTRY NOT FOUND" if no matches

**Via Command Line (curl):**
```bash
# Search for "test"
curl "http://localhost:8080/mdb-lookup?key=test"

# Search for "hello"
curl "http://localhost:8080/mdb-lookup?key=hello"

# Search with URL encoding
curl "http://localhost:8080/mdb-lookup?key=hello%20world"
```

**What to verify:**
- ✅ Form displays correctly
- ✅ Search returns matching records
- ✅ Shows "ENTRY NOT FOUND" when no matches
- ✅ Results are displayed in a table format

---

### 2. **LIST ALL - `/mdb-list`**

**Via Web Browser:**
- Navigate to: `http://localhost:8080/mdb-list`
- **Expected:** Table showing ALL database records with Edit and Delete buttons

**Via Command Line:**
```bash
curl http://localhost:8080/mdb-list
```

**What to verify:**
- ✅ All records are displayed
- ✅ Each record shows ID, Name, Message
- ✅ Edit and Delete links/buttons are present
- ✅ Navigation links work (Search, Add New)

---

### 3. **ADD - `/mdb-add`**

**Via Web Browser:**
1. Navigate to: `http://localhost:8080/mdb-add`
2. Fill in the form:
   - **Name:** (max 15 bytes)
   - **Message:** (max 23 bytes)
3. Click "Add"
4. **Expected:** Redirects to `/mdb-list` showing the new record

**Via Command Line (POST):**
```bash
# Add a new record
curl -L \
  -d "name=John&msg=Hello World" \
  "http://localhost:8080/mdb-add"

# Add another record
curl -L \
  -d "name=Jane&msg=Testing 123" \
  "http://localhost:8080/mdb-add"
```

Do not combine `-X POST` with `-L` here. With an explicit method, curl can
repeat POST against the redirect target; `-d` selects POST for the first
request and lets curl follow the server's `302` with GET.

**What to verify:**
- ✅ Form displays correctly
- ✅ Can add new records
- ✅ Redirects to list page after adding
- ✅ New record appears in the list
- ✅ Validation works (name/message length limits)

---

### 4. **EDIT/UPDATE - `/mdb-edit` and `/mdb-update`**

**Via Web Browser:**
1. Go to `/mdb-list` and click "Edit" on any record
2. **Expected:** Form pre-filled with current values
3. Modify the name or message
4. Click "Update"
5. **Expected:** Redirects to `/mdb-list` with updated record

**Via Command Line:**
```bash
# First, read the stable ID from the list, then get the edit form
curl "http://localhost:8080/mdb-list"
curl "http://localhost:8080/mdb-edit?id=RECORD_ID"

# Update that record
curl -L \
  -d "id=RECORD_ID&name=UpdatedName&msg=Updated Message" \
  "http://localhost:8080/mdb-update"
```

**What to verify:**
- ✅ Edit form shows current values
- ✅ Can modify name and message
- ✅ Updates are saved correctly
- ✅ Redirects to list after update
- ✅ Updated record shows new values in list

---

### 5. **DELETE - `/mdb-delete`**

**Via Web Browser:**
1. Go to `/mdb-list`
2. Click "Delete" on any record
3. Confirm deletion in the popup
4. **Expected:** Record is deleted and page refreshes

**Via Command Line:**
```bash
# Delete a record using the stable ID shown by /mdb-list
curl -L \
  -d "id=RECORD_ID" \
  "http://localhost:8080/mdb-delete"
```

**What to verify:**
- ✅ Delete button works
- ✅ Confirmation dialog appears (in browser)
- ✅ Record is removed from database
- ✅ List updates after deletion
- ✅ Deleted record no longer appears in search

---

## Complete Test Workflow

Here's a suggested workflow to test everything:

```bash
# 1. Start both servers as shown in Manual-Test Prerequisites.

# 2. Test Search
curl "http://localhost:8080/mdb-lookup?key=test"

# 3. Test List All
curl http://localhost:8080/mdb-list

# 4. Test Add
curl -L -d "name=TestUser&msg=TestMessage" "http://localhost:8080/mdb-add"

# 5. Test Search again (should find the new record)
curl "http://localhost:8080/mdb-lookup?key=TestUser"

# 6. Get the new stable ID from the list, then edit and update it
curl "http://localhost:8080/mdb-list"
curl "http://localhost:8080/mdb-edit?id=RECORD_ID"
curl -L -d "id=RECORD_ID&name=Updated&msg=NewMsg" \
  "http://localhost:8080/mdb-update"

# 7. Test Delete
curl -L -d "id=RECORD_ID" "http://localhost:8080/mdb-delete"

# 8. Verify deletion
curl "http://localhost:8080/mdb-lookup?key=Updated"  # Should not find it
```

Every successful ADD, UPDATE, or DELETE is persisted before the HTTP server
returns its redirect. Restart both servers and list the records again to
verify restart durability.

## Stable IDs and Persistence

- Legacy 40-byte database files remain unchanged during LIST and SEARCH.
- The first successful mutation migrates a legacy file to versioned `MDB2`.
- Record IDs are unsigned 64-bit values stored in the file, remain stable
  across deletions and restarts, and are never reassigned.
- Mutations use clone–persist–swap: a temporary file is written, flushed,
  synced, and atomically renamed before in-memory state changes.
- A persistence error returns HTTP `500` and leaves both memory and disk
  unchanged. A syntactically valid but absent update/delete ID returns `404`.

## Rendering and Backend Framing

- `<`, `>`, `&`, `"`, and `'` are escaped before names, messages, or search
  values are inserted into HTML text or form attributes.
- Printable braces are valid record content and survive list, search, edit,
  update, restart, and delete operations.
- The HTTP server uses the backend's `LIST2` and `SEARCH2` commands. Their
  records are `id<TAB>name<TAB>message`, with a blank line ending each
  response. Because stored control bytes are rejected, the two separators
  cannot be confused with field content.
- The legacy human-readable `LIST` and `SEARCH` commands remain available for
  direct backend testing and compatibility.

---

## Troubleshooting

### Search Returns Nothing
- ✅ Check that database server is running
- ✅ Verify the database file exists and is nonempty: `ls -l /tmp/http-mdb-manual-test.mdb`
- ✅ Check HTTP server logs for errors
- ✅ To test the backend directly, stop the HTTP server first, then run:
  `printf 'SEARCH test\n' | nc localhost 9999`

### Add/Update/Delete Not Working
- ✅ Check backend connection in HTTP server logs
- ✅ Verify form data is being sent correctly
- ✅ Check backend server logs for command errors
- ✅ Ensure database file is writable

### List Shows No Records
- ✅ Verify database file exists and has data
- ✅ Check backend server is responding
- ✅ With the HTTP server stopped, test LIST directly:
  `printf 'LIST\n' | nc localhost 9999`

### Connection Errors
- ✅ Ensure database server starts BEFORE HTTP server
- ✅ Check ports are not in use: `lsof -i :9999` and `lsof -i :8080`
- ✅ Verify localhost resolution works

---

## Expected URLs Summary

| Operation | Method | URL | Description |
|-----------|--------|-----|-------------|
| Search Form | GET | `/mdb-lookup` | Display search form |
| Search Query | GET | `/mdb-lookup?key=...` | Search database |
| List All | GET | `/mdb-list` | Show all records |
| Add Form | GET | `/mdb-add` | Display add form |
| Add Record | POST | `/mdb-add` | Create new record |
| Edit Form | GET | `/mdb-edit?id=X` | Display edit form |
| Update Record | POST | `/mdb-update` | Update existing record |
| Delete Record | POST | `/mdb-delete` | Delete record |

---

## Success Criteria

✅ All operations work via web browser  
✅ All operations work via curl/command line  
✅ Search finds matching records  
✅ List shows all records  
✅ Add creates new records  
✅ Edit updates existing records  
✅ Delete removes records  
✅ Navigation links work between pages  
✅ Error handling works (invalid IDs, missing fields, etc.)  
✅ Redirects work correctly after POST operations  
✅ Stable IDs survive deletion and restart
✅ A forced persistence failure changes neither memory nor disk
✅ HTML metacharacters are escaped exactly and braces remain intact
✅ Static symlinks cannot expose files outside the web root
✅ Failed client downloads neither clobber nor leave a final destination
