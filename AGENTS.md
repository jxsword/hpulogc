## `zcode agent.md` — C Coding Conventions

### 1. Source File Encoding

- All `.c` and `.h` files **must** be encoded in **UTF-8 without BOM**.
- Source files should start with a comment declaring the encoding when non-ASCII characters are present:

```c
/* -*- coding: utf-8 -*- */
```

- Non-ASCII characters are only allowed inside string literals (e.g., log messages). **Never** use non-ASCII in identifiers.

### 2. Comments and Documentation

#### 2.1 General Rule

- **All comments must be written in English.**
- Comments explain **intent and reasoning**, not a restatement of what the code does.

#### 2.2 Doxygen Format (Required)

- **All public APIs (functions, structs, enums, macros exposed in headers) must be documented in Doxygen format.**
- Use `/*!<` for member documentation (right-side comment style).
- Use `/** ... */` for function, struct, enum, and file-level documentation.
- Use `@` (preferred) or `\` as the Doxygen command prefix — **be consistent within the project; `@` is preferred.**

```c
/**
 * @file data_processor.h
 * @brief Declarations for incoming data frame processing.
 *
 * This module handles frame parsing, CRC validation, and dispatch.
 */

/**
 * @brief Connection state machine states.
 *
 * Used internally by connection_handle_t to track lifecycle.
 */
typedef enum {
    STATE_IDLE = 0,        /*!< Initial state, no connection */
    STATE_CONNECTING,      /*!< Handshake in progress */
    STATE_CONNECTED,       /*!< Active data channel */
    STATE_ERROR            /*!< Fatal error, requires reset */
} connection_state_t;

/**
 * @brief Handle for a single connection instance.
 */
typedef struct {
    int socket_fd;                 /*!< File descriptor, -1 if closed */
    connection_state_t state;      /*!< Current state machine state */
    char remote_addr[INET6_ADDRSTRLEN]; /*!< Peer address string */
} connection_handle_t;

/**
 * @brief Open a connection to the specified host and port.
 *
 * @param host  Target hostname or IP address (IPv4 or IPv6).
 * @param port  Target TCP port number.
 * @return      Pointer to a connection_handle_t on success,
 *              NULL on failure (errno is set accordingly).
 *
 * @note The caller is responsible for calling close_connection()
 *       on the returned handle when done.
 */
connection_handle_t *open_connection(const char *host, int port);

/**
 * @brief Close an open connection and release associated resources.
 *
 * @param handle  Pointer to a valid connection_handle_t.
 *                Passing NULL is safe and does nothing.
 */
void close_connection(connection_handle_t *handle);
```

#### 2.3 Doxygen Command Reference (Commonly Used)

| Command | Purpose | Example |
|---------|---------|---------|
| `@brief` | Short description | `@brief Initialize the buffer pool.` |
| `@param` | Parameter description | `@param size  Total buffer size in bytes.` |
| `@return` | Return value description | `@return 0 on success, negative errno on failure.` |
| `@retval` | Specific return value | `@retval NULL  Invalid input or allocation failed.` |
| `@note` | Additional note | `@note This function is not thread-safe.` |
| `@warning` | Warning/caution | `@warning Do not call from interrupt context.` |
| `@see` | Cross-reference | `@see close_connection()` |
| `@todo` | Pending work | `@todo Add timeout support.` |
| `@file` | File-level doc | `@file uart_driver.c` |
| `@defgroup` | Grouping symbols | `@defgroup uart_driver UART Driver` |
| `@ingroup` | Add to group | `@ingroup uart_driver` |

#### 2.4 Internal (Static) Functions

- `static` functions need **only brief `/*` comments**, not full Doxygen.
- If a static function is complex enough to warrant it, use Doxygen anyway.

```c
/* retry up to MAX_RETRIES times to handle transient I/O errors */
static int attempt_write(int fd, const uint8_t *buf, size_t len)
{
    ...
}
```

### 3. Naming Conventions

- **All identifiers use `snake_case` (lowercase with underscores).**
- **CamelCase, PascalCase, and Hungarian notation are strictly forbidden.**

| Element | Convention | Example |
|---------|-----------|---------|
| Variables (local/global) | `snake_case` | `user_count`, `buffer_size` |
| Functions | `snake_case` | `calculate_crc()`, `init_connection()` |
| `struct` / `union` names | `snake_case` | `struct request_header` |
| `typedef` types | `snake_case` + `_t` suffix | `typedef struct {...} request_header_t` |
| Enum constants | `UPPER_SNAKE_CASE` | `STATE_READY`, `ERR_TIMEOUT` |
| Macros / `#define` | `UPPER_SNAKE_CASE` | `MAX_BUFFER_SIZE`, `DEBUG_MODE` |
| File names | `snake_case` | `data_processor.c`, `uart_driver.h` |
| `goto` labels | `snake_case` | `cleanup`, `retry` |

```c
/* good */
#define MAX_RETRY_COUNT    3
#define DEFAULT_TIMEOUT_MS 5000

typedef enum {
    STATE_IDLE = 0,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_ERROR
} connection_state_t;

struct connection_handle {
    int socket_fd;
    connection_state_t state;
    char *remote_addr;
};

int open_connection(const char *host, int port);
void close_connection(int socket_fd);
static int resolve_hostname(const char *host, char *out_ip);

/* bad */
#define MaxRetryCount 3          /* camelCase macro */
typedef struct ConnectionHandle { /* PascalCase struct */
    int socketFD;                 /* camelCase member */
} ConnHandle;
int OpenConnection(...);          /* PascalCase function */
```

### 4. Struct and Typedef Style

```c
/**
 * @brief Data channel descriptor.
 */
typedef struct {
    uint32_t total_bytes;  /*!< Cumulative bytes transferred */
    uint16_t packet_count; /*!< Number of packets sent */
    uint8_t  is_open;      /*!< Flag: channel is active */
} data_channel_t;

/* self-referencing structs use a named tag */
typedef struct node {
    int value;             /*!< Node payload */
    struct node *next;     /*!< Pointer to next node, NULL at tail */
} node_t;
```

### 5. Function and Variable Rules

- Function names should be **verb phrases** describing the action.
- Global variables use `snake_case`; prefix with `g_` only if the project explicitly requires it (otherwise avoid globals).
- `static` functions and variables must be used to limit visibility to the translation unit.
- Constants should prefer `static const` over `#define` when possible (type safety).

```c
static const int max_buffer_size = 1024;  /* preferred over #define */

static int parse_header(const uint8_t *raw_data, size_t len);
```

### 6. Complete Example

```c
/* -*- coding: utf-8 -*- */

/**
 * @file data_processor.c
 * @brief Handles incoming data frames, validates CRC, and dispatches.
 *
 * @ingroup data_processor
 */

#include "data_processor.h"
#include <string.h>

#define HEADER_MAGIC    0xAA55          /**< Frame header magic number */
#define MAX_PAYLOAD_LEN 256             /**< Maximum payload size in bytes */

/**
 * @brief Result codes for frame parsing.
 */
typedef enum {
    PARSE_OK = 0,        /*!< Frame parsed successfully */
    PARSE_ERR_MAGIC,     /*!< Invalid magic number */
    PARSE_ERR_CRC,       /*!< CRC mismatch */
    PARSE_ERR_LENGTH     /*!< Length out of bounds */
} parse_result_t;

/**
 * @brief Compute CRC-16 using CCITT polynomial.
 *
 * @param data  Pointer to data buffer.
 * @param len   Number of bytes to process.
 * @return      Computed CRC-16 value.
 */
static uint16_t calculate_crc(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/**
 * @brief Process an incoming data frame.
 *
 * @param frame      Pointer to raw frame buffer.
 * @param frame_len  Total length of the frame in bytes.
 * @return           parse_result_t indicating success or failure reason.
 *
 * @note This function is safe to call from any context except ISR.
 */
parse_result_t process_incoming_frame(const uint8_t *frame, size_t frame_len)
{
    if (frame_len < 6) {
        return PARSE_ERR_LENGTH;
    }

    uint16_t magic = (frame[0] << 8) | frame[1];
    if (magic != HEADER_MAGIC) {
        return PARSE_ERR_MAGIC;
    }

    uint16_t payload_len = (frame[2] << 8) | frame[3];
    if (payload_len > MAX_PAYLOAD_LEN || payload_len + 6 > frame_len) {
        return PARSE_ERR_LENGTH;
    }

    uint16_t expected_crc = calculate_crc(frame + 4, payload_len + 2);
    uint16_t actual_crc   = (frame[payload_len + 4] << 8) | frame[payload_len + 5];
    if (expected_crc != actual_crc) {
        return PARSE_ERR_CRC;
    }

    /* dispatch to handler */
    dispatch_payload(frame + 4, payload_len);

    return PARSE_OK;
}
```

### 7. Enforcement

- **Doxygen**: Run `doxygen` as part of CI; warnings should be treated as errors (`WARNINGS = YES`, `WARN_AS_ERROR = YES`).
- **Naming**: Use `clang-tidy` with a custom `.clang-tidy` config, or `checkpatch.pl` with `--ignore CAMELCASE` disabled (i.e., enforce snake_case).
- **Encoding**: A pre-commit hook runs `file -i` or `chardet` to verify UTF-8; non-UTF-8 files are rejected.
- **Comment language**: A simple script (or `codespell` with a custom dictionary) flags Chinese/non-ASCII comment text.

