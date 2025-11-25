# 📁 IT4062 File Sharing System

Hệ thống chia sẻ file dựa trên nhóm (group-based file sharing) với kiến trúc Client-Server sử dụng TCP/IP, I/O Multiplexing và Token-based Authentication.

## 🎯 Tính năng

### ✅ Đã triển khai
- **Xác thực người dùng (Authentication)**
  - Đăng ký tài khoản với username/password
  - Đăng nhập với token-based session
  - Mã hóa password bằng SHA256
  - Session timeout (24 giờ)
  - Logout và invalidate token
  - Persistent connection (giữ kết nối giữa các request)

- **Server Architecture**
  - I/O Multiplexing với `select()` system call
  - Non-blocking sockets
  - Xử lý nhiều client đồng thời
  - Buffer-based send/recv để tối ưu performance

- **Security**
  - Password hashing (SHA256)
  - Token-based authentication (32-character random tokens)
  - Token expiry management
  - Hidden password input (terminal)
  - Sensitive data không xuất hiện trong logs

### 🚧 Đang phát triển
- Quản lý nhóm (tạo/xóa/mời thành viên)
- Upload/Download files
- Quản lý thư mục (directories)
- Activity logging
- Permission system

---

## 🏗️ Kiến trúc hệ thống

```
┌─────────────────────────────────────────────────────────────┐
│                         CLIENT                              │
│  ┌────────────┐  ┌────────────┐  ┌─────────────────────┐   │
│  │   Menu UI  │  │  Commands  │  │  Persistent Socket  │   │
│  └────────────┘  └────────────┘  └─────────────────────┘   │
└───────────────────────────┬─────────────────────────────────┘
                            │ TCP/IP (127.0.0.1:1234)
                            │ Protocol: TEXT-BASED + CRLF
┌───────────────────────────┴─────────────────────────────────┐
│                         SERVER                              │
│  ┌─────────────────────────────────────────────────────┐   │
│  │          I/O Multiplexing (select())                │   │
│  │  ┌──────────┐  ┌──────────┐  ┌─────────────────┐   │   │
│  │  │ Accept() │  │  Read()  │  │    Write()      │   │   │
│  │  └──────────┘  └──────────┘  └─────────────────┘   │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │          Protocol Command Handler                   │   │
│  │   LOGIN | REGISTER | VERIFY_TOKEN | LOGOUT          │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │          Authentication Layer                       │   │
│  │  ┌──────────┐  ┌──────────┐  ┌─────────────────┐   │   │
│  │  │   Auth   │  │   Hash   │  │     Token       │   │   │
│  │  └──────────┘  └──────────┘  └─────────────────┘   │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │          Database Layer (MySQL)                     │   │
│  │   users | user_sessions | groups | files | ...      │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

---

## 📂 Cấu trúc thư mục

```
IT4062-file-sharing/
├── server/
│   ├── main.c                      # Entry point server
│   ├── Makefile                    # Build configuration
│   ├── client.c                    # Client application
│   │
│   ├── auth/                       # Authentication layer
│   │   ├── auth.c/h               # Register/Login logic
│   │   ├── hash.c/h               # SHA256 password hashing
│   │   └── token.c/h              # Token generation & verification
│   │
│   ├── database/                   # Database layer
│   │   ├── db.c/h                 # MySQL connection
│   │   ├── schema.sql             # Database schema
│   │   └── seeder.sql             # Sample data
│   │
│   ├── io/                         # I/O Management
│   │   └── io_multiplexing.c/h   # select() based multiplexing
│   │
│   ├── net/                        # Network layer
│   │   ├── client.c/h             # Client connection management
│   │   └── stream.c/h             # Send/Recv buffer handling
│   │
│   └── protocol/                   # Protocol layer
│       └── command.c/h            # Command parser & handler
│
├── .gitignore
└── README.md
```

---

## 🚀 Cài đặt và chạy

### Yêu cầu hệ thống
- **OS**: Linux/WSL Ubuntu 20.04+
- **Compiler**: GCC
- **Database**: MySQL 8.0+
- **Libraries**: 
  - `libmysqlclient-dev`
  - `libssl-dev` (OpenSSL)

### 1. Cài đặt dependencies

```bash
# Ubuntu/Debian nếu chưa cài đặt
sudo apt update
sudo apt install -y gcc make mysql-server mysql-client libmysqlclient-dev libssl-dev

# Start MySQL service
sudo service mysql start
```

### 2. Thiết lập database

```bash
# Login vào MySQL
sudo mysql -u root -p

# Tạo database và import schema
mysql> source server/database/schema.sql;

# (Optional) Import sample data
mysql> source server/database/seeder.sql;

# Tạo user cho ứng dụng (nếu cần)
mysql> CREATE USER 'app_user'@'localhost' IDENTIFIED BY 'your_password';
mysql> GRANT ALL PRIVILEGES ON file_sharing_system.* TO 'app_user'@'localhost';
mysql> FLUSH PRIVILEGES;
mysql> exit;
```

### 3. Cấu hình database connection

Sửa file `server/database/db.c`:

```c
conn = mysql_init(NULL);
if (!mysql_real_connect(conn,
                        "localhost",      // Host
                        "root",           // Username - thay đổi nếu cần
                        "your_password",  // Password - thay đổi
                        "file_sharing_system",  // Database name
                        0, NULL, 0))
{
    fprintf(stderr, "Connection error: %s\n", mysql_error(conn));
    return -1;
}
```

### 4. Compile project

```bash
cd server/
make clean
make
```

Output:
```
gcc -Wall -g -c main.c -o main.o
gcc -Wall -g -c database/db.c -o database/db.o
gcc -Wall -g -c auth/auth.c -o auth/auth.o
...
gcc main.o ... -o server -lmysqlclient -lssl -lcrypto
gcc client.o -o client
```

### 5. Chạy server

```bash
./server
```

Output:
```
Connected to MySQL database: file_sharing_system
Server listening on 0.0.0.0:1234
Using I/O Multiplexing with select()...
```

### 6. Chạy client (terminal khác)

```bash
./client
```

---

## 🎮 Sử dụng

### Menu Client

```
========== FILE SHARING CLIENT ==========
Trạng thái: ✗ Chưa đăng nhập
=========================================
1. Register (Đăng ký)
2. Login (Đăng nhập)
3. Exit (Thoát)
=========================================
Chọn chức năng: _
```

### Đăng ký tài khoản

```
Chọn: 1

--- ĐĂNG KÝ ---
Username: john
Password: ********  (ẩn khi nhập)

✓ Đăng ký thành công!
✓ Đã tự động đăng nhập!
```

### Đăng nhập

```
Chọn: 2

--- ĐĂNG NHẬP ---
Username: john
Password: ********

✓ Đăng nhập thành công!
```

### Menu sau khi đăng nhập

```
========== FILE SHARING CLIENT ==========
Trạng thái: ✓ Đã đăng nhập
=========================================
1. Logout (Đăng xuất)
2. Exit (Thoát)
=========================================
```

---

## 📡 Protocol Documentation

### Format
```
<COMMAND> <param1> <param2> ...\r\n
```

### Commands

#### 1. REGISTER
**Request:**
```
REGISTER <username> <password>\r\n
```

**Response:**
- `200 <token>\r\n` - Thành công, trả về token
- `409\r\n` - Username đã tồn tại
- `500\r\n` - Lỗi server
- `400\r\n` - Thiếu tham số

**Example:**
```
Client → Server: REGISTER john pass123\r\n
Server → Client: 200 a7f3d9e2b1c4f8a3e5d7c9b2a4f6e8d0\r\n
```

#### 2. LOGIN
**Request:**
```
LOGIN <username> <password>\r\n
```

**Response:**
- `200 <token>\r\n` - Thành công
- `404\r\n` - Username không tồn tại hoặc sai password
- `500\r\n` - Lỗi server
- `400\r\n` - Thiếu tham số

**Example:**
```
Client → Server: LOGIN john pass123\r\n
Server → Client: 200 x9y8z7w6v5u4t3s2r1q0p9o8n7m6l5k4\r\n
```

#### 3. VERIFY_TOKEN
**Request:**
```
VERIFY_TOKEN <token>\r\n
```

**Response:**
- `200\r\n` - Token hợp lệ
- `401\r\n` - Token không hợp lệ hoặc hết hạn
- `400\r\n` - Thiếu token

**Example:**
```
Client → Server: VERIFY_TOKEN x9y8z7w6v5u4t3s2r1q0p9o8n7m6l5k4\r\n
Server → Client: 200\r\n
```

#### 4. LOGOUT
**Request:**
```
LOGOUT <token>\r\n
```

**Response:**
- `200\r\n` - Đăng xuất thành công
- `500\r\n` - Lỗi khi xóa token
- `400\r\n` - Thiếu token

**Example:**
```
Client → Server: LOGOUT x9y8z7w6v5u4t3s2r1q0p9o8n7m6l5k4\r\n
Server → Client: 200\r\n
```

---

## 🔐 Security Features

### Password Hashing
- Sử dụng **SHA256** từ OpenSSL
- Password không bao giờ lưu plaintext
- Hash 64 ký tự hex

```c
// Input:  "pass123"
// Output: "ef92b778bafe771e89245b89ecbc08a44a4e166c06659911881f383d4473e94f"
```

### Token System
- Token random 32 ký tự (alphanumeric)
- Lưu trong database với thời hạn 24 giờ
- Tự động invalidate khi hết hạn
- Có thể logout để xóa token sớm

### Connection Security
- Password ẩn khi nhập (sử dụng `termios`)
- Token không hiển thị trong console
- Logs không chứa password
- Persistent connection giảm overhead

---

## 🗄️ Database Schema

### Tables

#### `users`
```sql
user_id      INT PRIMARY KEY AUTO_INCREMENT
username     VARCHAR(50) UNIQUE NOT NULL
password     VARCHAR(255) NOT NULL  -- SHA256 hash
created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP
```

#### `user_sessions`
```sql
session_id   INT PRIMARY KEY AUTO_INCREMENT
user_id      INT FOREIGN KEY → users(user_id)
token        VARCHAR(255) UNIQUE NOT NULL
created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP
expires_at   TIMESTAMP NOT NULL  -- created_at + 24 hours
```

#### `groups`
```sql
group_id     INT PRIMARY KEY AUTO_INCREMENT
group_name   VARCHAR(100) UNIQUE NOT NULL
description  TEXT
created_by   INT FOREIGN KEY → users(user_id)
root_dir_id  INT FOREIGN KEY → directories(dir_id)
created_at   TIMESTAMP
```

#### `user_groups`
```sql
user_id      INT FOREIGN KEY → users(user_id)
group_id     INT FOREIGN KEY → groups(group_id)
role         ENUM('member', 'admin')
joined_at    TIMESTAMP
PRIMARY KEY (user_id, group_id)
```

#### `files`
```sql
file_id      INT PRIMARY KEY AUTO_INCREMENT
file_name    VARCHAR(255) NOT NULL
file_path    VARCHAR(500) NOT NULL  -- Physical path
file_size    BIGINT NOT NULL
file_type    VARCHAR(100)
dir_id       INT FOREIGN KEY → directories(dir_id)
group_id     INT FOREIGN KEY → groups(group_id)
uploaded_by  INT FOREIGN KEY → users(user_id)
is_deleted   BOOLEAN DEFAULT FALSE
uploaded_at  TIMESTAMP
updated_at   TIMESTAMP
deleted_at   TIMESTAMP NULL
```

*(Xem đầy đủ schema trong `server/database/schema.sql`)*

---

## 🧪 Testing

### Manual Testing với Client

```bash
# Terminal 1: Start server
cd server/
./server

# Terminal 2: Start client
./client
```

**Test Cases:**

1. **Đăng ký thành công**
   - Input: Username mới + password
   - Expected: `200 <token>`, auto login

2. **Đăng ký username trùng**
   - Input: Username đã tồn tại
   - Expected: `409`, message "Username đã tồn tại"

3. **Đăng nhập thành công**
   - Input: Username + password đúng
   - Expected: `200 <token>`

4. **Đăng nhập sai password**
   - Input: Username đúng + password sai
   - Expected: `404`, message lỗi

5. **Token validation**
   - Menu tự động check token với server
   - Expected: Hiển thị trạng thái đúng

6. **Logout**
   - Click Logout
   - Expected: Token bị xóa, menu chuyển về "Chưa đăng nhập"

7. **Persistent connection**
   - Thực hiện nhiều request liên tiếp
   - Expected: Server log "New client" chỉ 1 lần

### Testing với netcat/telnet

```bash
# Connect
nc localhost 1234

# Test REGISTER
REGISTER testuser testpass
# Response: 200 <token>

# Test LOGIN
LOGIN testuser testpass
# Response: 200 <token>

# Test VERIFY_TOKEN
VERIFY_TOKEN <token>
# Response: 200

# Test LOGOUT
LOGOUT <token>
# Response: 200
```

---

## 📊 Performance

### Benchmarks (ước tính)
- **Latency per request**: ~10-50ms (local)
- **Concurrent connections**: 1000+ clients
- **Throughput**: ~1000 requests/second
- **Memory per client**: ~16KB (buffers)

### Tối ưu hóa
- Non-blocking I/O với select()
- Buffer pooling để giảm allocations
- Persistent connections giảm TCP handshake overhead
- Prepared statements cho database queries (TODO)

---

## 🐛 Troubleshooting

### Server không start được

**Error:** `Connection error: Access denied`
```bash
# Fix: Kiểm tra MySQL credentials trong db.c
sudo mysql -u root -p
# Test connection thủ công
```

**Error:** `Address already in use`
```bash
# Fix: Kill process cũ
pkill -9 server
# Hoặc đổi port trong main.c
```

### Client không kết nối được

```bash
# Check server đang chạy
ps aux | grep server

# Check port
netstat -tuln | grep 1234

# Check firewall
sudo ufw status
```

### Compilation errors

**Error:** `fatal error: mysql/mysql.h`
```bash
sudo apt install libmysqlclient-dev
```

**Error:** `fatal error: openssl/sha.h`
```bash
sudo apt install libssl-dev
```

---

## 🔮 Roadmap

### Phase 1: Core (✅ Hoàn thành)
- [x] Server architecture với I/O Multiplexing
- [x] Authentication system
- [x] Token-based session management
- [x] Client application

### Phase 2: Group Management (🚧 Đang phát triển)
- [ ] CREATE_GROUP command
- [ ] INVITE_USER command
- [ ] JOIN_GROUP command
- [ ] LEAVE_GROUP command
- [ ] LIST_GROUPS command

### Phase 3: File Operations
- [ ] UPLOAD_FILE command
- [ ] DOWNLOAD_FILE command
- [ ] DELETE_FILE command
- [ ] LIST_FILES command
- [ ] File chunking for large files

### Phase 4: Directory Management
- [ ] CREATE_DIR command
- [ ] DELETE_DIR command
- [ ] MOVE_FILE command
- [ ] RENAME command

### Phase 5: Advanced Features
- [ ] File versioning
- [ ] Permission system (read/write/admin)
- [ ] Activity logging UI
- [ ] Search functionality
- [ ] File sharing links

### Phase 6: Production Ready
- [ ] TLS/SSL encryption
- [ ] Rate limiting
- [ ] Password salt + bcrypt
- [ ] Connection pooling
- [ ] Load balancing
- [ ] Monitoring & logging

---

## 👥 Contributors

- **Ngọc Hưng** - Developer
- **IT4062** - Thực hành Lập trình mạng

---

## 📄 License

Dự án học tập - IT4062 Network Programming Course

---

## 📚 References

- [Beej's Guide to Network Programming](https://beej.us/guide/bgnet/)
- [MySQL C API Documentation](https://dev.mysql.com/doc/c-api/8.0/en/)
- [OpenSSL Documentation](https://www.openssl.org/docs/)
- [POSIX select() Manual](https://man7.org/linux/man-pages/man2/select.2.html)

---

## 📞 Contact

Nếu có câu hỏi hoặc vấn đề, vui lòng tạo issue trên GitHub repository.

**Happy Coding! 🚀**
