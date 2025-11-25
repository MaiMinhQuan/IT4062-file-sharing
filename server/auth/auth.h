#ifndef AUTH_H
#define AUTH_H

int handle_register(const char *username, const char *password,
                    char *response, int resp_size);

int handle_login(const char *username, const char *password,
                 char *response, int resp_size);

#endif
