#ifndef TOKEN_H
#define TOKEN_H

#include <time.h>

#define TOKEN_LENGTH 32
#define TOKEN_EXPIRY_SECONDS 86400  // 24 hours

typedef struct {
    int user_id;
    char token[TOKEN_LENGTH + 1];
    time_t created_at;
    time_t expires_at;
} Session;

// Generate random token
void generate_token(char *token, int length);

// Save session to database
int save_session_to_db(int user_id, const char *token, char *error_msg, int error_size);

// Verify token and return user_id (0 if invalid)
int verify_token(const char *token, char *error_msg, int error_size);

// Delete expired sessions
void cleanup_expired_sessions();

#endif
