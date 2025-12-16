#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>

// Log levels
typedef enum {
    LOG_RECV,   // Received request
    LOG_SEND,   // Sent response
    LOG_INFO,   // General info
    LOG_ERROR,  // Error
    LOG_CONN,   // Connection event
    LOG_DISC    // Disconnection event
} LogLevel;

// Log a message with timestamp, client index, and optional user_id
void log_message(int idx, int user_id, LogLevel level, const char *format, ...);

// Log request received from client
void log_recv(int idx, int user_id, const char *format, ...);

// Log response sent to client
void log_send(int idx, int user_id, const char *format, ...);

// Log error
void log_error(int idx, int user_id, const char *format, ...);

// Log info
void log_info(int idx, int user_id, const char *format, ...);

// Log connection events
void log_conn(int idx, const char *format, ...);

// Log disconnection events
void log_disc(int idx, const char *format, ...);

#endif
