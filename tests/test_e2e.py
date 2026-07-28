import hashlib
import html
import http.client
import os
from pathlib import Path
import re
import socket
import stat
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from urllib.parse import urlencode


PROJECT_ROOT = Path(__file__).resolve().parents[1]
HTTP_SERVER = Path(
    os.environ.get(
        "HTTP_SERVER_BIN", PROJECT_ROOT / "network_programming" / "http-server"
    )
)
DB_SERVER = Path(
    os.environ.get(
        "DB_SERVER_BIN", PROJECT_ROOT / "searchdb" / "mdb-lookup-server"
    )
)
HTTP_CLIENT = Path(
    os.environ.get("HTTP_CLIENT_BIN", PROJECT_ROOT / "clientserv" / "http-client")
)

FORM_CONTENT_TYPE = "application/x-www-form-urlencoded"
MDB2_MAGIC = b"MDB2\r\n\x1a\n"
MDB2_VERSION = 1
MDB2_HEADER = struct.Struct("<8sIQQ")
MDB2_RECORD = struct.Struct("<Q16s24s")


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_port(port, process, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"process exited with status {process.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise TimeoutError(f"port {port} did not become ready")


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def make_database(path, record_count):
    records = [
        ("RouteAlpha", "KnownMessage"),
        ("SecondRecord", "OtherMessage"),
    ]
    for index in range(2, record_count):
        records.append((f"User{index:010d}"[:15], f"Message{index:016d}"[:23]))

    with path.open("wb") as database:
        for name, message in records:
            database.write(
                struct.pack(
                    "=16s24s",
                    name.encode("ascii"),
                    message.encode("ascii"),
                )
            )


def write_legacy_database(path, records):
    with path.open("wb") as database:
        for name, message in records:
            database.write(
                struct.pack(
                    "=16s24s",
                    name.encode("ascii"),
                    message.encode("ascii"),
                )
            )


def parse_mdb2(data):
    if len(data) < MDB2_HEADER.size:
        raise ValueError("MDB2 header is truncated")

    magic, version, next_id, record_count = MDB2_HEADER.unpack_from(data)
    if magic != MDB2_MAGIC:
        raise ValueError("invalid MDB2 magic")
    if version != MDB2_VERSION:
        raise ValueError("unsupported MDB2 version")

    expected_size = MDB2_HEADER.size + (record_count * MDB2_RECORD.size)
    if len(data) != expected_size:
        raise ValueError("MDB2 record count does not match the file size")

    records = []
    offset = MDB2_HEADER.size
    for _ in range(record_count):
        record_id, raw_name, raw_message = MDB2_RECORD.unpack_from(data, offset)
        records.append(
            (
                record_id,
                raw_name.split(b"\0", 1)[0].decode("ascii"),
                raw_message.split(b"\0", 1)[0].decode("ascii"),
            )
        )
        offset += MDB2_RECORD.size
    return next_id, records


def parse_http_response(raw_response):
    header_block, separator, body = raw_response.partition(b"\r\n\r\n")
    if not separator:
        raise AssertionError(f"incomplete HTTP response: {raw_response[:200]!r}")

    lines = header_block.split(b"\r\n")
    status_parts = lines[0].split(None, 2)
    if len(status_parts) < 2 or not status_parts[1].isdigit():
        raise AssertionError(f"invalid HTTP status line: {lines[0]!r}")

    headers = {}
    for line in lines[1:]:
        name, colon, value = line.partition(b":")
        if not colon:
            continue
        headers[name.decode("ascii").strip().lower()] = (
            value.decode("latin-1").strip()
        )
    return int(status_parts[1]), headers, body


def assert_database_start_rejected(test_case, database):
    port = unused_port()
    process = subprocess.Popen(
        [str(DB_SERVER), str(database), str(port)],
        cwd=PROJECT_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    try:
        deadline = time.monotonic() + 3
        while process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)

        if process.poll() is None:
            test_case.fail("database server accepted a malformed MDB2 file")

        output, _ = process.communicate(timeout=1)
        test_case.assertNotEqual(process.returncode, 0, output.decode(errors="replace"))
    finally:
        stop_process(process)


class OneShotHttpServer:
    def __init__(self, response, response_gate=None):
        self.response = response
        self.response_gate = response_gate
        self.listener = None
        self.port = None
        self.thread = None
        self.stop_event = threading.Event()
        self.request_received = threading.Event()
        self.error = None

    def __enter__(self):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()
        return self

    def _serve(self):
        connection = None
        try:
            while not self.stop_event.is_set():
                try:
                    connection, _ = self.listener.accept()
                    break
                except socket.timeout:
                    continue
                except OSError:
                    if self.stop_event.is_set():
                        return
                    raise

            if connection is None:
                return

            with connection:
                connection.settimeout(2)
                request = bytearray()
                while b"\r\n\r\n" not in request and len(request) < 16384:
                    chunk = connection.recv(4096)
                    if not chunk:
                        break
                    request.extend(chunk)
                self.request_received.set()
                if self.response_gate is not None:
                    while not self.response_gate.wait(timeout=0.1):
                        if self.stop_event.is_set():
                            return
                connection.sendall(self.response)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except BaseException as error:
            self.error = error

    def __exit__(self, exc_type, exc_value, traceback):
        self.stop_event.set()
        if self.listener is not None:
            self.listener.close()
        if self.thread is not None:
            self.thread.join(timeout=3)
            if self.thread.is_alive() and exc_type is None:
                raise RuntimeError("test HTTP server thread did not stop")
        if self.error is not None and exc_type is None:
            raise self.error


class RunningSystem:
    def __init__(self, record_count=32):
        self.record_count = record_count
        self.temp_dir = None
        self.db_process = None
        self.http_process = None
        self.db_log = None
        self.http_log = None

    def __enter__(self):
        self.temp_dir = tempfile.TemporaryDirectory(prefix="http-project-test-")
        root = Path(self.temp_dir.name)
        self.root = root
        self.database_dir = root / "database"
        self.database_dir.mkdir()
        self.database = self.database_dir / "test.mdb"
        self.web_root = root / "html"
        self.web_root.mkdir()
        self.index_body = b"routing-and-socket-regression\n"
        (self.web_root / "index.html").write_bytes(self.index_body)
        (self.web_root / "large.bin").write_bytes(b"0123456789abcdef" * 131072)
        make_database(self.database, self.record_count)

        self.db_port = unused_port()
        self.http_port = unused_port()
        while self.http_port == self.db_port:
            self.http_port = unused_port()
        self.db_log = (root / "database.log").open("wb")
        self.http_log = (root / "http.log").open("wb")

        try:
            self.start_database()
            self.start_http()
        except BaseException:
            self.stop_servers()
            self.http_log.close()
            self.db_log.close()
            self.temp_dir.cleanup()
            raise
        return self

    def start_database(self):
        self.db_process = subprocess.Popen(
            [str(DB_SERVER), str(self.database), str(self.db_port)],
            cwd=PROJECT_ROOT,
            stdout=self.db_log,
            stderr=subprocess.STDOUT,
        )
        wait_for_port(self.db_port, self.db_process)

    def start_http(self):
        self.http_process = subprocess.Popen(
            [
                str(HTTP_SERVER),
                str(self.http_port),
                str(self.web_root),
                "127.0.0.1",
                str(self.db_port),
            ],
            cwd=PROJECT_ROOT,
            stdout=self.http_log,
            stderr=subprocess.STDOUT,
        )
        wait_for_port(self.http_port, self.http_process)

    def stop_servers(self):
        stop_process(self.http_process)
        stop_process(self.db_process)

    def restart(self):
        self.stop_servers()
        self.start_database()
        self.start_http()

    def __exit__(self, exc_type, exc_value, traceback):
        self.stop_servers()
        if self.http_log:
            self.http_log.close()
        if self.db_log:
            self.db_log.close()

        if exc_type is not None:
            root = Path(self.temp_dir.name)
            for log_name in ("http.log", "database.log"):
                log_path = root / log_name
                if log_path.exists():
                    print(f"\n--- {log_name} ---")
                    print(log_path.read_text(errors="replace"))

        self.temp_dir.cleanup()

    def request(self, method, target, body=None, headers=None):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.http_port, timeout=5
        )
        try:
            connection.request(method, target, body=body, headers=headers or {})
            response = connection.getresponse()
            response_body = response.read()
            return response.status, dict(response.getheaders()), response_body
        finally:
            connection.close()

    def post_form(self, target, fields):
        body = urlencode(fields)
        return self.post_raw_form(target, body)

    def post_raw_form(self, target, body):
        return self.request(
            "POST",
            target,
            body,
            {
                "Content-Type": FORM_CONTENT_TYPE,
                "Content-Length": str(len(body)),
            },
        )

    def list_snapshot(self):
        status, _, body = self.request("GET", "/mdb-list")
        if status != 200:
            raise AssertionError(f"list returned {status}")
        return body

    def database_digest(self):
        return hashlib.sha256(self.database.read_bytes()).digest()

    def list_records(self):
        body = self.list_snapshot()
        records = {}
        row_pattern = re.compile(
            rb"<tr><td>([0-9]+)</td><td>(.*?)</td><td>(.*?)</td>",
            re.DOTALL,
        )
        for raw_id, raw_name, raw_message in row_pattern.findall(body):
            name = html.unescape(raw_name.decode("utf-8"))
            message = html.unescape(raw_message.decode("utf-8"))
            records[name] = (int(raw_id), message)
        return records

    def raw_request(self, request_bytes):
        with socket.create_connection(
            ("127.0.0.1", self.http_port), timeout=3
        ) as sock:
            sock.sendall(request_bytes)
            sock.shutdown(socket.SHUT_WR)
            sock.settimeout(3)
            response = bytearray()
            while True:
                try:
                    chunk = sock.recv(4096)
                except ConnectionResetError:
                    break
                if not chunk:
                    break
                response.extend(chunk)
        return parse_http_response(bytes(response))

    def reset_request(self, target):
        sock = socket.create_connection(("127.0.0.1", self.http_port), timeout=2)
        sock.sendall(
            f"GET {target} HTTP/1.0\r\nHost: localhost\r\n\r\n".encode("ascii")
        )
        sock.setsockopt(
            socket.SOL_SOCKET,
            socket.SO_LINGER,
            struct.pack("ii", 1, 0),
        )
        sock.close()


class RoutingTests(unittest.TestCase):
    def test_search_form_results_parameter_order_and_not_found(self):
        with RunningSystem() as system:
            status, _, form = system.request("GET", "/mdb-lookup")
            self.assertEqual(status, 200)
            self.assertIn(b"name=key", form)
            self.assertNotIn(b"<table", form)

            status, _, results = system.request(
                "GET", "/mdb-lookup?unused=x&key=routealpha"
            )
            self.assertEqual(status, 200)
            self.assertIn(b"<table", results)
            self.assertIn(b"RouteAlpha", results)

            status, _, missing = system.request(
                "GET", "/mdb-lookup?key=NeverPresent987"
            )
            self.assertEqual(status, 200)
            self.assertIn(b"ENTRY NOT FOUND", missing)

    def test_search_rejects_missing_empty_duplicate_and_malformed_keys(self):
        with RunningSystem() as system:
            targets = [
                "/mdb-lookup?unused=x",
                "/mdb-lookup?key=",
                "/mdb-lookup?key=+++",
                "/mdb-lookup?key=one&key=two",
                "/mdb-lookup?key=%",
                "/mdb-lookup?key=%0",
                "/mdb-lookup?key=%GG",
            ]
            for target in targets:
                with self.subTest(target=target):
                    status, _, _ = system.request("GET", target)
                    self.assertEqual(status, 400)

    def test_edit_route_drains_backend_response(self):
        with RunningSystem() as system:
            status, _, edit = system.request("GET", "/mdb-edit?unused=x&id=1")
            self.assertEqual(status, 200)
            self.assertIn(b"RouteAlpha", edit)
            self.assertIn(b"value=1", edit)

            status, _, search = system.request(
                "GET", "/mdb-lookup?key=KnownMessage"
            )
            self.assertEqual(status, 200)
            self.assertIn(b"RouteAlpha", search)

            status, _, listing = system.request("GET", "/mdb-list")
            self.assertEqual(status, 200)
            self.assertIn(b"SecondRecord", listing)

    def test_edit_rejects_invalid_ids_and_distinguishes_missing_record(self):
        with RunningSystem() as system:
            invalid_targets = [
                "/mdb-edit",
                "/mdb-edit?id=",
                "/mdb-edit?id=abc",
                "/mdb-edit?id=1abc",
                "/mdb-edit?id=0",
                "/mdb-edit?id=-1",
                "/mdb-edit?id=999999999999999999999999",
            ]
            for target in invalid_targets:
                with self.subTest(target=target):
                    status, _, _ = system.request("GET", target)
                    self.assertEqual(status, 400)

            status, _, _ = system.request("GET", "/mdb-edit?id=999999")
            self.assertEqual(status, 404)


class ProtocolValidationTests(unittest.TestCase):
    def assert_rejected_without_mutation(self, system, body):
        before_list = system.list_snapshot()
        before_digest = system.database_digest()
        status, _, _ = system.post_raw_form("/mdb-add", body)
        self.assertEqual(status, 400, body)
        self.assertEqual(system.list_snapshot(), before_list, body)
        self.assertEqual(system.database_digest(), before_digest, body)
        status, _, search = system.request("GET", "/mdb-lookup?key=RouteAlpha")
        self.assertEqual(status, 200)
        self.assertIn(b"RouteAlpha", search)

    def test_add_rejects_injection_malformed_empty_and_overlong_fields(self):
        with RunningSystem() as system:
            invalid_bodies = [
                "name=&msg=Message",
                "name=Name&msg=",
                "name=+++&msg=Message",
                "name=Bad%0ADELETE+1&msg=x",
                "name=Bad%0DDELETE+1&msg=x",
                "name=Bad%7CName&msg=x",
                "name=Bad%00Name&msg=x",
                "name=Bad%01Name&msg=x",
                b"name=Valid&msg=x\0ignored",
                "name=%GG&msg=x",
                f"name={'A' * 16}&msg=x",
                f"name=Valid&msg={'B' * 24}",
                "name=one&name=two&msg=x",
            ]
            for body in invalid_bodies:
                with self.subTest(body=body):
                    self.assert_rejected_without_mutation(system, body)

    def test_boundary_length_add_is_accepted_and_persisted(self):
        with RunningSystem() as system:
            status, headers, _ = system.post_form(
                "/mdb-add",
                {"name": "N" * 15, "msg": "M" * 23},
            )
            self.assertEqual(status, 302)
            self.assertEqual(headers.get("Location"), "/mdb-list")
            self.assertIn(b"N" * 15, system.list_snapshot())

            disk = system.database.read_bytes()
            self.assertIn((b"N" * 15) + b"\0" + (b"M" * 23) + b"\0", disk)

    def test_update_and_delete_use_strict_ids_and_field_validation(self):
        with RunningSystem() as system:
            before = system.list_snapshot()
            before_digest = system.database_digest()

            invalid_updates = [
                "id=1abc&name=Valid&msg=Message",
                "id=-1&name=Valid&msg=Message",
                "id=1&name=Bad%7CName&msg=Message",
                "id=1&name=Valid&msg=Bad%0AMessage",
                f"id=1&name={'X' * 16}&msg=Message",
            ]
            for body in invalid_updates:
                with self.subTest(body=body):
                    status, _, _ = system.post_raw_form("/mdb-update", body)
                    self.assertEqual(status, 400)

            for invalid_id in ("", "abc", "1abc", "-1", "0", "999999999999999999999"):
                with self.subTest(invalid_id=invalid_id):
                    status, _, _ = system.post_raw_form(
                        "/mdb-delete", f"id={invalid_id}"
                    )
                    self.assertEqual(status, 400)

            self.assertEqual(system.list_snapshot(), before)
            self.assertEqual(system.database_digest(), before_digest)

    def test_valid_update_and_delete_complete_and_persist(self):
        with RunningSystem() as system:
            status, headers, _ = system.post_form(
                "/mdb-update",
                {"id": "1", "name": "UpdatedName", "msg": "UpdatedMessage"},
            )
            self.assertEqual(status, 302)
            self.assertEqual(headers.get("Location"), "/mdb-list")
            self.assertIn(b"UpdatedName", system.list_snapshot())
            self.assertIn(b"UpdatedName", system.database.read_bytes())

            status, _, edit = system.request("GET", "/mdb-edit?id=1")
            self.assertEqual(status, 200)
            self.assertIn(b"UpdatedMessage", edit)

            status, headers, _ = system.post_form("/mdb-delete", {"id": "1"})
            self.assertEqual(status, 302)
            self.assertEqual(headers.get("Location"), "/mdb-list")
            self.assertNotIn(b"UpdatedName", system.list_snapshot())
            self.assertNotIn(b"UpdatedName", system.database.read_bytes())

    def test_search_command_words_are_literal_and_newlines_are_rejected(self):
        with RunningSystem() as system:
            before = system.list_snapshot()
            before_digest = system.database_digest()

            for key in ("DELETE+1", "LIST", "SAVE"):
                with self.subTest(key=key):
                    status, _, body = system.request(
                        "GET", f"/mdb-lookup?key={key}"
                    )
                    self.assertEqual(status, 200)
                    self.assertIn(b"ENTRY NOT FOUND", body)

            status, _, _ = system.request(
                "GET", "/mdb-lookup?key=safe%0ADELETE+1"
            )
            self.assertEqual(status, 400)
            self.assertEqual(system.list_snapshot(), before)
            self.assertEqual(system.database_digest(), before_digest)


class StructuredBackendRenderingTests(unittest.TestCase):
    def test_special_characters_are_escaped_exactly_everywhere(self):
        with RunningSystem() as system:
            name = """Esc<>&"'"""
            message = """Msg<>&"'"""
            escaped_name = b"Esc&lt;&gt;&amp;&quot;&#39;"
            escaped_message = b"Msg&lt;&gt;&amp;&quot;&#39;"

            status, _, _ = system.post_form(
                "/mdb-add",
                {"name": name, "msg": message},
            )
            self.assertEqual(status, 302)

            records = system.list_records()
            self.assertIn(name, records)
            record_id = records[name][0]

            status, _, listing = system.request("GET", "/mdb-list")
            self.assertEqual(status, 200)
            self.assertIn(escaped_name, listing)
            self.assertIn(escaped_message, listing)
            self.assertNotIn(name.encode("ascii"), listing)
            self.assertNotIn(message.encode("ascii"), listing)

            status, _, edit = system.request(
                "GET",
                f"/mdb-edit?id={record_id}",
            )
            self.assertEqual(status, 200)
            self.assertIn(b'value="' + escaped_name + b'"', edit)
            self.assertIn(b'value="' + escaped_message + b'"', edit)
            self.assertNotIn(name.encode("ascii"), edit)
            self.assertNotIn(message.encode("ascii"), edit)

            status, _, search = system.request("GET", "/mdb-lookup?key=Esc")
            self.assertEqual(status, 200)
            self.assertIn(escaped_name, search)
            self.assertIn(escaped_message, search)
            self.assertNotIn(name.encode("ascii"), search)
            self.assertNotIn(message.encode("ascii"), search)

    def test_reflected_search_key_is_escaped_in_form_value(self):
        with RunningSystem() as system:
            key = """<>&"'"""
            escaped_key = b"&lt;&gt;&amp;&quot;&#39;"

            status, _, body = system.request(
                "GET",
                "/mdb-lookup?" + urlencode({"key": key}),
            )

            self.assertEqual(status, 200)
            self.assertIn(
                b'name=key value="' + escaped_key + b'">',
                body,
            )
            self.assertNotIn(key.encode("ascii"), body)

    def test_closing_braces_survive_complete_crud_workflow(self):
        with RunningSystem() as system:
            original_name = "Brace}Name"
            original_message = "Brace}Message"
            updated_name = "New}Name"
            updated_message = "New}Message"

            status, _, _ = system.post_form(
                "/mdb-add",
                {"name": original_name, "msg": original_message},
            )
            self.assertEqual(status, 302)

            records = system.list_records()
            self.assertIn(original_name, records)
            record_id = records[original_name][0]
            self.assertEqual(records[original_name][1], original_message)

            status, _, edit = system.request(
                "GET",
                f"/mdb-edit?id={record_id}",
            )
            self.assertEqual(status, 200)
            self.assertIn(
                f'value="{original_name}"'.encode("ascii"),
                edit,
            )
            self.assertIn(
                f'value="{original_message}"'.encode("ascii"),
                edit,
            )

            status, _, search = system.request(
                "GET",
                "/mdb-lookup?" + urlencode({"key": original_name}),
            )
            self.assertEqual(status, 200)
            self.assertIn(original_name.encode("ascii"), search)
            self.assertIn(original_message.encode("ascii"), search)

            status, _, _ = system.post_form(
                "/mdb-update",
                {
                    "id": str(record_id),
                    "name": updated_name,
                    "msg": updated_message,
                },
            )
            self.assertEqual(status, 302)

            records = system.list_records()
            self.assertNotIn(original_name, records)
            self.assertIn(updated_name, records)
            self.assertEqual(records[updated_name], (record_id, updated_message))

            status, _, _ = system.post_form(
                "/mdb-delete",
                {"id": str(record_id)},
            )
            self.assertEqual(status, 302)
            self.assertNotIn(updated_name, system.list_records())

    def test_http_routes_preserve_legacy_empty_fields(self):
        legacy_records = [
            ("", "MessageOnly"),
            ("NameOnly", ""),
            ("", ""),
        ]

        with RunningSystem(record_count=2) as system:
            system.stop_servers()
            write_legacy_database(system.database, legacy_records)
            system.start_database()
            system.start_http()

            status, _, listing = system.request("GET", "/mdb-list")
            self.assertEqual(status, 200)
            self.assertIn(
                b"<tr><td>1</td><td></td><td>MessageOnly</td>",
                listing,
            )
            self.assertIn(
                b"<tr><td>2</td><td>NameOnly</td><td></td>",
                listing,
            )
            self.assertIn(
                b"<tr><td>3</td><td></td><td></td>",
                listing,
            )

            search_cases = [
                (
                    "MessageOnly",
                    b"<tr><td>1</td><td>1</td><td></td>"
                    b"<td>MessageOnly</td></tr>",
                ),
                (
                    "NameOnly",
                    b"<tr><td>1</td><td>2</td><td>NameOnly</td>"
                    b"<td></td></tr>",
                ),
            ]
            for key, expected_row in search_cases:
                with self.subTest(route="search", key=key):
                    status, _, body = system.request(
                        "GET",
                        "/mdb-lookup?" + urlencode({"key": key}),
                    )
                    self.assertEqual(status, 200)
                    self.assertIn(expected_row, body)

            edit_cases = [
                (1, b'name=name value=""', b'name=msg value="MessageOnly"'),
                (2, b'name=name value="NameOnly"', b'name=msg value=""'),
                (3, b'name=name value=""', b'name=msg value=""'),
            ]
            for record_id, expected_name, expected_message in edit_cases:
                with self.subTest(route="edit", record_id=record_id):
                    status, _, body = system.request(
                        "GET",
                        f"/mdb-edit?id={record_id}",
                    )
                    self.assertEqual(status, 200)
                    self.assertIn(expected_name, body)
                    self.assertIn(expected_message, body)


class HttpRequestValidationTests(unittest.TestCase):
    def test_form_posts_require_supported_content_type(self):
        with RunningSystem() as system:
            body = "name=Valid&msg=Message"

            status, _, _ = system.request(
                "POST",
                "/mdb-add",
                body,
                {"Content-Length": str(len(body))},
            )
            self.assertEqual(status, 415)

            status, _, _ = system.request(
                "POST",
                "/mdb-add",
                body,
                {
                    "Content-Type": "application/json",
                    "Content-Length": str(len(body)),
                },
            )
            self.assertEqual(status, 415)

            status, _, _ = system.request(
                "POST",
                "/mdb-add",
                body,
                {
                    "Content-Type": (
                        "application/x-www-form-urlencoded; charset=UTF-8"
                    ),
                    "Content-Length": str(len(body)),
                },
            )
            self.assertEqual(status, 302)

    def test_duplicate_and_conflicting_content_length_are_rejected(self):
        with RunningSystem() as system:
            body = b"name=Valid&msg=Message"
            requests = [
                (
                    b"POST /mdb-add HTTP/1.1\r\n"
                    b"Host: localhost\r\n"
                    b"Content-Type: application/x-www-form-urlencoded\r\n"
                    + f"Content-Length: {len(body)}\r\n".encode("ascii")
                    + f"Content-Length: {len(body)}\r\n".encode("ascii")
                    + b"\r\n"
                    + body
                ),
                (
                    b"POST /mdb-add HTTP/1.1\r\n"
                    b"Host: localhost\r\n"
                    b"Content-Type: application/x-www-form-urlencoded\r\n"
                    + f"Content-Length: {len(body)}\r\n".encode("ascii")
                    + b"Content-Length: 1\r\n"
                    b"\r\n"
                    + body
                ),
            ]

            for request in requests:
                with self.subTest(request=request[:120]):
                    status, _, _ = system.raw_request(request)
                    self.assertEqual(status, 400)

            self.assertNotIn(b"Valid", system.list_snapshot())

    def test_transfer_encoding_and_ambiguous_framing_are_rejected(self):
        with RunningSystem() as system:
            chunked_request = (
                b"POST /mdb-add HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Type: application/x-www-form-urlencoded\r\n"
                b"Transfer-Encoding: chunked\r\n"
                b"\r\n"
                b"16\r\nname=Valid&msg=Message\r\n"
                b"0\r\n\r\n"
            )
            status, _, _ = system.raw_request(chunked_request)
            self.assertEqual(status, 501)

            body = b"name=Valid&msg=Message"
            ambiguous_request = (
                b"POST /mdb-add HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Type: application/x-www-form-urlencoded\r\n"
                b"Transfer-Encoding: chunked\r\n"
                + f"Content-Length: {len(body)}\r\n".encode("ascii")
                + b"\r\n"
                + body
            )
            status, _, _ = system.raw_request(ambiguous_request)
            self.assertEqual(status, 400)
            self.assertNotIn(b"Valid", system.list_snapshot())

    def test_missing_content_length_is_rejected(self):
        with RunningSystem() as system:
            request = (
                b"POST /mdb-add HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Type: application/x-www-form-urlencoded\r\n"
                b"\r\n"
                b"name=Valid&msg=Message"
            )
            status, _, _ = system.raw_request(request)
            self.assertEqual(status, 411)

    def test_oversized_body_and_headers_are_rejected(self):
        with RunningSystem() as system:
            body = b"name=Valid&msg=" + (b"x" * 4096)
            oversized_body = (
                b"POST /mdb-add HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Type: application/x-www-form-urlencoded\r\n"
                + f"Content-Length: {len(body)}\r\n".encode("ascii")
                + b"\r\n"
                + body
            )
            status, _, _ = system.raw_request(oversized_body)
            self.assertEqual(status, 413)

            oversized_line = (
                b"POST /mdb-add HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"X-Oversized: "
                + (b"x" * 8192)
                + b"\r\n\r\n"
            )
            status, _, _ = system.raw_request(oversized_line)
            self.assertEqual(status, 431)

            aggregate_headers = (
                b"POST /mdb-add HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                b"Content-Type: application/x-www-form-urlencoded\r\n"
                b"Content-Length: 0\r\n"
                + b"".join(
                    f"X-Fill-{index}: ".encode("ascii")
                    + (b"x" * 1000)
                    + b"\r\n"
                    for index in range(33)
                )
                + b"\r\n"
            )
            status, _, _ = system.raw_request(aggregate_headers)
            self.assertEqual(status, 431)

    def test_post_to_static_resource_returns_method_not_allowed(self):
        with RunningSystem() as system:
            status, headers, _ = system.request(
                "POST",
                "/index.html",
                b"",
                {
                    "Content-Type": FORM_CONTENT_TYPE,
                    "Content-Length": "0",
                },
            )
            self.assertEqual(status, 405)
            self.assertEqual(headers.get("Allow"), "GET")

    def test_successful_add_still_redirects_to_list(self):
        with RunningSystem() as system:
            status, headers, _ = system.post_form(
                "/mdb-add",
                {"name": "RedirectCheck", "msg": "Created"},
            )
            self.assertEqual(status, 302)
            self.assertEqual(headers.get("Location"), "/mdb-list")
            self.assertIn("RedirectCheck", system.list_records())


class PersistenceAndStableIdTests(unittest.TestCase):
    def test_http_crud_preserves_ids_above_signed_integer_range(self):
        with RunningSystem(record_count=2) as system:
            record_id = (1 << 63) + 17
            name = b"WideIdentifier"
            message = b"StillAddressable"
            mdb2 = MDB2_HEADER.pack(
                MDB2_MAGIC,
                MDB2_VERSION,
                record_id + 1,
                1,
            ) + MDB2_RECORD.pack(
                record_id,
                name.ljust(16, b"\0"),
                message.ljust(24, b"\0"),
            )

            system.stop_servers()
            system.database.write_bytes(mdb2)
            system.start_database()
            system.start_http()

            records = system.list_records()
            self.assertEqual(records["WideIdentifier"][0], record_id)

            status, _, edit = system.request(
                "GET",
                f"/mdb-edit?id={record_id}",
            )
            self.assertEqual(status, 200)
            self.assertIn(str(record_id).encode("ascii"), edit)

            status, _, _ = system.post_form(
                "/mdb-update",
                {
                    "id": str(record_id),
                    "name": "WideUpdated",
                    "msg": "StillStable",
                },
            )
            self.assertEqual(status, 302)
            self.assertEqual(
                system.list_records()["WideUpdated"][0],
                record_id,
            )

            status, _, _ = system.post_form(
                "/mdb-delete",
                {"id": str(record_id)},
            )
            self.assertEqual(status, 302)
            self.assertNotIn("WideUpdated", system.list_records())

    def test_legacy_database_migrates_lazily_on_first_mutation(self):
        with RunningSystem() as system:
            legacy_bytes = system.database.read_bytes()
            self.assertFalse(legacy_bytes.startswith(MDB2_MAGIC))

            status, _, _ = system.request("GET", "/mdb-list")
            self.assertEqual(status, 200)
            status, _, _ = system.request("GET", "/mdb-lookup?key=RouteAlpha")
            self.assertEqual(status, 200)
            self.assertEqual(system.database.read_bytes(), legacy_bytes)

            status, headers, _ = system.post_form(
                "/mdb-add",
                {"name": "MigratedRecord", "msg": "Persisted"},
            )
            self.assertEqual(status, 302)
            self.assertEqual(headers.get("Location"), "/mdb-list")

            migrated_bytes = system.database.read_bytes()
            self.assertTrue(migrated_bytes.startswith(MDB2_MAGIC))
            next_id, records = parse_mdb2(migrated_bytes)
            self.assertEqual(len(records), system.record_count + 1)
            self.assertIn("MigratedRecord", {name for _, name, _ in records})
            self.assertGreater(next_id, max(record_id for record_id, _, _ in records))

    def test_ids_remain_stable_after_add_delete_and_restart(self):
        with RunningSystem() as system:
            initial = system.list_records()
            route_id = initial["RouteAlpha"][0]
            second_id = initial["SecondRecord"][0]

            status, _, _ = system.post_form(
                "/mdb-add",
                {"name": "StableAdded", "msg": "First"},
            )
            self.assertEqual(status, 302)
            after_add = system.list_records()
            added_id = after_add["StableAdded"][0]
            self.assertEqual(after_add["RouteAlpha"][0], route_id)
            self.assertEqual(after_add["SecondRecord"][0], second_id)
            self.assertGreater(added_id, max(route_id, second_id))

            status, _, _ = system.post_form(
                "/mdb-delete",
                {"id": str(route_id)},
            )
            self.assertEqual(status, 302)
            after_delete = system.list_records()
            self.assertNotIn("RouteAlpha", after_delete)
            self.assertEqual(after_delete["SecondRecord"][0], second_id)
            self.assertEqual(after_delete["StableAdded"][0], added_id)

            expected = after_delete
            system.restart()
            self.assertEqual(system.list_records(), expected)

    def test_deleted_ids_are_not_reused(self):
        with RunningSystem() as system:
            status, _, _ = system.post_form(
                "/mdb-add",
                {"name": "FirstAdded", "msg": "Temporary"},
            )
            self.assertEqual(status, 302)
            deleted_id = system.list_records()["FirstAdded"][0]

            status, _, _ = system.post_form(
                "/mdb-delete",
                {"id": str(deleted_id)},
            )
            self.assertEqual(status, 302)

            status, _, _ = system.post_form(
                "/mdb-add",
                {"name": "Replacement", "msg": "Permanent"},
            )
            self.assertEqual(status, 302)
            replacement_id = system.list_records()["Replacement"][0]
            self.assertNotEqual(replacement_id, deleted_id)
            self.assertGreater(replacement_id, deleted_id)

            system.restart()
            self.assertEqual(
                system.list_records()["Replacement"][0],
                replacement_id,
            )

    @unittest.skipUnless(
        os.name == "posix" and hasattr(os, "chmod"),
        "requires POSIX directory permissions",
    )
    def test_persistence_failure_changes_neither_memory_nor_disk(self):
        with RunningSystem() as system:
            before_records = system.list_records()
            before_bytes = system.database.read_bytes()
            original_mode = stat.S_IMODE(system.database_dir.stat().st_mode)
            probe = system.database_dir / "permission-probe"

            os.chmod(system.database_dir, 0o555)
            try:
                try:
                    descriptor = os.open(
                        probe,
                        os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                        0o600,
                    )
                except PermissionError:
                    descriptor = None
                else:
                    os.close(descriptor)
                    probe.unlink()
                    self.skipTest(
                        "this environment can write through read-only directory modes"
                    )

                status, _, _ = system.post_form(
                    "/mdb-add",
                    {"name": "MustRollback", "msg": "NoDiskWrite"},
                )
                self.assertEqual(status, 500)
                self.assertEqual(system.list_records(), before_records)
                self.assertEqual(system.database.read_bytes(), before_bytes)
            finally:
                os.chmod(system.database_dir, original_mode)

            self.assertFalse(probe.exists())
            self.assertEqual(
                {path.name for path in system.database_dir.iterdir()},
                {system.database.name},
            )

    def test_malformed_and_truncated_mdb2_files_are_rejected(self):
        with RunningSystem() as system:
            status, _, _ = system.post_form(
                "/mdb-add",
                {"name": "MakeMDB2", "msg": "Versioned"},
            )
            self.assertEqual(status, 302)
            valid = system.database.read_bytes()
            self.assertTrue(valid.startswith(MDB2_MAGIC))
            _, records = parse_mdb2(valid)

            system.stop_servers()
            cases = {
                "truncated header": valid[: MDB2_HEADER.size - 1],
                "truncated record": valid[:-1],
                "unsupported version": (
                    valid[:8] + struct.pack("<I", MDB2_VERSION + 1) + valid[12:]
                ),
                "incorrect record count": (
                    valid[:20]
                    + struct.pack("<Q", len(records) + 1)
                    + valid[28:]
                ),
            }
            for label, malformed in cases.items():
                with self.subTest(label=label):
                    candidate = system.root / f"{label.replace(' ', '-')}.mdb"
                    candidate.write_bytes(malformed)
                    assert_database_start_rejected(self, candidate)


@unittest.skipUnless(hasattr(os, "symlink"), "requires symbolic-link support")
class StaticPathContainmentTests(unittest.TestCase):
    def test_final_and_intermediate_symlinks_cannot_escape_web_root(self):
        with RunningSystem() as system:
            outside = system.root / "outside"
            outside.mkdir()
            final_secret = b"FINAL-SYMLINK-SECRET-6f768c\n"
            intermediate_secret = b"INTERMEDIATE-SYMLINK-SECRET-c15bd8\n"
            final_target = outside / "final-secret.txt"
            intermediate_target = outside / "nested-secret.txt"
            final_target.write_bytes(final_secret)
            intermediate_target.write_bytes(intermediate_secret)

            try:
                (system.web_root / "final-link.txt").symlink_to(final_target)
                (system.web_root / "outside-link").symlink_to(
                    outside,
                    target_is_directory=True,
                )
            except OSError as error:
                self.skipTest(f"cannot create test symlinks: {error}")

            requests = [
                ("/final-link.txt", final_secret),
                ("/outside-link/nested-secret.txt", intermediate_secret),
            ]
            for target, secret in requests:
                with self.subTest(target=target):
                    status, _, body = system.request("GET", target)
                    self.assertEqual(status, 403)
                    self.assertNotIn(secret, body)

            status, _, index = system.request("GET", "/index.html")
            self.assertEqual(status, 200)
            self.assertEqual(index, system.index_body)


class DisconnectTests(unittest.TestCase):
    def test_static_and_dynamic_resets_do_not_kill_or_desynchronize_servers(self):
        with RunningSystem(record_count=2048) as system:
            for _ in range(5):
                system.reset_request("/large.bin")
            for _ in range(3):
                system.reset_request("/mdb-list")

            status, _, index = system.request("GET", "/index.html")
            self.assertEqual(status, 200)
            self.assertEqual(index, system.index_body)
            self.assertIsNone(system.http_process.poll())
            self.assertIsNone(system.db_process.poll())

            status, _, search = system.request(
                "GET", "/mdb-lookup?key=RouteAlpha"
            )
            self.assertEqual(status, 200)
            self.assertIn(b"RouteAlpha", search)

    def test_bundled_client_downloads_binary_response_exactly(self):
        with RunningSystem() as system:
            completed = subprocess.run(
                [
                    str(HTTP_CLIENT),
                    "127.0.0.1",
                    str(system.http_port),
                    "/large.bin",
                ],
                cwd=system.root,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=10,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(
                (system.root / "large.bin").read_bytes(),
                (system.web_root / "large.bin").read_bytes(),
            )


class HttpClientAtomicDownloadTests(unittest.TestCase):
    def test_existing_destination_is_never_clobbered(self):
        response_body = b"replacement-content-must-not-win\n"
        response = (
            b"HTTP/1.0 200 OK\r\n"
            + f"Content-Length: {len(response_body)}\r\n".encode("ascii")
            + b"Content-Type: application/octet-stream\r\n"
            b"\r\n"
            + response_body
        )

        with tempfile.TemporaryDirectory(
            prefix="http-client-existing-test-"
        ) as directory:
            root = Path(directory)
            destination = root / "artifact.bin"
            sentinel = b"existing-destination-sentinel\n"
            destination.write_bytes(sentinel)

            with OneShotHttpServer(response) as server:
                completed = subprocess.run(
                    [
                        str(HTTP_CLIENT),
                        "127.0.0.1",
                        str(server.port),
                        "/artifact.bin",
                    ],
                    cwd=root,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=10,
                    check=False,
                )

            self.assertNotEqual(completed.returncode, 0)
            self.assertEqual(destination.read_bytes(), sentinel)
            self.assertEqual(list(root.glob(".artifact.bin.part.*")), [])

    def test_destination_created_during_download_is_never_clobbered(self):
        response_body = b"download-completes-after-destination-appears\n"
        response = (
            b"HTTP/1.0 200 OK\r\n"
            + f"Content-Length: {len(response_body)}\r\n".encode("ascii")
            + b"Content-Type: application/octet-stream\r\n"
            b"\r\n"
            + response_body
        )
        response_gate = threading.Event()

        with tempfile.TemporaryDirectory(
            prefix="http-client-publish-race-test-"
        ) as directory:
            root = Path(directory)
            destination = root / "raced.bin"
            sentinel = b"destination-created-by-another-writer\n"

            with OneShotHttpServer(
                response,
                response_gate=response_gate,
            ) as server:
                process = subprocess.Popen(
                    [
                        str(HTTP_CLIENT),
                        "127.0.0.1",
                        str(server.port),
                        "/raced.bin",
                    ],
                    cwd=root,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                try:
                    self.assertTrue(
                        server.request_received.wait(timeout=3),
                        "client did not send its request",
                    )
                    destination.write_bytes(sentinel)
                    response_gate.set()
                    _, stderr = process.communicate(timeout=10)
                finally:
                    response_gate.set()
                    if process.poll() is None:
                        process.kill()
                        process.communicate(timeout=3)

            self.assertNotEqual(process.returncode, 0, stderr)
            self.assertEqual(destination.read_bytes(), sentinel)
            self.assertEqual(list(root.glob(".raced.bin.part.*")), [])

    def test_truncated_response_leaves_no_output_or_temporary_file(self):
        partial_body = b"only-a-small-prefix"
        response = (
            b"HTTP/1.0 200 OK\r\n"
            b"Content-Length: 4096\r\n"
            b"Content-Type: application/octet-stream\r\n"
            b"\r\n"
            + partial_body
        )

        with tempfile.TemporaryDirectory(
            prefix="http-client-truncated-test-"
        ) as directory:
            root = Path(directory)
            destination = root / "truncated.bin"

            with OneShotHttpServer(response) as server:
                completed = subprocess.run(
                    [
                        str(HTTP_CLIENT),
                        "127.0.0.1",
                        str(server.port),
                        "/truncated.bin",
                    ],
                    cwd=root,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=10,
                    check=False,
                )

            self.assertNotEqual(completed.returncode, 0)
            self.assertTrue(server.request_received.is_set())
            self.assertFalse(destination.exists())
            self.assertEqual(list(root.glob(".truncated.bin.part.*")), [])

    def test_incomplete_or_malformed_headers_leave_no_artifacts(self):
        cases = [
            (
                "eof before header terminator",
                b"HTTP/1.0 200 OK\r\n"
                b"Content-Type: application/octet-stream\r\n",
            ),
            (
                "malformed content length",
                b"HTTP/1.0 200 OK\r\n"
                b"Content-Length: not-a-number\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"\r\n",
            ),
        ]

        for label, response in cases:
            with self.subTest(label=label):
                with tempfile.TemporaryDirectory(
                    prefix="http-client-header-failure-test-"
                ) as directory:
                    root = Path(directory)
                    destination = root / "headers.bin"

                    with OneShotHttpServer(response) as server:
                        completed = subprocess.run(
                            [
                                str(HTTP_CLIENT),
                                "127.0.0.1",
                                str(server.port),
                                "/headers.bin",
                            ],
                            cwd=root,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            timeout=10,
                            check=False,
                        )

                    self.assertNotEqual(completed.returncode, 0)
                    self.assertTrue(server.request_received.is_set())
                    self.assertFalse(destination.exists())
                    self.assertEqual(
                        list(root.glob(".headers.bin.part.*")),
                        [],
                    )


class DirectBackendTests(unittest.TestCase):
    def test_structured_read_protocol_preserves_legacy_field_boundaries(self):
        legacy_records = [
            ("Brace}Name", "Value} <>&\"'"),
            ("", "MessageOnly"),
            ("NameOnly", ""),
            ("", ""),
        ]

        with RunningSystem(record_count=2) as system:
            system.stop_servers()
            write_legacy_database(system.database, legacy_records)
            system.start_database()

            with socket.create_connection(
                ("127.0.0.1", system.db_port), timeout=3
            ) as sock:
                with sock.makefile("rwb", buffering=0) as stream:
                    def read_framed_response():
                        response = bytearray()
                        while True:
                            line = stream.readline()
                            self.assertNotEqual(
                                line,
                                b"",
                                "backend closed mid-response",
                            )
                            response.extend(line)
                            if line == b"\n":
                                return bytes(response)

                    stream.write(b"LIST2\n")
                    self.assertEqual(
                        read_framed_response(),
                        (
                            b"1\tBrace}Name\tValue} <>&\"'\n"
                            b"2\t\tMessageOnly\n"
                            b"3\tNameOnly\t\n"
                            b"4\t\t\n"
                            b"\n"
                        ),
                    )

                    stream.write(b"SEARCH2 }\n")
                    self.assertEqual(
                        read_framed_response(),
                        b"1\tBrace}Name\tValue} <>&\"'\n\n",
                    )

                    stream.write(b"SEARCH2 MessageOnly\n")
                    self.assertEqual(
                        read_framed_response(),
                        b"2\t\tMessageOnly\n\n",
                    )

                    stream.write(b"SEARCH2 NameOnly\n")
                    self.assertEqual(
                        read_framed_response(),
                        b"3\tNameOnly\t\n\n",
                    )

                    stream.write(b"SEARCH2 NeverPresent987\n")
                    self.assertEqual(read_framed_response(), b"\n")

                    stream.write(b"ADD Bad\tName|Message\n")
                    self.assertEqual(
                        stream.readline(),
                        b"ERROR: Invalid name or message\n",
                    )

                    stream.write(b"LIST\n")
                    self.assertEqual(
                        read_framed_response(),
                        (
                            b"   1. {Brace}Name},said {Value} <>&\"'}\n"
                            b"   2. {},said {MessageOnly}\n"
                            b"   3. {NameOnly},said {}\n"
                            b"   4. {},said {}\n"
                            b"\n"
                        ),
                    )

                    stream.write(b"SEARCH NameOnly\n")
                    self.assertEqual(
                        read_framed_response(),
                        b"   3. {NameOnly},said {}\n\n",
                    )

    def test_backend_rejects_malformed_commands_and_survives_reset(self):
        with RunningSystem(record_count=1024) as system:
            # The database server handles one persistent connection at a time.
            stop_process(system.http_process)

            with socket.create_connection(
                ("127.0.0.1", system.db_port), timeout=3
            ) as sock:
                with sock.makefile("rwb", buffering=0) as stream:
                    invalid_commands = [
                        b"ADD |message\n",
                        b"ADD name|\n",
                        b"ADD name|message|extra\n",
                        b"UPDATE 1abc|name|message\n",
                        b"UPDATE 1|name|message|extra\n",
                        b"DELETE 1abc\n",
                        b"DELETE -1\n",
                        b"ADD bad\0name|message\n",
                        b"A" * 1200 + b"\n",
                    ]
                    for command in invalid_commands:
                        with self.subTest(command=command[:40]):
                            stream.write(command)
                            response = stream.readline()
                            self.assertTrue(response.startswith(b"ERROR"), response)

                    stream.write(b"ADD FifteenCharName|TwentyThreeCharacterMsg\n")
                    response = stream.readline()
                    self.assertRegex(response, rb"^OK [1-9][0-9]*\n$")

            reset = socket.create_connection(
                ("127.0.0.1", system.db_port), timeout=3
            )
            reset.sendall(b"LIST\n")
            reset.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_LINGER,
                struct.pack("ii", 1, 0),
            )
            reset.close()

            deadline = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(
                        ("127.0.0.1", system.db_port), timeout=1
                    ) as probe:
                        probe.sendall(b"SEARCH RouteAlpha\n")
                        probe.settimeout(2)
                        response = b""
                        while not response.endswith(b"\n\n"):
                            chunk = probe.recv(4096)
                            if not chunk:
                                break
                            response += chunk
                        self.assertIn(b"RouteAlpha", response)
                        break
                except (ConnectionError, TimeoutError, socket.timeout):
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.05)

            self.assertIsNone(system.db_process.poll())


if __name__ == "__main__":
    unittest.main(verbosity=2)
