#include "logger.h"
#include <stdio.h>
#include <time.h>
#include <stdarg.h>

static const char* level_to_string(LogLevel level) {
    switch (level) {
        case LOG_RECV:  return "RECV";
        case LOG_SEND:  return "SEND";
        case LOG_INFO:  return "INFO";
        case LOG_ERROR: return "ERROR";
        case LOG_CONN:  return "CONN";
        case LOG_DISC:  return "DISC";
        default:        return "UNKNOWN";
    }
}

void log_message(int idx, int user_id, LogLevel level, const char *format, ...) {
    // Get current timestamp
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    // Print timestamp
    printf("[%04d-%02d-%02d %02d:%02d:%02d] ",
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec);
    
    // Print client info
    if (user_id > 0) {
        printf("[CLIENT:%d|USER:%d] ", idx, user_id);
    } else {
        printf("[CLIENT:%d] ", idx);
    }
    
    // Print log level
    printf("[%s] ", level_to_string(level));
    
    // Print message
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}

void log_recv(int idx, int user_id, const char *format, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    printf("[%04d-%02d-%02d %02d:%02d:%02d] ",
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec);
    
    if (user_id > 0) {
        printf("[CLIENT:%d|USER:%d] [RECV] ", idx, user_id);
    } else {
        printf("[CLIENT:%d] [RECV] ", idx);
    }
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}

void log_send(int idx, int user_id, const char *format, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    printf("[%04d-%02d-%02d %02d:%02d:%02d] ",
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec);
    
    if (user_id > 0) {
        printf("[CLIENT:%d|USER:%d] [SEND] ", idx, user_id);
    } else {
        printf("[CLIENT:%d] [SEND] ", idx);
    }
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}

void log_error(int idx, int user_id, const char *format, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    printf("[%04d-%02d-%02d %02d:%02d:%02d] ",
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec);
    
    if (user_id > 0) {
        printf("[CLIENT:%d|USER:%d] [ERROR] ", idx, user_id);
    } else {
        printf("[CLIENT:%d] [ERROR] ", idx);
    }
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}

void log_info(int idx, int user_id, const char *format, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    printf("[%04d-%02d-%02d %02d:%02d:%02d] ",
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec);
    
    if (user_id > 0) {
        printf("[CLIENT:%d|USER:%d] [INFO] ", idx, user_id);
    } else {
        printf("[CLIENT:%d] [INFO] ", idx);
    }
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}

void log_conn(int idx, const char *format, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    printf("[%04d-%02d-%02d %02d:%02d:%02d] [CLIENT:%d] [CONN] ",
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec, idx);
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}

void log_disc(int idx, const char *format, ...) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    printf("[%04d-%02d-%02d %02d:%02d:%02d] [CLIENT:%d] [DISC] ",
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
           t->tm_hour, t->tm_min, t->tm_sec, idx);
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
}
