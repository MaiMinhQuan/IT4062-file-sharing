#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql/mysql.h>
#include "../database/db.h"
#include "../auth/token.h"

// Example: Verify token trước khi xử lý CREATE_GROUP
int handle_create_group(const char *token, const char *group_name,
                        char *response, int resp_size)
{
    char error_msg[256];
    
    // 1. Verify token
    int user_id = verify_token(token, error_msg, sizeof(error_msg));
    
    if (user_id <= 0) {
        // Token invalid hoặc expired
        snprintf(response, resp_size, "401");
        return -1;
    }
    
    // 2. Escape group_name
    char escaped_name[256];
    mysql_real_escape_string(conn, escaped_name, group_name, strlen(group_name));
    
    // 3. Check if group already exists
    char query[512];
    snprintf(query, sizeof(query),
             "SELECT group_id FROM `groups` WHERE group_name='%s'",
             escaped_name);
    
    if (mysql_query(conn, query) != 0) {
        snprintf(response, resp_size, "500");
        return -1;
    }
    
    MYSQL_RES *res = mysql_store_result(conn);
    if (mysql_num_rows(res) > 0) {
        mysql_free_result(res);
        snprintf(response, resp_size, "409");  // Group already exists
        return 0;
    }
    mysql_free_result(res);
    
    // 4. Create group
    snprintf(query, sizeof(query),
             "INSERT INTO `groups` (group_name, created_by) VALUES ('%s', %d)",
             escaped_name, user_id);
    
    if (mysql_query(conn, query) != 0) {
        snprintf(response, resp_size, "500");
        return -1;
    }
    
    int group_id = (int)mysql_insert_id(conn);
    
    // 5. Add creator as admin
    snprintf(query, sizeof(query),
             "INSERT INTO user_groups (user_id, group_id, role) "
             "VALUES (%d, %d, 'admin')",
             user_id, group_id);
    
    if (mysql_query(conn, query) != 0) {
        snprintf(response, resp_size, "500");
        return -1;
    }
    
    // 6. Success
    snprintf(response, resp_size, "200 %d", group_id);
    return group_id;
}

// Example: LIST_GROUPS - danh sách groups mà user là member
int handle_list_groups(const char *token, char *response, int resp_size)
{
    char error_msg[256];
    
    // 1. Verify token
    int user_id = verify_token(token, error_msg, sizeof(error_msg));
    
    if (user_id <= 0) {
        snprintf(response, resp_size, "401");
        return -1;
    }
    
    // 2. Get groups
    char query[512];
    snprintf(query, sizeof(query),
             "SELECT g.group_id, g.group_name, ug.role "
             "FROM `groups` g "
             "JOIN user_groups ug ON g.group_id = ug.group_id "
             "WHERE ug.user_id = %d",
             user_id);
    
    if (mysql_query(conn, query) != 0) {
        snprintf(response, resp_size, "500");
        return -1;
    }
    
    MYSQL_RES *res = mysql_store_result(conn);
    int num_rows = mysql_num_rows(res);
    
    if (num_rows == 0) {
        mysql_free_result(res);
        snprintf(response, resp_size, "200 0");
        return 0;
    }
    
    // 3. Build response: "200 <count> <group_id1>:<name1>:<role1> ..."
    char *ptr = response;
    int remaining = resp_size;
    int written = snprintf(ptr, remaining, "200 %d", num_rows);
    ptr += written;
    remaining -= written;
    
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) && remaining > 0) {
        written = snprintf(ptr, remaining, " %s:%s:%s",
                          row[0], row[1], row[2]);
        ptr += written;
        remaining -= written;
    }
    
    mysql_free_result(res);
    return num_rows;
}

// Example: UPLOAD_FILE - verify token và quyền truy cập group
int handle_upload_file(const char *token, int group_id, 
                       const char *filename, long filesize,
                       char *response, int resp_size)
{
    char error_msg[256];
    
    // 1. Verify token
    int user_id = verify_token(token, error_msg, sizeof(error_msg));
    
    if (user_id <= 0) {
        snprintf(response, resp_size, "401");
        return -1;
    }
    
    // 2. Check if user is member of group
    char query[512];
    snprintf(query, sizeof(query),
             "SELECT role FROM user_groups "
             "WHERE user_id=%d AND group_id=%d",
             user_id, group_id);
    
    if (mysql_query(conn, query) != 0) {
        snprintf(response, resp_size, "500");
        return -1;
    }
    
    MYSQL_RES *res = mysql_store_result(conn);
    MYSQL_ROW row = mysql_fetch_row(res);
    
    if (!row) {
        mysql_free_result(res);
        snprintf(response, resp_size, "403");  // Not a member
        return 0;
    }
    mysql_free_result(res);
    
    // 3. TODO: Handle actual file upload
    // This is just example showing auth flow
    
    snprintf(response, resp_size, "200 READY_FOR_UPLOAD");
    return 1;
}

/*
 * USAGE trong command.c:
 * 
 * if (strcasecmp(cmd, "CREATE_GROUP") == 0) {
 *     char *token = strtok(NULL, " \r\n");
 *     char *group_name = strtok(NULL, " \r\n");
 *     
 *     if (!token || !group_name) {
 *         snprintf(response, sizeof(response), "400\r\n");
 *         enqueue_send(idx, response, strlen(response));
 *         return;
 *     }
 *     
 *     char resp[256];
 *     handle_create_group(token, group_name, resp, sizeof(resp));
 *     snprintf(response, sizeof(response), "%s\r\n", resp);
 *     enqueue_send(idx, response, strlen(response));
 * }
 */
