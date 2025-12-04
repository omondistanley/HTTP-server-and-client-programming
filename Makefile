# Root Makefile for HTTP Server and Client Programming Project
# Builds all components: HTTP client, HTTP server, and database lookup server

.PHONY: all clean client server database

# Default target: build all components
all: client server database

# Build HTTP client
client:
	@echo "Building HTTP client..."
	cd clientserv && $(MAKE)

# Build HTTP server
server:
	@echo "Building HTTP server..."
	cd network_programming && $(MAKE)

# Build database lookup server
database:
	@echo "Building database lookup server..."
	cd searchdb && $(MAKE)

# Clean all build artifacts
clean:
	@echo "Cleaning all components..."
	cd clientserv && $(MAKE) clean
	cd network_programming && $(MAKE) clean
	cd searchdb && $(MAKE) clean

# Help target
help:
	@echo "Available targets:"
	@echo "  all      - Build all components (default)"
	@echo "  client   - Build HTTP client only"
	@echo "  server   - Build HTTP server only"
	@echo "  database - Build database lookup server only"
	@echo "  clean    - Remove all build artifacts"
	@echo "  help     - Show this help message"

