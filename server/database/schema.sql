-- Tạo database
CREATE DATABASE IF NOT EXISTS file_sharing_system;
USE file_sharing_system;

-- Bảng Users
CREATE TABLE IF NOT EXISTS users (
    user_id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(255) NOT NULL,  -- Lưu hash password
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Bảng Sessions/Tokens
CREATE TABLE IF NOT EXISTS user_sessions (
    session_id INT AUTO_INCREMENT PRIMARY KEY,
    user_id INT NOT NULL,
    token VARCHAR(255) NOT NULL UNIQUE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    expires_at TIMESTAMP NOT NULL,
    FOREIGN KEY (user_id) REFERENCES users(user_id)
);

-- Bảng Groups
CREATE TABLE IF NOT EXISTS `groups` (
    group_id INT AUTO_INCREMENT PRIMARY KEY,
    group_name VARCHAR(100) NOT NULL UNIQUE,
    description TEXT,
    created_by INT NOT NULL,       -- ID người tạo nhóm
    root_dir_id INT DEFAULT NULL,  -- ID thư mục gốc của nhóm
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (created_by) REFERENCES users(user_id)
);

-- Bảng User_Groups
CREATE TABLE IF NOT EXISTS user_groups (
    user_id INT NOT NULL,
    group_id INT NOT NULL,
    role ENUM('member', 'admin') NOT NULL DEFAULT 'member',
    joined_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id, group_id),
    FOREIGN KEY (user_id) REFERENCES users(user_id),
    FOREIGN KEY (group_id) REFERENCES `groups`(group_id)
);

-- Bảng lưu trữ lời mời và yêu cầu gia nhập nhóm
CREATE TABLE IF NOT EXISTS group_requests (
    request_id INT AUTO_INCREMENT PRIMARY KEY,
    user_id INT NOT NULL,  -- Người gửi yêu cầu hoặc nhận lời mời
    group_id INT NOT NULL,  -- Nhóm mà yêu cầu hoặc lời mời hướng đến
    request_type ENUM('join_request', 'invitation') NOT NULL,
    status ENUM('pending', 'accepted', 'rejected') NOT NULL DEFAULT 'pending',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(user_id),
    FOREIGN KEY (group_id) REFERENCES `groups`(group_id)
);

-- Bảng Directories
CREATE TABLE IF NOT EXISTS directories (
    dir_id INT AUTO_INCREMENT PRIMARY KEY,
    dir_name VARCHAR(100) NOT NULL,
    parent_dir_id INT DEFAULT NULL,  -- ID thư mục cha - cho phép cấu trúc cây
                                     -- NULL = root directory
    group_id INT NOT NULL,
    created_by INT NOT NULL,
    is_deleted BOOLEAN DEFAULT FALSE,  -- Soft delete
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    deleted_at TIMESTAMP NULL,
    FOREIGN KEY (parent_dir_id) REFERENCES directories(dir_id),
    FOREIGN KEY (group_id) REFERENCES `groups`(group_id),
    FOREIGN KEY (created_by) REFERENCES users(user_id)
);

-- Bảng Files
CREATE TABLE IF NOT EXISTS files (
    file_id INT AUTO_INCREMENT PRIMARY KEY,
    file_name VARCHAR(255) NOT NULL,
    file_path VARCHAR(500) NOT NULL,  -- Đường dẫn vật lý của file trên server
    file_size BIGINT NOT NULL,
    file_type VARCHAR(100),             -- Loại file (ví dụ: 'image/png', 'application/pdf')
    dir_id INT NOT NULL,
    group_id INT NOT NULL,
    uploaded_by INT NOT NULL,           -- ID người upload file
    is_deleted BOOLEAN DEFAULT FALSE,  -- Soft delete
    uploaded_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    deleted_at TIMESTAMP NULL,
    FOREIGN KEY (dir_id) REFERENCES directories(dir_id),
    FOREIGN KEY (group_id) REFERENCES `groups`(group_id),
    FOREIGN KEY (uploaded_by) REFERENCES users(user_id)
);

-- Bảng log hoạt động của nhóm
CREATE TABLE IF NOT EXISTS activity_log (
    log_id INT AUTO_INCREMENT PRIMARY KEY,
    user_id INT NOT NULL,
    description VARCHAR(100) NOT NULL, -- Mô tả hành động (ví dụ: 'upload_file', 'create_directory')
    group_id INT NOT NULL,  -- Lưu group_id của nhóm mà người dùng đã thực hiện hành động
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(user_id),
    FOREIGN KEY (group_id) REFERENCES `groups`(group_id)
);

-- ============================================
-- Stored Procedures
-- ============================================

DELIMITER $$

DROP PROCEDURE IF EXISTS create_group$$
CREATE PROCEDURE create_group(
    IN p_group_name VARCHAR(100),
    IN p_description TEXT,
    IN p_user_id INT
)
BEGIN
    DECLARE new_group_id INT;
    DECLARE new_dir_id INT;

    INSERT INTO `groups` (group_name, `description`, created_by, root_dir_id, created_at)
    VALUES (p_group_name, p_description, p_user_id, NULL, NOW());

    SET new_group_id = LAST_INSERT_ID();

    INSERT INTO directories (dir_name, parent_dir_id, group_id, created_by, created_at)
    VALUES ('Root', NULL, new_group_id, p_user_id, NOW());

    SET new_dir_id = LAST_INSERT_ID();

    UPDATE `groups`
    SET root_dir_id = new_dir_id
    WHERE group_id = new_group_id;

    INSERT INTO user_groups (user_id, group_id, role, joined_at)
    VALUES (p_user_id, new_group_id, 'admin', NOW());

    SELECT new_group_id AS group_id;
END$$

DROP PROCEDURE IF EXISTS get_user_groups$$
CREATE PROCEDURE get_user_groups(
    IN p_user_id INT
)
BEGIN
    SELECT 
        g.group_id,
        g.group_name,
        COALESCE(g.description, '') AS description,
        ug.role,
        g.created_at
    FROM `groups` g
    JOIN user_groups ug ON g.group_id = ug.group_id
    WHERE ug.user_id = p_user_id
    ORDER BY g.created_at DESC;
END$$

DELIMITER ;