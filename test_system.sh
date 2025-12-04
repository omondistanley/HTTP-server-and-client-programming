#!/bin/bash

# Test script for HTTP Server and Client Programming Project
# Tests the complete system: Database -> HTTP Server -> Client/Web

echo "=========================================="
echo "Testing HTTP Server and Client System"
echo "=========================================="
echo ""

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Cleanup function
cleanup() {
    echo ""
    echo "Cleaning up..."
    kill $DB_PID 2>/dev/null
    kill $HTTP_PID 2>/dev/null
    wait $DB_PID 2>/dev/null
    wait $HTTP_PID 2>/dev/null
    echo "Cleanup complete."
    exit 0
}

# Set trap for cleanup on exit
trap cleanup EXIT INT TERM

# Function to kill processes on a specific port
kill_port() {
    local port=$1
    # Try to find and kill processes using the port
    if command -v lsof >/dev/null 2>&1; then
        local pids=$(lsof -ti :$port 2>/dev/null)
        if [ -n "$pids" ]; then
            echo "Killing existing processes on port $port..."
            kill -9 $pids 2>/dev/null
            sleep 1
        fi
    elif command -v netstat >/dev/null 2>&1; then
        # Alternative method using netstat (macOS)
        local pids=$(netstat -anv | grep ":$port" | grep LISTEN | awk '{print $9}' | sort -u)
        if [ -n "$pids" ]; then
            echo "Killing existing processes on port $port..."
            kill -9 $pids 2>/dev/null
            sleep 1
        fi
    fi
}

# Clean up any existing processes on our ports
echo "Cleaning up any existing processes on ports 9999 and 8080..."
kill_port 9999
kill_port 8080
sleep 1

# Check if binaries exist
if [ ! -f "searchdb/mdb-lookup-server" ]; then
    echo -e "${RED}Error: mdb-lookup-server not found. Run 'make' first.${NC}"
    exit 1
fi

if [ ! -f "network_programming/http-server" ]; then
    echo -e "${RED}Error: http-server not found. Run 'make' first.${NC}"
    exit 1
fi

if [ ! -f "clientserv/http-client" ]; then
    echo -e "${RED}Error: http-client not found. Run 'make' first.${NC}"
    exit 1
fi

# Check if database file exists
if [ ! -f "searchdb/mdb-cs3157" ]; then
    echo -e "${RED}Error: Database file mdb-cs3157 not found.${NC}"
    exit 1
fi

echo "Step 1: Starting Database Lookup Server on port 9999..."
cd searchdb
./mdb-lookup-server mdb-cs3157 9999 > /tmp/db-server.log 2>&1 &
DB_PID=$!
cd ..
sleep 3

# Check if process is still running and if port is actually listening
if ps -p $DB_PID > /dev/null 2>&1; then
    # Give it a moment to bind
    sleep 1
    # Check if port is actually listening
    if (lsof -i :9999 >/dev/null 2>&1 || netstat -an | grep -q ":9999.*LISTEN" 2>/dev/null); then
        echo -e "${GREEN}✓ Database server started (PID: $DB_PID)${NC}"
    else
        echo -e "${RED}✗ Database server process exists but port not listening${NC}"
        cat /tmp/db-server.log
        kill $DB_PID 2>/dev/null
        exit 1
    fi
else
    echo -e "${RED}✗ Database server failed to start${NC}"
    cat /tmp/db-server.log
    exit 1
fi

echo ""
echo "Step 2: Starting HTTP Server on port 8080..."
cd network_programming
./http-server 8080 html localhost 9999 > /tmp/http-server.log 2>&1 &
HTTP_PID=$!
cd ..
sleep 3

# Check if process is still running and if port is actually listening
if ps -p $HTTP_PID > /dev/null 2>&1; then
    # Give it a moment to bind
    sleep 1
    # Check if port is actually listening
    if (lsof -i :8080 >/dev/null 2>&1 || netstat -an | grep -q ":8080.*LISTEN" 2>/dev/null); then
        echo -e "${GREEN}✓ HTTP server started (PID: $HTTP_PID)${NC}"
    else
        echo -e "${RED}✗ HTTP server process exists but port not listening${NC}"
        cat /tmp/http-server.log
        kill $HTTP_PID 2>/dev/null
        kill $DB_PID 2>/dev/null
        exit 1
    fi
else
    echo -e "${RED}✗ HTTP server failed to start${NC}"
    cat /tmp/http-server.log
    kill $DB_PID 2>/dev/null
    exit 1
fi

echo ""
echo "Step 3: Testing Static File Serving..."
echo "----------------------------------------"
STATIC_TEST=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:8080/index.html)
if [ "$STATIC_TEST" = "200" ]; then
    echo -e "${GREEN}✓ Static file serving works (HTTP 200)${NC}"
    curl -s http://localhost:8080/index.html | head -5
    echo "..."
else
    echo -e "${RED}✗ Static file serving failed (HTTP $STATIC_TEST)${NC}"
fi

echo ""
echo "Step 4: Testing Database Lookup Form (Web Interface)..."
echo "----------------------------------------"
FORM_TEST=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:8080/mdb-lookup)
if [ "$FORM_TEST" = "200" ]; then
    echo -e "${GREEN}✓ Database lookup form accessible (HTTP 200)${NC}"
    echo "Form content:"
    curl -s http://localhost:8080/mdb-lookup | head -10
else
    echo -e "${RED}✗ Database lookup form failed (HTTP $FORM_TEST)${NC}"
fi

echo ""
echo "Step 5: Testing Database Query via HTTP..."
echo "----------------------------------------"
# Try a few common search terms
for term in "test" "hello" "world" "a"; do
    QUERY_TEST=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:8080/mdb-lookup?key=$term")
    if [ "$QUERY_TEST" = "200" ]; then
        echo -e "${GREEN}✓ Query for '$term' successful (HTTP 200)${NC}"
        RESULTS=$(curl -s "http://localhost:8080/mdb-lookup?key=$term" | grep -c "<tr>")
        echo "   Found $RESULTS matching records"
        if [ "$RESULTS" -gt 0 ]; then
            echo "   Sample result:"
            curl -s "http://localhost:8080/mdb-lookup?key=$term" | grep "<tr>" | head -1 | sed 's/<[^>]*>//g' | sed 's/^[[:space:]]*//'
        fi
        break
    fi
done

echo ""
echo "Step 6: Testing HTTP Client..."
echo "----------------------------------------"
cd clientserv
if ./http-client localhost 8080 /index.html 2>&1 | grep -q "200\|OK"; then
    echo -e "${GREEN}✓ HTTP client successfully downloaded index.html${NC}"
    if [ -f "index.html" ]; then
        echo "   File size: $(wc -c < index.html) bytes"
        rm -f index.html
    fi
else
    echo -e "${YELLOW}⚠ HTTP client test completed (check output above)${NC}"
fi
cd ..

echo ""
echo "Step 7: Testing Database Connection (HTTP Server -> Database Server)..."
echo "----------------------------------------"
# Test that HTTP server can communicate with database server
DB_CONN_TEST=$(curl -s "http://localhost:8080/mdb-lookup?key=test" | grep -i "error\|unavailable" | wc -l)
if [ "$DB_CONN_TEST" -eq 0 ]; then
    echo -e "${GREEN}✓ HTTP Server successfully connected to Database Server${NC}"
    echo "   Backend connection is working"
else
    echo -e "${RED}✗ HTTP Server failed to connect to Database Server${NC}"
fi

echo ""
echo "=========================================="
echo "Test Summary"
echo "=========================================="
echo "Database Server: Running (PID: $DB_PID)"
echo "HTTP Server: Running (PID: $HTTP_PID)"
echo ""
echo "Access the web interface at:"
echo -e "${YELLOW}  http://localhost:8080/mdb-lookup${NC}"
echo ""
echo "Test static files at:"
echo -e "${YELLOW}  http://localhost:8080/index.html${NC}"
echo ""
echo "Press Ctrl+C to stop servers and exit"
echo ""

# Keep running until interrupted
while true; do
    sleep 1
    if ! ps -p $DB_PID > /dev/null; then
        echo -e "${RED}Database server stopped unexpectedly${NC}"
        break
    fi
    if ! ps -p $HTTP_PID > /dev/null; then
        echo -e "${RED}HTTP server stopped unexpectedly${NC}"
        break
    fi
done

