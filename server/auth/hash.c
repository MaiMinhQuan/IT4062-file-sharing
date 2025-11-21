#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#include "hash.h"

void sha256_hash(const char *password, char *output, int output_size) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, password, strlen(password));
    SHA256_Final(hash, &sha256);

    // Convert to hex string
    int i;
    for (i = 0; i < SHA256_DIGEST_LENGTH && i * 2 + 1 < output_size; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[i * 2] = '\0';
}
