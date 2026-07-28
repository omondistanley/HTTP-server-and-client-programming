#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "mdb.h"
#include "mylist.h"

#define KEY_MAX 1000
#define MAX_LINE_LEN 4096
#define MAX_RESPONSE_LEN 1200
#define MAX_NAME_LEN 15
#define MAX_MSG_LEN 23

#define LEGACY_RECORD_SIZE 40U
#define MDB2_HEADER_SIZE 28U
#define MDB2_RECORD_SIZE 48U
#define MDB2_VERSION 1U

static const unsigned char MDB2_MAGIC[8] = {
    'M', 'D', 'B', '2', '\r', '\n', 0x1a, '\n'
};

struct Database {
    struct List records;
    uint64_t next_id;
};

enum MutationResult {
    MUTATION_OK = 0,
    MUTATION_NOT_FOUND,
    MUTATION_NO_MEMORY,
    MUTATION_PERSISTENCE_FAILED,
    MUTATION_ID_EXHAUSTED
};

static void die(const char *message)
{
    perror(message);
    exit(1);
}

static ssize_t write_all(int socket_fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = (const unsigned char *)buffer;
    size_t total = 0;

    while (total < length) {
        ssize_t written = send(socket_fd, cursor + total, length - total, 0);
        if (written > 0) {
            total += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written == 0)
            errno = EPIPE;
        return -1;
    }

    return (ssize_t)total;
}

static int send_text(int socket_fd, const char *text)
{
    return write_all(socket_fd, text, strlen(text)) < 0 ? -1 : 0;
}

static int parse_positive_u64(const char *text, uint64_t *result)
{
    char *end = NULL;
    uintmax_t value;
    const unsigned char *cursor;

    if (!text || !*text || !result)
        return -1;

    for (cursor = (const unsigned char *)text; *cursor; cursor++) {
        if (!isdigit(*cursor))
            return -1;
    }

    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' ||
        value == 0 || value > UINT64_MAX) {
        return -1;
    }

    *result = (uint64_t)value;
    return 0;
}

static int parse_port(const char *text, unsigned short *result)
{
    uint64_t value;

    if (parse_positive_u64(text, &value) < 0 || value > 65535)
        return -1;

    *result = (unsigned short)value;
    return 0;
}

static int validate_text_value(
    const char *value,
    size_t max_length,
    int forbid_pipe)
{
    size_t length;
    size_t i;
    int has_non_space = 0;

    if (!value)
        return -1;

    length = strlen(value);
    if (length == 0 || length > max_length)
        return -1;

    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 32 || c == 127 || (forbid_pipe && c == '|'))
            return -1;
        if (!isspace(c))
            has_non_space = 1;
    }

    return has_non_space ? 0 : -1;
}

/*
 * Read one newline-framed backend command while retaining its true byte
 * length. Embedded NUL bytes and overlong lines are drained and rejected.
 */
static int read_command_line(
    int socket_fd,
    char *line,
    size_t capacity,
    size_t *line_length)
{
    size_t used = 0;
    int invalid = 0;

    while (1) {
        unsigned char c;
        ssize_t received = recv(socket_fd, &c, 1, 0);

        if (received == 0) {
            if (used == 0 && !invalid)
                return 0;
            return -1;
        }
        if (received < 0) {
            if (errno == EINTR)
                continue;
            return -2;
        }

        if (c == '\n')
            break;
        if (c == '\0') {
            invalid = 1;
            continue;
        }
        if (used + 1 >= capacity) {
            invalid = 1;
            continue;
        }
        line[used++] = (char)c;
    }

    if (invalid)
        return -1;
    if (used > 0 && line[used - 1] == '\r')
        used--;

    line[used] = '\0';
    *line_length = used;
    return 1;
}

static void database_init(struct Database *database)
{
    initList(&database->records);
    database->next_id = 1;
}

void freemdb(struct List *list)
{
    traverseList(list, &free);
    removeAllNodes(list);
}

static void database_free(struct Database *database)
{
    freemdb(&database->records);
    database->next_id = 1;
}

static struct Node *append_owned_record(
    struct List *list,
    struct Node **tail,
    struct MdbRec *record)
{
    struct Node *node = addAfter(list, *tail, record);
    if (node)
        *tail = node;
    return node;
}

static struct MdbRec *find_record(struct List *list, uint64_t id)
{
    struct Node *node;

    for (node = list->head; node; node = node->next) {
        struct MdbRec *record = (struct MdbRec *)node->data;
        if (record && record->id == id)
            return record;
    }

    return NULL;
}

static int validate_stored_field(const char *field, size_t capacity)
{
    const char *terminator = (const char *)memchr(field, '\0', capacity);
    size_t length;
    size_t i;

    if (!terminator)
        return -1;

    length = (size_t)(terminator - field);
    for (i = length + 1; i < capacity; i++) {
        if (field[i] != '\0')
            return -1;
    }

    /*
     * Legacy databases can contain empty values and delimiter characters.
     * Preserve those records during migration, while still rejecting bytes
     * that would corrupt the line-framed backend response protocol.
     */
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)field[i];
        if (c < 32 || c == 127)
            return -1;
    }

    return 0;
}

static int database_record_count(
    const struct Database *database,
    uint64_t *count_out)
{
    const struct Node *node;
    uint64_t count = 0;

    if (!database || database->next_id == 0)
        return -1;

    for (node = database->records.head; node; node = node->next) {
        const struct Node *previous;
        const struct MdbRec *record = (const struct MdbRec *)node->data;

        if (!record || record->id == 0 || record->id >= database->next_id ||
            validate_stored_field(record->name, sizeof(record->name)) < 0 ||
            validate_stored_field(record->msg, sizeof(record->msg)) < 0) {
            return -1;
        }

        for (previous = database->records.head;
             previous != node;
             previous = previous->next) {
            const struct MdbRec *other =
                (const struct MdbRec *)previous->data;
            if (!other || other->id == record->id)
                return -1;
        }

        if (count == UINT64_MAX)
            return -1;
        count++;
    }

    *count_out = count;
    return 0;
}

static uint32_t decode_le32(const unsigned char bytes[4])
{
    return ((uint32_t)bytes[0]) |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint64_t decode_le64(const unsigned char bytes[8])
{
    return ((uint64_t)bytes[0]) |
           ((uint64_t)bytes[1] << 8) |
           ((uint64_t)bytes[2] << 16) |
           ((uint64_t)bytes[3] << 24) |
           ((uint64_t)bytes[4] << 32) |
           ((uint64_t)bytes[5] << 40) |
           ((uint64_t)bytes[6] << 48) |
           ((uint64_t)bytes[7] << 56);
}

static void encode_le32(unsigned char bytes[4], uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24) & 0xffU);
}

static void encode_le64(unsigned char bytes[8], uint64_t value)
{
    bytes[0] = (unsigned char)(value & UINT64_C(0xff));
    bytes[1] = (unsigned char)((value >> 8) & UINT64_C(0xff));
    bytes[2] = (unsigned char)((value >> 16) & UINT64_C(0xff));
    bytes[3] = (unsigned char)((value >> 24) & UINT64_C(0xff));
    bytes[4] = (unsigned char)((value >> 32) & UINT64_C(0xff));
    bytes[5] = (unsigned char)((value >> 40) & UINT64_C(0xff));
    bytes[6] = (unsigned char)((value >> 48) & UINT64_C(0xff));
    bytes[7] = (unsigned char)((value >> 56) & UINT64_C(0xff));
}

static int read_exact(FILE *stream, void *buffer, size_t length)
{
    return fread(buffer, 1, length, stream) == length ? 0 : -1;
}

static int write_exact(FILE *stream, const void *buffer, size_t length)
{
    return fwrite(buffer, 1, length, stream) == length ? 0 : -1;
}

static int copy_legacy_field(
    char *destination,
    size_t destination_size,
    const unsigned char *source,
    size_t source_size)
{
    const unsigned char *terminator =
        (const unsigned char *)memchr(source, '\0', source_size);
    size_t length;

    if (!terminator) {
        errno = EINVAL;
        return -1;
    }

    length = (size_t)(terminator - source);
    if (length >= destination_size) {
        errno = EINVAL;
        return -1;
    }

    memset(destination, 0, destination_size);
    memcpy(destination, source, length);
    if (validate_stored_field(destination, destination_size) < 0) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

static int get_file_size(FILE *stream, uint64_t *size_out)
{
    struct stat status;

    if (fstat(fileno(stream), &status) < 0)
        return -1;
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        errno = EINVAL;
        return -1;
    }

    *size_out = (uint64_t)status.st_size;
    return 0;
}

static int load_legacy_database(
    FILE *stream,
    uint64_t file_size,
    struct Database *database)
{
    struct Node *tail = NULL;
    uint64_t count;
    uint64_t index;

    if (file_size % LEGACY_RECORD_SIZE != 0) {
        errno = EINVAL;
        return -1;
    }

    count = file_size / LEGACY_RECORD_SIZE;
    if (count == UINT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    if (fseek(stream, 0, SEEK_SET) != 0)
        return -1;

    for (index = 0; index < count; index++) {
        unsigned char bytes[LEGACY_RECORD_SIZE];
        struct MdbRec *record;

        if (read_exact(stream, bytes, sizeof(bytes)) < 0) {
            errno = EINVAL;
            return -1;
        }

        record = (struct MdbRec *)calloc(1, sizeof(*record));
        if (!record)
            return -1;
        record->id = index + 1;

        if (copy_legacy_field(
                record->name, sizeof(record->name),
                bytes, sizeof(record->name)) < 0 ||
            copy_legacy_field(
                record->msg, sizeof(record->msg),
                bytes + sizeof(record->name), sizeof(record->msg)) < 0 ||
            !append_owned_record(&database->records, &tail, record)) {
            int saved_errno = errno ? errno : ENOMEM;
            free(record);
            errno = saved_errno;
            return -1;
        }
    }

    database->next_id = count + 1;
    return 0;
}

static int load_mdb2_database(
    FILE *stream,
    uint64_t file_size,
    struct Database *database)
{
    unsigned char magic[sizeof(MDB2_MAGIC)];
    unsigned char version_bytes[4];
    unsigned char next_id_bytes[8];
    unsigned char count_bytes[8];
    struct Node *tail = NULL;
    uint32_t version;
    uint64_t next_id;
    uint64_t count;
    uint64_t expected_size;
    uint64_t index;

    if (file_size < MDB2_HEADER_SIZE ||
        fseek(stream, 0, SEEK_SET) != 0 ||
        read_exact(stream, magic, sizeof(magic)) < 0 ||
        read_exact(stream, version_bytes, sizeof(version_bytes)) < 0 ||
        read_exact(stream, next_id_bytes, sizeof(next_id_bytes)) < 0 ||
        read_exact(stream, count_bytes, sizeof(count_bytes)) < 0) {
        errno = EINVAL;
        return -1;
    }

    version = decode_le32(version_bytes);
    next_id = decode_le64(next_id_bytes);
    count = decode_le64(count_bytes);

    if (memcmp(magic, MDB2_MAGIC, sizeof(MDB2_MAGIC)) != 0 ||
        version != MDB2_VERSION ||
        next_id == 0 ||
        count > (UINT64_MAX - MDB2_HEADER_SIZE) / MDB2_RECORD_SIZE) {
        errno = EINVAL;
        return -1;
    }

    expected_size = MDB2_HEADER_SIZE + count * MDB2_RECORD_SIZE;
    if (file_size != expected_size) {
        errno = EINVAL;
        return -1;
    }

    for (index = 0; index < count; index++) {
        unsigned char bytes[MDB2_RECORD_SIZE];
        struct MdbRec *record;

        if (read_exact(stream, bytes, sizeof(bytes)) < 0) {
            errno = EINVAL;
            return -1;
        }

        record = (struct MdbRec *)calloc(1, sizeof(*record));
        if (!record)
            return -1;

        record->id = decode_le64(bytes);
        memcpy(record->name, bytes + 8, sizeof(record->name));
        memcpy(
            record->msg,
            bytes + 8 + sizeof(record->name),
            sizeof(record->msg));

        if (record->id == 0 || record->id >= next_id ||
            find_record(&database->records, record->id) ||
            validate_stored_field(record->name, sizeof(record->name)) < 0 ||
            validate_stored_field(record->msg, sizeof(record->msg)) < 0 ||
            !append_owned_record(&database->records, &tail, record)) {
            int saved_errno = errno ? errno : EINVAL;
            free(record);
            errno = saved_errno;
            return -1;
        }
    }

    database->next_id = next_id;
    return 0;
}

static int load_database(
    FILE *stream,
    struct Database *database,
    int *was_legacy)
{
    unsigned char magic[sizeof(MDB2_MAGIC)];
    uint64_t file_size;
    int result;

    database_init(database);
    if (get_file_size(stream, &file_size) < 0)
        return -1;

    *was_legacy = 1;
    if (file_size >= sizeof(magic)) {
        if (fseek(stream, 0, SEEK_SET) != 0 ||
            read_exact(stream, magic, sizeof(magic)) < 0) {
            return -1;
        }
        if (memcmp(magic, MDB2_MAGIC, sizeof(magic)) == 0) {
            *was_legacy = 0;
            result = load_mdb2_database(stream, file_size, database);
            if (result < 0)
                database_free(database);
            return result;
        }
    }

    result = load_legacy_database(stream, file_size, database);
    if (result < 0)
        database_free(database);
    return result;
}

static int write_mdb2_stream(
    FILE *stream,
    const struct Database *database)
{
    unsigned char version_bytes[4];
    unsigned char next_id_bytes[8];
    unsigned char count_bytes[8];
    uint64_t count;
    const struct Node *node;

    if (database_record_count(database, &count) < 0) {
        errno = EINVAL;
        return -1;
    }

    encode_le32(version_bytes, MDB2_VERSION);
    encode_le64(next_id_bytes, database->next_id);
    encode_le64(count_bytes, count);

    if (write_exact(stream, MDB2_MAGIC, sizeof(MDB2_MAGIC)) < 0 ||
        write_exact(stream, version_bytes, sizeof(version_bytes)) < 0 ||
        write_exact(stream, next_id_bytes, sizeof(next_id_bytes)) < 0 ||
        write_exact(stream, count_bytes, sizeof(count_bytes)) < 0) {
        return -1;
    }

    for (node = database->records.head; node; node = node->next) {
        const struct MdbRec *record = (const struct MdbRec *)node->data;
        unsigned char id_bytes[8];

        encode_le64(id_bytes, record->id);
        if (write_exact(stream, id_bytes, sizeof(id_bytes)) < 0 ||
            write_exact(stream, record->name, sizeof(record->name)) < 0 ||
            write_exact(stream, record->msg, sizeof(record->msg)) < 0) {
            return -1;
        }
    }

    return 0;
}

static int persist_database(
    const char *filename,
    const struct Database *database)
{
    static const char suffix[] = ".tmp.XXXXXX";
    struct stat original_status;
    char *temporary_name = NULL;
    size_t filename_length;
    FILE *stream = NULL;
    int file_descriptor = -1;
    int saved_errno;

    if (stat(filename, &original_status) < 0)
        return -1;
    if (!S_ISREG(original_status.st_mode)) {
        errno = EINVAL;
        return -1;
    }

    filename_length = strlen(filename);
    if (filename_length > SIZE_MAX - sizeof(suffix)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    temporary_name =
        (char *)malloc(filename_length + sizeof(suffix));
    if (!temporary_name)
        return -1;

    memcpy(temporary_name, filename, filename_length);
    memcpy(
        temporary_name + filename_length,
        suffix,
        sizeof(suffix));

    file_descriptor = mkstemp(temporary_name);
    if (file_descriptor < 0)
        goto fail;

    if (fchmod(file_descriptor, original_status.st_mode & 07777) < 0)
        goto fail;

    stream = fdopen(file_descriptor, "wb");
    if (!stream)
        goto fail;
    file_descriptor = -1;

    if (write_mdb2_stream(stream, database) < 0 ||
        fflush(stream) != 0 ||
        fsync(fileno(stream)) != 0) {
        goto fail;
    }

    {
        FILE *closing_stream = stream;
        stream = NULL;
        if (fclose(closing_stream) != 0)
            goto fail;
    }

    if (rename(temporary_name, filename) != 0)
        goto fail;

    free(temporary_name);
    return 0;

fail:
    saved_errno = errno ? errno : EIO;
    if (stream)
        fclose(stream);
    else if (file_descriptor >= 0)
        close(file_descriptor);
    if (temporary_name)
        unlink(temporary_name);
    free(temporary_name);
    errno = saved_errno;
    return -1;
}

static int clone_database(
    const struct Database *source,
    struct Database *destination)
{
    const struct Node *node;
    struct Node *tail = NULL;

    database_init(destination);
    destination->next_id = source->next_id;

    for (node = source->records.head; node; node = node->next) {
        const struct MdbRec *source_record =
            (const struct MdbRec *)node->data;
        struct MdbRec *record =
            (struct MdbRec *)malloc(sizeof(*record));

        if (!record) {
            database_free(destination);
            return -1;
        }
        memcpy(record, source_record, sizeof(*record));

        if (!append_owned_record(&destination->records, &tail, record)) {
            free(record);
            database_free(destination);
            return -1;
        }
    }

    return 0;
}

static void publish_candidate(
    struct Database *live,
    struct Database *candidate)
{
    struct Database old = *live;

    *live = *candidate;
    database_init(candidate);
    database_free(&old);
}

static int add_record(
    struct Database *database,
    const char *name,
    const char *message,
    uint64_t *assigned_id)
{
    struct MdbRec *record;
    uint64_t id;

    if (database->next_id == UINT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    id = database->next_id;
    record = (struct MdbRec *)calloc(1, sizeof(*record));
    if (!record)
        return -1;

    record->id = id;
    memcpy(record->name, name, strlen(name));
    memcpy(record->msg, message, strlen(message));

    if (!addAfter(&database->records, NULL, record)) {
        free(record);
        return -1;
    }

    database->next_id++;
    *assigned_id = id;
    return 0;
}

static int update_record(
    struct Database *database,
    uint64_t id,
    const char *name,
    const char *message)
{
    struct MdbRec *record = find_record(&database->records, id);

    if (!record)
        return -1;

    memset(record->name, 0, sizeof(record->name));
    memset(record->msg, 0, sizeof(record->msg));
    memcpy(record->name, name, strlen(name));
    memcpy(record->msg, message, strlen(message));
    return 0;
}

static int delete_record(struct Database *database, uint64_t id)
{
    struct Node *node = database->records.head;
    struct Node *previous = NULL;

    while (node) {
        struct MdbRec *record = (struct MdbRec *)node->data;
        if (record && record->id == id) {
            if (previous)
                previous->next = node->next;
            else
                database->records.head = node->next;
            free(record);
            free(node);
            return 0;
        }
        previous = node;
        node = node->next;
    }

    return -1;
}

static enum MutationResult atomic_add(
    struct Database *live,
    const char *filename,
    const char *name,
    const char *message,
    uint64_t *assigned_id)
{
    struct Database candidate;
    uint64_t id;

    if (live->next_id == UINT64_MAX)
        return MUTATION_ID_EXHAUSTED;
    if (clone_database(live, &candidate) < 0)
        return MUTATION_NO_MEMORY;
    if (add_record(&candidate, name, message, &id) < 0) {
        database_free(&candidate);
        return errno == EOVERFLOW
            ? MUTATION_ID_EXHAUSTED
            : MUTATION_NO_MEMORY;
    }
    if (persist_database(filename, &candidate) < 0) {
        database_free(&candidate);
        return MUTATION_PERSISTENCE_FAILED;
    }

    publish_candidate(live, &candidate);
    *assigned_id = id;
    return MUTATION_OK;
}

static enum MutationResult atomic_update(
    struct Database *live,
    const char *filename,
    uint64_t id,
    const char *name,
    const char *message)
{
    struct Database candidate;

    if (!find_record(&live->records, id))
        return MUTATION_NOT_FOUND;
    if (clone_database(live, &candidate) < 0)
        return MUTATION_NO_MEMORY;
    if (update_record(&candidate, id, name, message) < 0) {
        database_free(&candidate);
        return MUTATION_NOT_FOUND;
    }
    if (persist_database(filename, &candidate) < 0) {
        database_free(&candidate);
        return MUTATION_PERSISTENCE_FAILED;
    }

    publish_candidate(live, &candidate);
    return MUTATION_OK;
}

static enum MutationResult atomic_delete(
    struct Database *live,
    const char *filename,
    uint64_t id)
{
    struct Database candidate;

    if (!find_record(&live->records, id))
        return MUTATION_NOT_FOUND;
    if (clone_database(live, &candidate) < 0)
        return MUTATION_NO_MEMORY;
    if (delete_record(&candidate, id) < 0) {
        database_free(&candidate);
        return MUTATION_NOT_FOUND;
    }
    if (persist_database(filename, &candidate) < 0) {
        database_free(&candidate);
        return MUTATION_PERSISTENCE_FAILED;
    }

    publish_candidate(live, &candidate);
    return MUTATION_OK;
}

static char *my_strcasestr(const char *haystack, const char *needle)
{
    if (!haystack || !needle)
        return NULL;
    if (*needle == '\0')
        return (char *)haystack;

    for (; *haystack; haystack++) {
        const char *haystack_cursor = haystack;
        const char *needle_cursor = needle;

        while (*haystack_cursor && *needle_cursor &&
               tolower((unsigned char)*haystack_cursor) ==
                   tolower((unsigned char)*needle_cursor)) {
            haystack_cursor++;
            needle_cursor++;
        }
        if (!*needle_cursor)
            return (char *)haystack;
    }

    return NULL;
}

static int send_record(int client_socket, const struct MdbRec *record)
{
    char response[MAX_RESPONSE_LEN];
    int length = snprintf(
        response,
        sizeof(response),
        "%4" PRIu64 ". {%s},said {%s}\n",
        record->id,
        record->name,
        record->msg);

    if (length < 0 || (size_t)length >= sizeof(response)) {
        errno = EOVERFLOW;
        return -1;
    }

    return write_all(client_socket, response, (size_t)length) < 0 ? -1 : 0;
}

static int send_record_v2(int client_socket, const struct MdbRec *record)
{
    char response[MAX_RESPONSE_LEN];
    int length = snprintf(
        response,
        sizeof(response),
        "%" PRIu64 "\t%s\t%s\n",
        record->id,
        record->name,
        record->msg);

    if (length < 0 || (size_t)length >= sizeof(response)) {
        errno = EOVERFLOW;
        return -1;
    }

    return write_all(client_socket, response, (size_t)length) < 0 ? -1 : 0;
}

static int list_all_records(
    const struct Database *database,
    int client_socket)
{
    const struct Node *node;

    for (node = database->records.head; node; node = node->next) {
        const struct MdbRec *record = (const struct MdbRec *)node->data;
        if (send_record(client_socket, record) < 0)
            return -1;
    }

    return write_all(client_socket, "\n", 1) < 0 ? -1 : 0;
}

static int list_all_records_v2(
    const struct Database *database,
    int client_socket)
{
    const struct Node *node;

    for (node = database->records.head; node; node = node->next) {
        const struct MdbRec *record = (const struct MdbRec *)node->data;
        if (send_record_v2(client_socket, record) < 0)
            return -1;
    }

    return write_all(client_socket, "\n", 1) < 0 ? -1 : 0;
}

static int search_records(
    const struct Database *database,
    int client_socket,
    const char *key,
    int *match_count)
{
    const struct Node *node;
    int matches = 0;

    for (node = database->records.head; node; node = node->next) {
        const struct MdbRec *record = (const struct MdbRec *)node->data;
        if (my_strcasestr(record->name, key) ||
            my_strcasestr(record->msg, key)) {
            if (send_record(client_socket, record) < 0)
                return -1;
            matches++;
        }
    }

    if (write_all(client_socket, "\n", 1) < 0)
        return -1;

    *match_count = matches;
    return 0;
}

static int search_records_v2(
    const struct Database *database,
    int client_socket,
    const char *key,
    int *match_count)
{
    const struct Node *node;
    int matches = 0;

    for (node = database->records.head; node; node = node->next) {
        const struct MdbRec *record = (const struct MdbRec *)node->data;
        if (my_strcasestr(record->name, key) ||
            my_strcasestr(record->msg, key)) {
            if (send_record_v2(client_socket, record) < 0)
                return -1;
            matches++;
        }
    }

    if (write_all(client_socket, "\n", 1) < 0)
        return -1;

    *match_count = matches;
    return 0;
}

static int send_mutation_error(
    int client_socket,
    enum MutationResult result,
    const char *operation)
{
    if (result == MUTATION_NOT_FOUND)
        return send_text(client_socket, "ERROR: Record not found\n");
    if (result == MUTATION_ID_EXHAUSTED)
        return send_text(client_socket, "ERROR: Record ID space exhausted\n");
    if (result == MUTATION_NO_MEMORY)
        return send_text(client_socket, "ERROR: Out of memory\n");

    if (strcmp(operation, "add") == 0)
        return send_text(client_socket, "ERROR: Failed to persist added record\n");
    if (strcmp(operation, "update") == 0)
        return send_text(client_socket, "ERROR: Failed to persist updated record\n");
    return send_text(client_socket, "ERROR: Failed to persist deleted record\n");
}

int main(int argc, char **argv)
{
    const char *filename;
    unsigned short port;
    FILE *database_file;
    struct Database database;
    int was_legacy = 0;
    uint64_t loaded_count;
    int server_socket;
    int reuse = 1;
    struct sockaddr_in server_address;

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal");

    if (argc != 3) {
        fprintf(
            stderr,
            "Usage: %s <database_file> <server_port>\n",
            argv[0]);
        return 1;
    }

    filename = argv[1];
    if (!filename || !*filename || strlen(filename) >= 1024) {
        fprintf(stderr, "Error: Invalid database filename\n");
        return 1;
    }
    if (parse_port(argv[2], &port) < 0) {
        fprintf(stderr, "Error: Invalid port number (must be 1-65535)\n");
        return 1;
    }

    database_file = fopen(filename, "rb");
    if (!database_file)
        die(filename);
    if (load_database(database_file, &database, &was_legacy) < 0) {
        int saved_errno = errno;
        fclose(database_file);
        errno = saved_errno;
        die("load database");
    }
    if (fclose(database_file) != 0) {
        database_free(&database);
        die("close database");
    }
    if (database_record_count(&database, &loaded_count) < 0) {
        database_free(&database);
        errno = EINVAL;
        die("validate database");
    }

    fprintf(
        stderr,
        "Loaded %" PRIu64 " records from %s database; next ID is %" PRIu64 "\n",
        loaded_count,
        was_legacy ? "legacy" : "MDB2",
        database.next_id);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        database_free(&database);
        die("socket");
    }
    if (setsockopt(
            server_socket,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)) < 0) {
        database_free(&database);
        close(server_socket);
        die("setsockopt");
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(
            server_socket,
            (struct sockaddr *)&server_address,
            sizeof(server_address)) < 0) {
        database_free(&database);
        close(server_socket);
        die("bind");
    }
    if (listen(server_socket, 10) < 0) {
        database_free(&database);
        close(server_socket);
        die("listen");
    }

    while (1) {
        struct sockaddr_in client_address;
        socklen_t client_length = sizeof(client_address);
        int client_socket = accept(
            server_socket,
            (struct sockaddr *)&client_address,
            &client_length);
        char line[MAX_LINE_LEN];

        if (client_socket < 0) {
            if (errno == EINTR)
                continue;
            database_free(&database);
            close(server_socket);
            die("accept");
        }

        fprintf(
            stderr,
            "\nconnection started from: %s\n",
            inet_ntoa(client_address.sin_addr));

        while (1) {
            size_t length = 0;
            int read_result = read_command_line(
                client_socket,
                line,
                sizeof(line),
                &length);

            if (read_result == 0)
                break;
            if (read_result == -2) {
                fprintf(stderr, "Error reading from client connection\n");
                break;
            }
            if (read_result == -1) {
                if (send_text(
                        client_socket,
                        "ERROR: Invalid command line\n") < 0) {
                    break;
                }
                continue;
            }
            if (length == 0)
                continue;

            if (strncmp(line, "SEARCH2 ", 8) == 0) {
                char *key = line + 8;
                int match_count;

                if (validate_text_value(key, KEY_MAX, 0) < 0) {
                    if (send_text(
                            client_socket,
                            "ERROR: Invalid search key\n") < 0) {
                        break;
                    }
                    continue;
                }

                if (search_records_v2(
                        &database,
                        client_socket,
                        key,
                        &match_count) < 0) {
                    break;
                }
                fprintf(
                    stderr,
                    "SEARCH2 for '%s' completed: %d match(es) found\n",
                    key,
                    match_count);
            } else if (strncmp(line, "SEARCH ", 7) == 0) {
                char *key = line + 7;
                int match_count;

                if (validate_text_value(key, KEY_MAX, 0) < 0) {
                    if (send_text(
                            client_socket,
                            "ERROR: Invalid search key\n") < 0) {
                        break;
                    }
                    continue;
                }

                if (search_records(
                        &database,
                        client_socket,
                        key,
                        &match_count) < 0) {
                    break;
                }
                fprintf(
                    stderr,
                    "SEARCH for '%s' completed: %d match(es) found\n",
                    key,
                    match_count);
            } else if (strncmp(line, "ADD ", 4) == 0) {
                char *data = line + 4;
                char *pipe = strchr(data, '|');
                char *name;
                char *message;
                uint64_t assigned_id;
                enum MutationResult result;

                if (!pipe || strchr(pipe + 1, '|')) {
                    if (send_text(
                            client_socket,
                            "ERROR: Invalid ADD format\n") < 0) {
                        break;
                    }
                    continue;
                }
                *pipe = '\0';
                name = data;
                message = pipe + 1;

                if (validate_text_value(name, MAX_NAME_LEN, 1) < 0 ||
                    validate_text_value(message, MAX_MSG_LEN, 1) < 0) {
                    if (send_text(
                            client_socket,
                            "ERROR: Invalid name or message\n") < 0) {
                        break;
                    }
                    continue;
                }

                result = atomic_add(
                    &database,
                    filename,
                    name,
                    message,
                    &assigned_id);
                if (result == MUTATION_OK) {
                    char response[64];
                    int response_length = snprintf(
                        response,
                        sizeof(response),
                        "OK %" PRIu64 "\n",
                        assigned_id);
                    if (response_length < 0 ||
                        (size_t)response_length >= sizeof(response) ||
                        write_all(
                            client_socket,
                            response,
                            (size_t)response_length) < 0) {
                        break;
                    }
                } else if (send_mutation_error(
                               client_socket, result, "add") < 0) {
                    break;
                }
            } else if (strncmp(line, "DELETE ", 7) == 0) {
                uint64_t id;
                enum MutationResult result;

                if (parse_positive_u64(line + 7, &id) < 0) {
                    if (send_text(
                            client_socket,
                            "ERROR: Invalid record ID\n") < 0) {
                        break;
                    }
                    continue;
                }

                result = atomic_delete(&database, filename, id);
                if (result == MUTATION_OK) {
                    if (send_text(client_socket, "OK\n") < 0)
                        break;
                } else if (send_mutation_error(
                               client_socket, result, "delete") < 0) {
                    break;
                }
            } else if (strncmp(line, "UPDATE ", 7) == 0) {
                char *data = line + 7;
                char *first_pipe = strchr(data, '|');
                char *second_pipe;
                char *name;
                char *message;
                uint64_t id;
                enum MutationResult result;

                if (!first_pipe) {
                    if (send_text(
                            client_socket,
                            "ERROR: Invalid UPDATE format\n") < 0) {
                        break;
                    }
                    continue;
                }
                *first_pipe = '\0';
                if (parse_positive_u64(data, &id) < 0) {
                    if (send_text(
                            client_socket,
                            "ERROR: Invalid record ID\n") < 0) {
                        break;
                    }
                    continue;
                }

                second_pipe = strchr(first_pipe + 1, '|');
                if (!second_pipe || strchr(second_pipe + 1, '|')) {
                    if (send_text(
                            client_socket,
                            "ERROR: Invalid UPDATE format\n") < 0) {
                        break;
                    }
                    continue;
                }
                *second_pipe = '\0';
                name = first_pipe + 1;
                message = second_pipe + 1;

                if (validate_text_value(name, MAX_NAME_LEN, 1) < 0 ||
                    validate_text_value(message, MAX_MSG_LEN, 1) < 0) {
                    if (send_text(
                            client_socket,
                            "ERROR: Invalid name or message\n") < 0) {
                        break;
                    }
                    continue;
                }

                result = atomic_update(
                    &database,
                    filename,
                    id,
                    name,
                    message);
                if (result == MUTATION_OK) {
                    if (send_text(client_socket, "OK\n") < 0)
                        break;
                } else if (send_mutation_error(
                               client_socket, result, "update") < 0) {
                    break;
                }
            } else if (strcmp(line, "LIST2") == 0) {
                if (list_all_records_v2(&database, client_socket) < 0)
                    break;
            } else if (strcmp(line, "LIST") == 0) {
                if (list_all_records(&database, client_socket) < 0)
                    break;
            } else if (strcmp(line, "SAVE") == 0) {
                if (persist_database(filename, &database) == 0) {
                    if (send_text(client_socket, "OK\n") < 0)
                        break;
                } else if (send_text(
                               client_socket,
                               "ERROR: Failed to save\n") < 0) {
                    break;
                }
            } else {
                int match_count;

                if (validate_text_value(line, KEY_MAX, 0) < 0) {
                    if (send_text(
                            client_socket,
                            "ERROR: Invalid search key\n") < 0) {
                        break;
                    }
                    continue;
                }

                if (search_records(
                        &database,
                        client_socket,
                        line,
                        &match_count) < 0) {
                    break;
                }
                fprintf(
                    stderr,
                    "Search for '%s' completed: %d match(es) found\n",
                    line,
                    match_count);
            }
        }

        close(client_socket);
        fprintf(
            stderr,
            "connection terminated from: %s\n",
            inet_ntoa(client_address.sin_addr));
    }
}
