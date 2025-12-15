# Server Logging System - Implementation Summary

## Files Created/Modified

### New Files:
1. **`server/utils/logger.h`** - Header file with logging function declarations
2. **`server/utils/logger.c`** - Implementation of logging functions with timestamp and user tracking

### Modified Files:
1. **`server/net/client.h`** - Added `user_id` field to Client struct
2. **`server/net/client.c`** - Initialize `user_id = 0` in all client operations
3. **`server/protocol/command.c`** - Added logging to all command handlers
4. **`server/io/io_multiplexing.c`** - Added connection/disconnection logging
5. **`server/Makefile`** - Added utils/logger.c to build

## Log Format

```
[YYYY-MM-DD HH:MM:SS] [CLIENT:idx|USER:user_id] [LEVEL] message
```

### Log Levels:
- `[RECV]` - Request received from client
- `[SEND]` - Response sent to client  
- `[INFO]` - General information
- `[ERROR]` - Error occurred
- `[CONN]` - New connection established
- `[DISC]` - Client disconnected

## Example Logs

### Connection:
```
[2025-12-02 14:23:45] [CLIENT:3] [CONN] New connection from 127.0.0.1:54321 (fd=8)
```

### Login Success:
```
[2025-12-02 14:23:50] [CLIENT:3] [RECV] LOGIN alice ***
[2025-12-02 14:23:50] [CLIENT:3|USER:5] [INFO] User authenticated: username=alice
[2025-12-02 14:23:50] [CLIENT:3|USER:5] [SEND] 200 eyJhbGciOiJIUzI1...
```

### Upload File:
```
[2025-12-02 14:30:00] [CLIENT:3|USER:5] [RECV] UPLOAD_FILE <token> 5 12 test.pdf 1 75 <base64>
[2025-12-02 14:30:00] [CLIENT:3|USER:5] [SEND] 202 1/75
[2025-12-02 14:30:01] [CLIENT:3|USER:5] [RECV] UPLOAD_FILE <token> 5 12 test.pdf 2 75 <base64>
[2025-12-02 14:30:01] [CLIENT:3|USER:5] [SEND] 202 2/75
...
[2025-12-02 14:30:45] [CLIENT:3|USER:5] [RECV] UPLOAD_FILE <token> 5 12 test.pdf 75 75 <base64>
[2025-12-02 14:30:45] [CLIENT:3|USER:5] [SEND] 200 75/75
```

### Error:
```
[2025-12-02 14:31:00] [CLIENT:4] [RECV] LOGIN bob wrongpass
[2025-12-02 14:31:00] [CLIENT:4] [ERROR] Invalid credentials
[2025-12-02 14:31:00] [CLIENT:4] [SEND] 404
```

### Disconnection:
```
[2025-12-02 14:32:00] [CLIENT:3] [DISC] Client disconnected (connection closed)
```

## Implementation Details

### User ID Tracking:
- `user_id = 0` → Not logged in
- `user_id > 0` → Logged in user (shows in log as `[CLIENT:idx|USER:user_id]`)
- Set after successful LOGIN or REGISTER
- Cleared on LOGOUT or disconnect

### Helper Functions:
```c
// Send response and auto-log
static void send_response(int idx, const char *response)

// Log functions
void log_recv(int idx, int user_id, const char *format, ...)
void log_send(int idx, int user_id, const char *format, ...)
void log_info(int idx, int user_id, const char *format, ...)
void log_error(int idx, int user_id, const char *format, ...)
void log_conn(int idx, const char *format, ...)
void log_disc(int idx, const char *format, ...)
```

## Build Instructions

```bash
cd server
make clean
make
./server
```

## Next Steps (Optional Enhancements)

1. **Log to file**: Redirect output to file with `./server > server.log 2>&1`
2. **Log rotation**: Implement daily log files with timestamps
3. **Log levels**: Add DEBUG level for more verbose logging
4. **Performance metrics**: Add request duration tracking
5. **Structured logs**: Output JSON format for log analysis tools

## Notes

- All existing `printf()` statements replaced with logging functions
- Password masking: LOGIN/REGISTER commands show `***` instead of actual password
- Token masking: Long tokens shown as `<token>` in logs for readability
- Automatic timestamp on every log entry
- Flushed immediately to stdout for real-time monitoring
