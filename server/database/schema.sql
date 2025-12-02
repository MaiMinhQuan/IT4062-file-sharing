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

DROP PROCEDURE IF EXISTS request_join_group$$
CREATE PROCEDURE request_join_group(
    IN p_user_id INT,
    IN p_group_id INT,
    OUT result_code INT
)
request_join_group: BEGIN
    DECLARE group_exists INT DEFAULT 0;
    DECLARE already_member INT DEFAULT 0;
    DECLARE pending_request INT DEFAULT 0;

    -- Kiểm tra nhóm có tồn tại không
    SELECT COUNT(*) INTO group_exists
    FROM `groups`
    WHERE group_id = p_group_id;

    IF group_exists = 0 THEN
        SET result_code = 404; -- Nhóm không tồn tại
        LEAVE request_join_group;
    END IF;

    -- Kiểm tra đã là thành viên chưa
    SELECT COUNT(*) INTO already_member
    FROM user_groups
    WHERE user_id = p_user_id AND group_id = p_group_id;

    IF already_member > 0 THEN
        SET result_code = 409; -- Đã là thành viên của nhóm
        LEAVE request_join_group;
    END IF;

    -- Kiểm tra đã gửi yêu cầu trước đó chưa
    SELECT COUNT(*) INTO pending_request
    FROM group_requests
    WHERE user_id = p_user_id
      AND group_id = p_group_id
      AND request_type = 'join_request'
      AND status = 'pending';

    IF pending_request > 0 THEN
        SET result_code = 423; -- Đã gửi yêu cầu trước đó
        LEAVE request_join_group;
    END IF;

    -- Tạo yêu cầu mới
    INSERT INTO group_requests (user_id, group_id, request_type, status, created_at)
    VALUES (p_user_id, p_group_id, 'join_request', 'pending', NOW());

    SET result_code = 200; -- Gửi yêu cầu thành công
END$$

DROP PROCEDURE IF EXISTS check_admin$$
CREATE PROCEDURE check_admin(
    IN p_user_id INT,
    IN p_group_id INT,
    OUT result_code INT
)
check_admin: BEGIN
    DECLARE group_exists INT DEFAULT 0;
    DECLARE is_admin INT DEFAULT 0;
    DECLARE is_member INT DEFAULT 0;

    -- Kiểm tra nhóm có tồn tại không
    SELECT COUNT(*) INTO group_exists
    FROM `groups`
    WHERE group_id = p_group_id;

    IF group_exists = 0 THEN
        SET result_code = 404; -- Nhóm không tồn tại
        LEAVE check_admin;
    END IF;

    -- Kiểm tra user có phải là thành viên không
    SELECT COUNT(*) INTO is_member
    FROM user_groups
    WHERE user_id = p_user_id AND group_id = p_group_id;

    IF is_member = 0 THEN
        SET result_code = 403; -- User chỉ là thành viên (không phải admin)
        LEAVE check_admin;
    END IF;

    -- Kiểm tra user có phải là admin không
    SELECT COUNT(*) INTO is_admin
    FROM user_groups
    WHERE user_id = p_user_id AND group_id = p_group_id AND role = 'admin';

    IF is_admin > 0 THEN
        SET result_code = 200; -- User là admin của nhóm
    ELSE
        SET result_code = 403; -- User chỉ là thành viên (không phải admin)
    END IF;
END$$

DROP PROCEDURE IF EXISTS handle_join_request$$
CREATE PROCEDURE handle_join_request(
    IN p_admin_user_id INT,
    IN p_request_id INT,
    IN p_option VARCHAR(10),
    OUT result_code INT
)
handle_join_request: BEGIN
    DECLARE req_user_id INT DEFAULT 0;
    DECLARE req_group_id INT DEFAULT 0;
    DECLARE req_status VARCHAR(20);
    DECLARE is_admin INT DEFAULT 0;
    DECLARE request_exists INT DEFAULT 0;

    -- Kiểm tra yêu cầu có tồn tại không
    SELECT COUNT(*) INTO request_exists
    FROM group_requests
    WHERE request_id = p_request_id AND request_type = 'join_request';

    IF request_exists = 0 THEN
        SET result_code = 404; -- Nhóm không tồn tại
        LEAVE handle_join_request;
    END IF;

    -- Lấy thông tin yêu cầu
    SELECT user_id, group_id, status
    INTO req_user_id, req_group_id, req_status
    FROM group_requests
    WHERE request_id = p_request_id AND request_type = 'join_request';

    -- Kiểm tra người xử lý có quyền admin không
    SELECT COUNT(*) INTO is_admin
    FROM user_groups
    WHERE user_id = p_admin_user_id AND group_id = req_group_id AND role = 'admin';

    IF is_admin = 0 THEN
        SET result_code = 403; -- Người dùng không có quyền xét duyệt
        LEAVE handle_join_request;
    END IF;

    -- Kiểm tra yêu cầu đã là thành viên chưa
    IF req_status != 'pending' THEN
        SET result_code = 409; -- Đã là thành viên của nhóm
        LEAVE handle_join_request;
    END IF;

    -- Xử lý theo option
    IF p_option = 'accepted' THEN
        -- Cập nhật trạng thái yêu cầu
        UPDATE group_requests
        SET status = 'accepted', updated_at = NOW()
        WHERE request_id = p_request_id;

        -- Thêm user vào nhóm
        INSERT INTO user_groups (user_id, group_id, role, joined_at)
        VALUES (req_user_id, req_group_id, 'member', NOW());

        SET result_code = 200; -- Xử lý yêu cầu thành công
    ELSEIF p_option = 'rejected' THEN
        -- Cập nhật trạng thái yêu cầu
        UPDATE group_requests
        SET status = 'rejected', updated_at = NOW()
        WHERE request_id = p_request_id;

        SET result_code = 200; -- Xử lý yêu cầu thành công
    ELSE
        SET result_code = 400; -- Option không hợp lệ
    END IF;
END$$

DROP PROCEDURE IF EXISTS get_groups_not_joined$$
CREATE PROCEDURE get_groups_not_joined(
    IN p_user_id INT
)
BEGIN
    SELECT
        g.group_id,
        g.group_name,
        COALESCE(g.description, '') AS description,
        u.username AS admin_name,
        g.created_at
    FROM `groups` g
    JOIN users u ON g.created_by = u.user_id
    WHERE g.group_id NOT IN (
        SELECT group_id FROM user_groups WHERE user_id = p_user_id
    )
    ORDER BY g.created_at DESC;
END$$

DROP PROCEDURE IF EXISTS get_pending_requests_for_admin$$
CREATE PROCEDURE get_pending_requests_for_admin(
    IN p_admin_user_id INT
)
BEGIN
    SELECT
        gr.request_id,
        gr.user_id,
        u.username,
        gr.group_id,
        g.group_name,
        gr.created_at
    FROM group_requests gr
    JOIN users u ON gr.user_id = u.user_id
    JOIN `groups` g ON gr.group_id = g.group_id
    WHERE gr.request_type = 'join_request'
      AND gr.status = 'pending'
      AND gr.group_id IN (
          SELECT group_id FROM user_groups WHERE user_id = p_admin_user_id AND role = 'admin'
      )
    ORDER BY g.group_id, gr.created_at ASC;
END$$

DROP FUNCTION IF EXISTS get_file_name_by_id$$
CREATE FUNCTION get_file_name_by_id(p_file_id INT)
RETURNS VARCHAR(255)
DETERMINISTIC
BEGIN
    DECLARE result VARCHAR(255);

    SELECT file_name INTO result
    FROM files
    WHERE file_id = p_file_id
    LIMIT 1;

    RETURN result;  -- Nếu không có thì trả NULL
END$$

DELIMITER ;