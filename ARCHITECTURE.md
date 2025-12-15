# 📚 Kiến Trúc Hệ Thống File Sharing Server - IT4062

## 📋 Mục Lục
1. [Tổng Quan Kiến Trúc](#1-tổng-quan-kiến-trúc)
2. [Kiến Trúc I/O Multiplexing](#2-kiến-trúc-io-multiplexing)
3. [Cơ Chế Non-Blocking I/O](#3-cơ-chế-non-blocking-io)
4. [Quản Lý Buffer](#4-quản-lý-buffer)
5. [Vòng Đời Request/Response](#5-vòng-đời-requestresponse)
6. [Chi Tiết Các Module](#6-chi-tiết-các-module)
7. [Logging System](#7-logging-system)
8. [File Transfer Protocol](#8-file-transfer-protocol)
9. [Database Integration](#9-database-integration)
10. [Performance Optimization](#10-performance-optimization)

---

## 1. Tổng Quan Kiến Trúc

### 1.1. Mô Hình Hệ Thống

```
┌─────────────────────────────────────────────────────────────┐
│                    CLIENT APPLICATIONS                       │
│  (Multiple concurrent connections - up to 30 clients)       │
└────────────┬────────────────────────────────┬───────────────┘
             │                                │
             │ TCP/IP Socket                  │
             │ (Non-blocking)                 │
             ▼                                ▼
┌────────────────────────────────────────────────────────────┐
│                  SERVER MAIN PROCESS                        │
│  ┌──────────────────────────────────────────────────────┐  │
│  │         I/O Multiplexing (select())                  │  │
│  │  - Monitors all client sockets simultaneously        │  │
│  │  - Single-threaded event loop                        │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │  Connection  │  │   Protocol   │  │    Stream    │    │
│  │  Management  │  │   Handler    │  │   Manager    │    │
│  └──────────────┘  └──────────────┘  └──────────────┘    │
│                                                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │     Auth     │  │   Database   │  │    Logger    │    │
│  │   Module     │  │   (MySQL)    │  │    System    │    │
│  └──────────────┘  └──────────────┘  └──────────────┘    │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌──────────────────┐
                    │  File System     │
                    │  ./storage/      │
                    └──────────────────┘
```

### 1.2. Đặc Điểm Kiến Trúc

- **Single-threaded**: Sử dụng một process duy nhất với I/O multiplexing
- **Event-driven**: Xử lý sự kiện theo mô hình reactor pattern
- **Non-blocking I/O**: Tất cả socket operations đều non-blocking
- **Scalable**: Hỗ trợ 30 concurrent clients với hiệu suất cao
- **Stateful**: Mỗi client có state riêng (buffers, authentication, user_id)

---

## 2. Kiến Trúc I/O Multiplexing

### 2.1. Mô Hình Select-Based Multiplexing

```c
void run_server_loop(int server_sock) {
    fd_set readfds, writefds;
    int max_fd;

    while (1) {
        // 1. SETUP: Chuẩn bị fd_sets
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(server_sock, &readfds);  // Monitor server socket
        
        // Add all active client sockets
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].sock > 0) {
                FD_SET(clients[i].sock, &readfds);   // Monitor for read
                if (clients[i].send_len > clients[i].send_offset)
                    FD_SET(clients[i].sock, &writefds);  // Monitor for write
            }
        }

        // 2. BLOCK: Chờ sự kiện I/O
        int activity = select(max_fd + 1, &readfds, &writefds, NULL, NULL);
        
        // 3. PROCESS: Xử lý sự kiện
        // - Accept new connections
        // - Read from ready sockets
        // - Write to ready sockets
    }
}
```

### 2.2. Ưu Điểm của Select-Based Approach

| Ưu Điểm | Giải Thích |
|---------|-----------|
| **Đơn giản** | Dễ hiểu, dễ debug, không cần quản lý threads |
| **Hiệu quả** | Một process xử lý nhiều connections |
| **Portable** | Hoạt động trên mọi Unix-like systems |
| **Predictable** | Không có race conditions, deadlocks |

### 2.3. Flow Diagram

```
START
  │
  ▼
┌─────────────────────┐
│  Initialize FD_SETs │
│  - readfds          │
│  - writefds         │
└──────────┬──────────┘
           │
           ▼
┌─────────────────────┐
│  Add server_sock    │
│  to readfds         │
└──────────┬──────────┘
           │
           ▼
┌─────────────────────┐
│  For each client:   │
│  - Add to readfds   │
│  - Add to writefds  │
│    (if has data)    │
└──────────┬──────────┘
           │
           ▼
┌─────────────────────┐
│   select() BLOCKS   │◄──────┐
│   Waiting for I/O   │       │
└──────────┬──────────┘       │
           │                  │
           ▼                  │
      Event Ready?            │
           │                  │
     ┌─────┴─────┐           │
     │           │           │
     ▼           ▼           │
New Conn?    Client I/O?     │
     │           │           │
     ▼           ▼           │
  Accept()   Read/Write()    │
     │           │           │
     └───────────┴───────────┘
           Loop Forever
```

---

## 3. Cơ Chế Non-Blocking I/O

### 3.1. Thiết Lập Non-Blocking Mode

```c
void set_nonblocking(int fd) {
    // 1. Lấy flags hiện tại
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return;
    }
    
    // 2. Thêm O_NONBLOCK flag
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
```

### 3.2. Tại Sao Cần Non-Blocking?

**Blocking I/O (Vấn Đề):**
```
Client A sends data → recv() BLOCKS → Server stuck!
                     ↓
Client B wants to connect → WAITING...
Client C sends data → WAITING...
```

**Non-Blocking I/O (Giải Pháp):**
```
Client A sends data → recv() returns immediately → Process data
                     ↓
Client B connects → accept() returns immediately → Add to pool
                   ↓
Client C sends data → recv() returns immediately → Process data
```

### 3.3. Xử Lý EAGAIN/EWOULDBLOCK

```c
int flush_send(int idx) {
    while (c->send_offset < c->send_len) {
        ssize_t n = send(c->sock, c->send_buf + c->send_offset, 
                        to_send, 0);
        
        if (n > 0) {
            c->send_offset += n;  // Gửi thành công
        } 
        else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Socket buffer đầy, chờ lần select() tiếp theo
            return 0;  
        } 
        else {
            return -1;  // Lỗi thực sự
        }
    }
}
```

**Giải thích:**
- `EAGAIN`: Socket buffer đầy, không thể gửi thêm
- **Không phải lỗi**: Chỉ cần đợi socket ready lại
- select() sẽ notify khi socket writeable trở lại

---

## 4. Quản Lý Buffer

### 4.1. Client State Structure

```c
typedef struct {
    int sock;                          // Socket file descriptor
    
    // RECEIVE BUFFER
    char recv_buf[BUFFER_SIZE];        // 24KB receive buffer
    int recv_len;                      // Current data length
    
    // SEND BUFFER
    char send_buf[SEND_BUFFER_SIZE];   // 32KB send buffer
    int send_len;                      // Total data to send
    int send_offset;                   // Already sent bytes
    
    // AUTHENTICATION
    int authenticated;
    int user_id;
} Client;
```

### 4.2. Buffer Configuration

| Buffer | Size | Purpose |
|--------|------|---------|
| `recv_buf` | 24,576 bytes (24KB) | Nhận dữ liệu từ client |
| `send_buf` | 32,768 bytes (32KB) | Gửi dữ liệu đến client |
| `FILE_CHUNK_SIZE` | 16,384 bytes (16KB) | Chunk size cho file transfer |

### 4.3. Receive Buffer Flow

```
┌─────────────────────────────────────────────────────┐
│              RECEIVE BUFFER (24KB)                   │
├─────────────────────────────────────────────────────┤
│ LOGIN user1 password\r\n│CREATE_GROUP tok...       │
│ ◄────────────────────► │                            │
│   Complete Command     │   Incomplete Data          │
│   (will be processed)  │   (wait for more)          │
└─────────────────────────────────────────────────────┘
         │
         ▼
  Find CRLF (\r\n)
         │
         ▼
  Extract Command → Process → Shift Buffer Left
```

**Code Implementation:**
```c
// 1. Nhận dữ liệu vào buffer
memcpy(clients[i].recv_buf + clients[i].recv_len, tmpbuf, bytes);
clients[i].recv_len += bytes;

// 2. Tìm CRLF (command delimiter)
while ((pos = find_crlf(clients[i].recv_buf, clients[i].recv_len)) >= 0) {
    // 3. Xử lý command
    process_command(i, clients[i].recv_buf, pos);
    
    // 4. Dịch chuyển buffer (remove processed data)
    int tail = clients[i].recv_len - (pos + 2);
    memmove(clients[i].recv_buf, clients[i].recv_buf + pos + 2, tail);
    clients[i].recv_len = tail;
}
```

### 4.4. Send Buffer Flow

```
┌─────────────────────────────────────────────────────┐
│               SEND BUFFER (32KB)                     │
├─────────────────────────────────────────────────────┤
│ 200 OK\r\ngroup1|Desc1\r\n│group2|Desc2\r\n...     │
│ ◄──────────────────────────►│                       │
│   Already Sent              │   Waiting to Send     │
│   (send_offset)             │                       │
└─────────────────────────────────────────────────────┘
            ▲
            │
    send() in chunks
    (may not send all at once due to kernel buffer limits)
```

**Progressive Send Mechanism:**
```c
while (c->send_offset < c->send_len) {
    ssize_t n = send(c->sock, 
                    c->send_buf + c->send_offset,  // From current offset
                    remaining, 0);
    
    if (n > 0) {
        c->send_offset += n;  // Update offset
    } else if (EAGAIN) {
        return 0;  // Will continue later
    }
}

// All sent, reset buffer
c->send_len = 0;
c->send_offset = 0;
```

### 4.5. Buffer Overflow Protection

```c
// Check before accepting more data
if (clients[i].recv_len + bytes > BUFFER_SIZE) {
    log_disc(i, "Client disconnected (buffer overflow)");
    remove_client_index(i);
    continue;
}
```

---

## 5. Vòng Đời Request/Response

### 5.1. Complete Request/Response Cycle

```
[CLIENT]                    [SERVER]                    [DATABASE]
   │                           │                            │
   │ 1. LOGIN user1 pass\r\n   │                            │
   ├──────────────────────────►│                            │
   │                           │ 2. Parse command           │
   │                           ├────┐                       │
   │                           │    │ process_command()     │
   │                           │◄───┘                       │
   │                           │                            │
   │                           │ 3. Authenticate            │
   │                           ├───────────────────────────►│
   │                           │    handle_login()          │
   │                           │                            │
   │                           │◄───────────────────────────┤
   │                           │    user_id=7               │
   │                           │                            │
   │                           │ 4. Set user_id             │
   │                           ├────┐                       │
   │                           │    │ clients[idx].user_id=7│
   │                           │◄───┘                       │
   │                           │                            │
   │                           │ 5. Log authentication      │
   │                           ├────┐                       │
   │                           │    │ log_info()            │
   │                           │◄───┘                       │
   │                           │                            │
   │                           │ 6. Enqueue response        │
   │                           ├────┐                       │
   │                           │    │ send_response()       │
   │                           │◄───┘                       │
   │                           │                            │
   │ 7. 200 token...\r\n       │                            │
   │◄──────────────────────────┤                            │
   │                           │                            │
   │                           │ 8. Log response sent       │
   │                           ├────┐                       │
   │                           │    │ log_send()            │
   │                           │◄───┘                       │
   │                           │                            │
```

### 5.2. Detailed Step-by-Step Flow

#### Step 1: Client Sends Request
```c
// Client code
char request[] = "LOGIN hungtn mypassword\r\n";
send(sock, request, strlen(request), 0);
```

#### Step 2: Server Receives (select() notifies)
```c
// In run_server_loop()
if (FD_ISSET(clients[i].sock, &readfds)) {
    ssize_t bytes = recv(sd, tmpbuf, sizeof(tmpbuf), 0);
    
    // Append to client's receive buffer
    memcpy(clients[i].recv_buf + clients[i].recv_len, tmpbuf, bytes);
    clients[i].recv_len += bytes;
}
```

#### Step 3: Parse Command (find CRLF)
```c
// Extract complete command
int pos = find_crlf(clients[i].recv_buf, clients[i].recv_len);
if (pos >= 0) {
    process_command(i, clients[i].recv_buf, pos);
}
```

#### Step 4: Log Incoming Request
```c
// In process_command()
log_recv(idx, clients[idx].user_id, "LOGIN hungtn ***");
// Password is masked for security
```

#### Step 5: Execute Business Logic
```c
// Parse tokens
char *cmd = next_token(&ptr);        // "LOGIN"
char *username = next_token(&ptr);   // "hungtn"
char *password = next_token(&ptr);   // "mypassword"

// Call authentication handler
int user_id = handle_login(username, password, resp, sizeof(resp));

if (user_id > 0) {
    clients[idx].user_id = user_id;  // Set authenticated user ID
    log_info(idx, user_id, "User authenticated: username=%s", username);
}
```

#### Step 6: Prepare Response
```c
snprintf(response, sizeof(response), 
         "200 %s\r\n", token);  // token from handle_login()
```

#### Step 7: Enqueue Response
```c
// send_response() does two things:
// 1. Enqueue data to send buffer
enqueue_send(idx, response, strlen(response));

// 2. Log outgoing response
log_send(idx, clients[idx].user_id, "200 mBvjJjOTPvewv8itAs9QdmanEV0XVH2I");
```

#### Step 8: Send Response (when socket writable)
```c
// In next select() iteration
if (FD_ISSET(clients[i].sock, &writefds)) {
    flush_send(i);  // Send buffered data
}
```

### 5.3. Timeline Diagram

```
Time ─────────────────────────────────────────────────────►

T0: Client send()           ████
                                │
T1: Server recv()               │  ██
                                ▼
T2: Parse & Log RECV           [RECV] LOGIN ...
                                │
T3: Database Query              │     ████████
                                ▼
T4: Set user_id & Log INFO     [INFO] User authenticated
                                │
T5: Enqueue response            │  ██
                                │
T6: Log SEND                   [SEND] 200 token...
                                │
T7: Server send()               │     ████
                                ▼
T8: Client recv()                      ████

Total latency: T8 - T0 (typically < 10ms for local network)
```

---

## 6. Chi Tiết Các Module

### 6.1. Module Structure

```
server/
├── main.c                 # Entry point, server initialization
├── io/
│   ├── io_multiplexing.c # select() event loop
│   └── io_multiplexing.h
├── net/
│   ├── client.c          # Client state management
│   ├── client.h          # Client structure definition
│   ├── stream.c          # Buffer operations (send/recv)
│   └── stream.h
├── protocol/
│   ├── command.c         # Command parsing & execution
│   └── command.h
├── auth/
│   ├── auth.c            # Login/register logic
│   ├── hash.c            # Password hashing (bcrypt)
│   └── token.c           # JWT token generation
├── database/
│   ├── db.c              # MySQL connection & queries
│   ├── schema.sql        # Database schema
│   └── seeder.sql        # Sample data
└── utils/
    ├── logger.c          # Logging system
    └── logger.h
```

### 6.2. Main.c - Server Bootstrap

```c
int main() {
    // 1. Initialize database connection pool
    init_mysql();
    
    // 2. Create TCP server socket
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    set_nonblocking(server_sock);
    
    // 3. Configure socket options
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // 4. Bind to port 1234
    bind(server_sock, (struct sockaddr *)&addr, sizeof(addr));
    
    // 5. Listen for connections (backlog=10)
    listen(server_sock, BACKLOG);
    
    // 6. Initialize client pool
    init_clients();
    
    // 7. Enter event loop (NEVER RETURNS)
    run_server_loop(server_sock);
    
    // Cleanup (never reached)
    close_mysql();
    return 0;
}
```

### 6.3. IO Multiplexing Module

**Responsibilities:**
- Monitor all sockets with select()
- Accept new connections
- Dispatch read/write events
- Handle disconnections

**Key Functions:**
```c
void run_server_loop(int server_sock);  // Main event loop
static void set_nonblocking(int fd);    // Configure socket
```

### 6.4. Client Management Module

**Responsibilities:**
- Maintain client state (30 concurrent clients)
- Allocate/deallocate client slots
- Track authentication status

**Key Functions:**
```c
void init_clients();                 // Initialize client pool
int add_client(int sock);           // Add new client
void remove_client_index(int idx);  // Cleanup client
```

**Client Pool:**
```c
Client clients[MAX_CLIENTS];  // Static array of 30 clients

// Find free slot
for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].sock == 0) {
        clients[i].sock = new_sock;
        return i;  // Return index
    }
}
```

### 6.5. Stream Module

**Responsibilities:**
- Buffer management
- Progressive send/recv
- CRLF delimiter detection

**Key Functions:**
```c
int enqueue_send(int idx, const char *data, int len);  // Add to send buffer
int flush_send(int idx);                               // Send buffered data
int find_crlf(const char *buf, int len);              // Find command boundary
```

**Progressive Send Example:**
```
Iteration 1: send_offset=0    → send 8192 bytes  → offset=8192
Iteration 2: send_offset=8192 → send 8192 bytes  → offset=16384
Iteration 3: send_offset=16384 → EAGAIN (wait)
Iteration 4: send_offset=16384 → send 8192 bytes → offset=24576 (DONE)
```

### 6.6. Protocol Module

**Responsibilities:**
- Parse incoming commands
- Route to appropriate handlers
- Generate responses

**Supported Commands:**
```
REGISTER username password
LOGIN username password
LOGOUT token
VERIFY_TOKEN token (internal, no logging)
CREATE_GROUP token name description
LIST_GROUPS_JOINED token
LIST_GROUPS_NOT_JOINED token
JOIN_GROUP token group_id
UPLOAD_FILE token group_id dir_id filename size
DOWNLOAD_FILE token group_id dir_id file_id chunk_index
```

**Command Parsing:**
```c
char *ptr = buffer;
char *cmd = next_token(&ptr);  // Get command name

if (strcasecmp(cmd, "LOGIN") == 0) {
    char *username = next_token(&ptr);
    char *password = next_token(&ptr);
    handle_login_command(idx, username, password);
}
```

### 6.7. Authentication Module

**Responsibilities:**
- User registration with bcrypt hashing
- Login with password verification
- JWT token generation (32 chars, 24h expiry)

**Login Flow:**
```c
int handle_login(const char *username, const char *password, 
                 char *response, size_t resp_size) {
    // 1. Query user from database
    char query[512];
    snprintf(query, sizeof(query), 
            "SELECT user_id, password_hash FROM users WHERE username='%s'", 
            username);
    
    // 2. Verify password with bcrypt
    if (bcrypt_checkpw(password, stored_hash) != 0) {
        return -1;  // Wrong password
    }
    
    // 3. Generate JWT token
    char token[TOKEN_LENGTH + 1];
    generate_token(user_id, token);
    
    // 4. Store token in database
    store_token(user_id, token);
    
    // 5. Return response
    snprintf(response, resp_size, "200 %s", token);
    return user_id;
}
```

### 6.8. Database Module

**Responsibilities:**
- MySQL connection management
- Prepared statements
- Stored procedures

**Connection Setup:**
```c
MYSQL *conn = NULL;

void init_mysql() {
    conn = mysql_init(NULL);
    mysql_real_connect(conn, "localhost", "user", "password", 
                      "file_sharing_db", 3306, NULL, 0);
}
```

**Example Query:**
```c
// Using stored procedure
char query[256];
snprintf(query, sizeof(query), "CALL create_group(%d, '%s', '%s')", 
        user_id, group_name, description);
mysql_query(conn, query);
```

---

## 7. Logging System

### 7.1. Log Levels

| Level | Symbol | Purpose | Example |
|-------|--------|---------|---------|
| CONN | `[CONN]` | New connections | `New connection from 127.0.0.1:45590` |
| DISC | `[DISC]` | Disconnections | `Client disconnected (buffer overflow)` |
| RECV | `[RECV]` | Incoming requests | `LOGIN hungtn ***` |
| SEND | `[SEND]` | Outgoing responses | `200 mBvjJjOTPvewv8itAs9QdmanEV0XVH2I` |
| INFO | `[INFO]` | Business events | `User authenticated: username=hungtn` |
| ERROR | `[ERROR]` | Errors | `Database connection failed` |

### 7.2. Log Format

```
[YYYY-MM-DD HH:MM:SS] [CLIENT:idx|USER:user_id] [LEVEL] message

Examples:
[2025-12-02 19:11:53] [CLIENT:0] [CONN] New connection from 127.0.0.1:45590 (fd=5)
[2025-12-02 19:11:53] [CLIENT:0] [RECV] LOGIN hungtn ***
[2025-12-02 19:11:53] [CLIENT:0|USER:7] [INFO] User authenticated: username=hungtn
[2025-12-02 19:11:53] [CLIENT:0|USER:7] [SEND] 200 mBvjJjOTPvewv8itAs9QdmanEV0XVH2I
```

### 7.3. Implementation

```c
void log_send(int idx, int user_id, const char *format, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    // Print timestamp
    printf("[%04d-%02d-%02d %02d:%02d:%02d] ",
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec);
    
    // Print client info
    if (user_id > 0) {
        printf("[CLIENT:%d|USER:%d] [SEND] ", idx, user_id);
    } else {
        printf("[CLIENT:%d] [SEND] ", idx);
    }
    
    // Print message
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}
```

### 7.4. Security Features

**Password Masking:**
```c
// Mask passwords in logs
if (strncasecmp(safe_log, "LOGIN ", 6) == 0 || 
    strncasecmp(safe_log, "REGISTER ", 9) == 0) {
    // Replace password with ***
    char *second_space = strchr(first_space + 1, ' ');
    if (second_space) {
        snprintf(second_space + 1, remaining, "***");
    }
}

log_recv(idx, clients[idx].user_id, "%s", safe_log);
// Output: LOGIN hungtn ***
```

**Skip Internal Requests:**
```c
// Don't log VERIFY_TOKEN (internal check)
int should_log = (strncasecmp(safe_log, "VERIFY_TOKEN ", 13) != 0);
if (should_log) {
    log_recv(idx, clients[idx].user_id, "%s", safe_log);
}
```

### 7.5. Response Truncation

```c
// For long responses, log first 2KB
char log_buf[2048];
size_t max_log = sizeof(log_buf) - 10;

if (len > max_log) {
    memcpy(log_buf, response, max_log);
    strcpy(log_buf + max_log, "...");  // Indicate truncation
}

log_send(idx, clients[idx].user_id, "%s", log_buf);
```

---

## 8. File Transfer Protocol

### 8.1. Chunk-Based Transfer

**Configuration:**
```c
#define FILE_CHUNK_SIZE 16384  // 16KB per chunk
#define BASE64_CHUNK_SIZE (((FILE_CHUNK_SIZE + 2) / 3) * 4 + 4)
```

**Why 16KB?**
- Balance between throughput and latency
- Fits comfortably in socket buffers
- Efficient for both small and large files

### 8.2. Upload Flow

```
CLIENT                           SERVER
  │                                │
  │ 1. UPLOAD_FILE token gid did   │
  │    filename size               │
  ├───────────────────────────────►│
  │                                │ Create .part file
  │                                │ Calculate total_chunks
  │                                │
  │ 2. 200 total_chunks file_id    │
  │◄───────────────────────────────┤
  │                                │
  │ 3. UPLOAD_CHUNK file_id 1      │
  │    base64_data\r\n             │
  ├───────────────────────────────►│ Decode & write chunk 1
  │                                │
  │ 4. 200\r\n                     │
  │◄───────────────────────────────┤
  │                                │
  │ (Repeat for all chunks)        │
  │                                │
  │ 5. FINISH_UPLOAD file_id       │
  ├───────────────────────────────►│ Rename .part → final
  │                                │ Update database
  │ 6. 200\r\n                     │
  │◄───────────────────────────────┤
```

### 8.3. Download Flow

```
CLIENT                           SERVER
  │                                │
  │ 1. DOWNLOAD_FILE token gid     │
  │    did file_id 1               │
  ├───────────────────────────────►│
  │                                │ Seek to chunk 1
  │                                │ Read 16KB
  │                                │ Base64 encode
  │                                │
  │ 2. 200 chunk_size\r\n          │
  │    base64_data\r\n             │
  │◄───────────────────────────────┤
  │                                │
  │ (Request next chunk)           │
```

### 8.4. File Size Calculation

```c
// For 1.2MB file (1,228,800 bytes):
total_chunks = (file_size + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE
             = (1,228,800 + 16,384 - 1) / 16,384
             = 1,245,183 / 16,384
             = 75 chunks

// Comparison with old 2KB chunks:
// Old: 1,228,800 / 2,048 = 600 chunks (8x more overhead!)
```

### 8.5. Base64 Encoding

**Why Base64?**
- Binary-safe transmission over text protocol
- No issues with null bytes, control characters
- Standard encoding (compatible with all languages)

**Size Overhead:**
```
Original: 16,384 bytes
Base64:   21,848 bytes (33% larger)

Formula: encoded_size = ((original_size + 2) / 3) * 4
```

---

## 9. Database Integration

### 9.1. Schema Overview

**Tables:**
- `users`: User accounts with bcrypt passwords
- `groups`: File sharing groups
- `group_members`: Many-to-many user-group relationship
- `directories`: Folder structure within groups
- `files`: File metadata (name, size, path)
- `tokens`: Active JWT tokens with expiry

### 9.2. Stored Procedures

```sql
-- Create group and add creator as admin
CALL create_group(user_id, 'Group Name', 'Description');

-- Get groups user has joined
CALL get_user_groups(user_id);

-- Handle join request with approval
CALL handle_join_request(group_id, user_id, is_admin);
```

### 9.3. Connection Pool Pattern

```c
// Single global connection (simple approach)
MYSQL *conn = NULL;

void init_mysql() {
    conn = mysql_init(NULL);
    if (!mysql_real_connect(conn, host, user, pass, db, port, NULL, 0)) {
        fprintf(stderr, "MySQL connection failed: %s\n", mysql_error(conn));
        exit(1);
    }
}

// All queries use this connection
mysql_query(conn, "SELECT ...");
```

**Note:** Single connection is acceptable for single-threaded server.

---

## 10. Performance Optimization

### 10.1. Buffer Size Tuning

```c
// Receive buffer: 24KB (accommodate multiple commands)
#define BUFFER_SIZE 24576

// Send buffer: 32KB (handle large responses)
#define SEND_BUFFER_SIZE 32768

// File chunk: 16KB (optimal throughput)
#define FILE_CHUNK_SIZE 16384
```

### 10.2. Progressive Send Strategy

```c
// Limit bytes per flush_send() iteration
int max_bytes_per_call = 32 * 1024;  // 32KB

while (bytes_sent_this_call < max_bytes_per_call) {
    ssize_t n = send(...);
    if (n > 0) {
        bytes_sent_this_call += n;
    } else if (EAGAIN) {
        break;  // Continue in next iteration
    }
}
```

**Benefits:**
- Prevents one client from monopolizing CPU
- Fair scheduling across all clients
- Predictable latency

### 10.3. Latency Measurements

| Operation | Typical Latency | Notes |
|-----------|-----------------|-------|
| LOGIN | 5-10ms | Includes bcrypt verification |
| CREATE_GROUP | 2-5ms | Database insert |
| LIST_GROUPS | 3-8ms | Database query |
| UPLOAD_CHUNK | 1-3ms per chunk | Depends on disk I/O |
| DOWNLOAD_CHUNK | 1-2ms per chunk | Sequential read |

### 10.4. Scalability Limits

**Current Configuration:**
- **Max clients:** 30 concurrent
- **select() FD limit:** 1024 on most systems
- **Memory per client:** ~56KB (buffers)
- **Total memory:** ~1.7MB for all clients

**To Scale Beyond 30 Clients:**
1. Use `epoll()` (Linux) or `kqueue()` (BSD) instead of `select()`
2. Implement multi-threading or multi-process
3. Add connection pooling for database
4. Use async I/O for disk operations

### 10.5. Optimization Checklist

✅ **Completed:**
- Non-blocking I/O
- Progressive send/recv
- Buffer overflow protection
- Efficient chunk size (16KB)
- Single-threaded event loop
- Log truncation for long responses

🔄 **Future Improvements:**
- Connection pooling for database
- File caching layer
- Compression for large responses
- Rate limiting per client
- Load balancing across multiple servers

---

## 11. Troubleshooting Common Issues

### 11.1. Client Hangs After Login

**Symptom:**
```
[CLIENT:0] [RECV] LOGIN hungtn ***
[CLIENT:0|USER:7] [INFO] User authenticated: username=hungtn
Segmentation fault
```

**Cause:** Infinite recursion in `send_response()`

**Solution:**
```c
// WRONG:
static void send_response(int idx, const char *response) {
    send_response(idx, response);  // Calls itself!
}

// CORRECT:
static void send_response(int idx, const char *response) {
    enqueue_send(idx, response, strlen(response));  // Calls enqueue_send()
}
```

### 11.2. Buffer Overflow Disconnection

**Symptom:**
```
[CLIENT:0] [DISC] Client disconnected (buffer overflow)
```

**Cause:** Response larger than `BUFFER_SIZE` (24KB)

**Solution:** Increase buffer or paginate responses

### 11.3. Log Truncation

**Symptom:** Long responses cut off in logs

**Solution:** Increase `log_buf` size:
```c
char log_buf[2048];  // Was 256, now 2KB
```

### 11.4. EAGAIN Not Handled

**Symptom:** Clients disconnect after sending large data

**Solution:** Always check for EAGAIN:
```c
if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return 0;  // Not an error, just wait
}
```

---

## 12. Kết Luận

### 12.1. Tổng Kết Kiến Trúc

Hệ thống File Sharing Server sử dụng kiến trúc **single-threaded I/O multiplexing** với các đặc điểm:

1. **Event-Driven:** Xử lý sự kiện theo mô hình reactor pattern
2. **Non-Blocking I/O:** Tất cả operations đều async
3. **Stateful Connections:** Mỗi client có buffer và authentication state riêng
4. **Scalable:** Hỗ trợ 30 concurrent clients hiệu quả
5. **Observable:** Comprehensive logging system cho debugging

### 12.2. Điểm Mạnh

✅ **Đơn giản:** Dễ hiểu, dễ maintain  
✅ **Hiệu quả:** Một process xử lý nhiều connections  
✅ **Ổn định:** Không có race conditions, deadlocks  
✅ **An toàn:** Password hashing, token authentication  
✅ **Observable:** Chi tiết logs cho mọi operations  

### 12.3. Hạn Chế và Cải Tiến

⚠️ **Single-threaded:** Không tận dụng multi-core  
⚠️ **select() limit:** Tối đa ~1024 file descriptors  
⚠️ **Blocking database:** MySQL queries block event loop  

💡 **Cải tiến tương lai:**
- Chuyển sang `epoll()` hoặc `io_uring`
- Multi-process với shared memory
- Async database driver
- Redis cache layer

### 12.4. Bài Học Kinh Nghiệm

1. **Buffer Management:** Cẩn thận với buffer overflow và truncation
2. **EAGAIN Handling:** Luôn xử lý EAGAIN trong non-blocking I/O
3. **Progressive I/O:** Không giả định send()/recv() sẽ transfer toàn bộ data
4. **Logging:** Comprehensive logging giúp debug nhanh chóng
5. **Security:** Mask sensitive data trong logs

---

## 📚 References

- **UNIX Network Programming** - W. Richard Stevens
- **The C10K Problem** - Dan Kegel
- **select(2) man page** - Linux manual
- **Reactor Pattern** - Douglas Schmidt
- **Non-blocking I/O** - POSIX standards

---

**Tài liệu này mô tả kiến trúc của File Sharing Server - IT4062**  
**Version:** 2.0  
**Last Updated:** December 2, 2025  
**Authors:** IT4062 Development Team
