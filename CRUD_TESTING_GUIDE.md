# Complete CRUD Operations Testing Guide

This guide shows you how to test all database operations: Search, List, Add, Update, and Delete.

## Prerequisites

1. **Start the Database Server** (Terminal 1):
```bash
cd searchdb
./mdb-lookup-server mdb-cs3157 9999
```

2. **Start the HTTP Server** (Terminal 2):
```bash
cd network_programming
./http-server 8080 html localhost 9999
```

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
   - **Name:** (max 15 characters)
   - **Message:** (max 23 characters)
3. Click "Add"
4. **Expected:** Redirects to `/mdb-list` showing the new record

**Via Command Line (POST):**
```bash
# Add a new record
curl -X POST "http://localhost:8080/mdb-add" \
  -d "name=John&msg=Hello World" \
  -L  # Follow redirects

# Add another record
curl -X POST "http://localhost:8080/mdb-add" \
  -d "name=Jane&msg=Testing 123" \
  -L
```

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
# First, get the edit form (replace X with actual ID)
curl "http://localhost:8080/mdb-edit?id=1"

# Update a record (replace X with actual ID)
curl -X POST "http://localhost:8080/mdb-update" \
  -d "id=1&name=UpdatedName&msg=Updated Message" \
  -L
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
# Delete a record (replace X with actual ID)
curl -X POST "http://localhost:8080/mdb-delete" \
  -d "id=1" \
  -L
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
# 1. Start both servers (in separate terminals)
# Terminal 1:
cd searchdb && ./mdb-lookup-server mdb-cs3157 9999

# Terminal 2:
cd network_programming && ./http-server 8080 html localhost 9999

# 2. Test Search
curl "http://localhost:8080/mdb-lookup?key=test"

# 3. Test List All
curl http://localhost:8080/mdb-list

# 4. Test Add
curl -X POST "http://localhost:8080/mdb-add" -d "name=TestUser&msg=TestMessage" -L

# 5. Test Search again (should find the new record)
curl "http://localhost:8080/mdb-lookup?key=TestUser"

# 6. Test Edit (get ID from list, then update)
curl "http://localhost:8080/mdb-edit?id=1"  # View edit form
curl -X POST "http://localhost:8080/mdb-update" -d "id=1&name=Updated&msg=NewMsg" -L

# 7. Test Delete
curl -X POST "http://localhost:8080/mdb-delete" -d "id=1" -L

# 8. Verify deletion
curl "http://localhost:8080/mdb-lookup?key=Updated"  # Should not find it
```

---

## Troubleshooting

### Search Returns Nothing
- ✅ Check that database server is running
- ✅ Verify database file has data: `cat searchdb/mdb-cs3157`
- ✅ Check HTTP server logs for errors
- ✅ Test backend directly: `echo "SEARCH test" | nc localhost 9999`

### Add/Update/Delete Not Working
- ✅ Check backend connection in HTTP server logs
- ✅ Verify form data is being sent correctly
- ✅ Check backend server logs for command errors
- ✅ Ensure database file is writable

### List Shows No Records
- ✅ Verify database file exists and has data
- ✅ Check backend server is responding
- ✅ Test LIST command directly: `echo "LIST" | nc localhost 9999`

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

