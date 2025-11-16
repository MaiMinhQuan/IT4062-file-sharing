-- Seeder cho hệ thống chia sẻ file
-- File này chứa dữ liệu mẫu để test và phát triển

USE file_sharing_system;

-- Xóa dữ liệu cũ (theo thứ tự để tránh lỗi foreign key)
SET FOREIGN_KEY_CHECKS = 0;
TRUNCATE TABLE activity_log;
TRUNCATE TABLE group_requests;
TRUNCATE TABLE user_sessions;
TRUNCATE TABLE files;
TRUNCATE TABLE directories;
TRUNCATE TABLE user_groups;
TRUNCATE TABLE `groups`;
TRUNCATE TABLE users;
SET FOREIGN_KEY_CHECKS = 1;

-- ============================================
-- 1. THÊM USERS (Người dùng)
-- ============================================
-- Password được hash bằng bcrypt (password gốc: "password123")
-- Trong thực tế, bạn cần hash password trước khi insert
INSERT INTO users (user_id, username, password) VALUES
(1, 'admin', '$2a$10$XQjz3qXKxJ5Y5qXKxJ5Y5e5Y5qXKxJ5Y5qXKxJ5Y5qXKxJ5Y5qXKx'),  -- admin user
(2, 'john_doe', '$2a$10$XQjz3qXKxJ5Y5qXKxJ5Y5e5Y5qXKxJ5Y5qXKxJ5Y5qXKxJ5Y5qXKx'),
(3, 'jane_smith', '$2a$10$XQjz3qXKxJ5Y5qXKxJ5Y5e5Y5qXKxJ5Y5qXKxJ5Y5qXKxJ5Y5qXKx'),
(4, 'bob_wilson', '$2a$10$XQjz3qXKxJ5Y5qXKxJ5Y5e5Y5qXKxJ5Y5qXKxJ5Y5qXKxJ5Y5qXKx'),
(5, 'alice_chen', '$2a$10$XQjz3qXKxJ5Y5qXKxJ5Y5e5Y5qXKxJ5Y5qXKxJ5Y5qXKxJ5Y5qXKx'),
(6, 'mike_brown', '$2a$10$XQjz3qXKxJ5Y5qXKxJ5Y5e5Y5qXKxJ5Y5qXKxJ5Y5qXKxJ5Y5qXKx');

-- ============================================
-- 2. THÊM GROUPS (Nhóm chia sẻ)
-- ============================================
-- Tạo các nhóm khác nhau cho mục đích khác nhau
INSERT INTO `groups` (group_id, group_name, description, created_by) VALUES
(1, 'Project Alpha', 'Nhóm dự án Alpha - Chia sẻ tài liệu dự án', 1),
(2, 'Marketing Team', 'Nhóm Marketing - Chia sẻ tài liệu quảng cáo, hình ảnh', 2),
(3, 'Development Team', 'Nhóm phát triển - Code và tài liệu kỹ thuật', 3),
(4, 'Study Group IT4062', 'Nhóm học tập môn IT4062 - Tài liệu học tập', 4),
(5, 'Company Resources', 'Tài nguyên công ty - Tài liệu chung cho toàn công ty', 1);

-- ============================================
-- 3. THÊM USER_GROUPS (Thành viên nhóm)
-- ============================================
-- Gán users vào các `groups` với roles khác nhau
INSERT INTO user_groups (user_id, group_id, role) VALUES
-- Project Alpha (Group 1)
(1, 1, 'admin'),   -- admin là admin của nhóm
(2, 1, 'member'),  -- john_doe là member
(3, 1, 'member'),  -- jane_smith là member

-- Marketing Team (Group 2)
(2, 2, 'admin'),   -- john_doe là admin
(4, 2, 'member'),  -- bob_wilson là member
(5, 2, 'member'),  -- alice_chen là member

-- Development Team (Group 3)
(3, 3, 'admin'),   -- jane_smith là admin
(1, 3, 'member'),  -- admin là member
(6, 3, 'member'),  -- mike_brown là member

-- Study Group IT4062 (Group 4)
(4, 4, 'admin'),   -- bob_wilson là admin
(5, 4, 'member'),  -- alice_chen là member
(6, 4, 'member'),  -- mike_brown là member

-- Company Resources (Group 5)
(1, 5, 'admin'),   -- admin là admin
(2, 5, 'member'),  -- tất cả members
(3, 5, 'member'),
(4, 5, 'member'),
(5, 5, 'member'),
(6, 5, 'member');

-- ============================================
-- 4. THÊM DIRECTORIES (Thư mục)
-- ============================================
-- Tạo cấu trúc thư mục phân cấp cho mỗi nhóm

-- Group 1: Project Alpha
INSERT INTO directories (dir_id, dir_name, parent_dir_id, group_id, created_by) VALUES
(1, 'Root', NULL, 1, 1),                    -- Thư mục gốc
(2, 'Documents', 1, 1, 1),                  -- Thư mục con
(3, 'Images', 1, 1, 2),
(4, 'Proposals', 2, 1, 1),                  -- Thư mục con cấp 2
(5, 'Reports', 2, 1, 3);

-- Group 2: Marketing Team
INSERT INTO directories (dir_id, dir_name, parent_dir_id, group_id, created_by) VALUES
(6, 'Root', NULL, 2, 2),
(7, 'Campaigns', 6, 2, 2),
(8, 'Design Assets', 6, 2, 4),
(9, '2024 Q1', 7, 2, 2),
(10, 'Social Media', 8, 2, 5);

-- Group 3: Development Team
INSERT INTO directories (dir_id, dir_name, parent_dir_id, group_id, created_by) VALUES
(11, 'Root', NULL, 3, 3),
(12, 'Source Code', 11, 3, 3),
(13, 'Documentation', 11, 3, 1),
(14, 'Backend', 12, 3, 6),
(15, 'Frontend', 12, 3, 3);

-- Group 4: Study Group IT4062
INSERT INTO directories (dir_id, dir_name, parent_dir_id, group_id, created_by) VALUES
(16, 'Root', NULL, 4, 4),
(17, 'Lectures', 16, 4, 4),
(18, 'Assignments', 16, 4, 5),
(19, 'Projects', 16, 4, 6);

-- Group 5: Company Resources
INSERT INTO directories (dir_id, dir_name, parent_dir_id, group_id, created_by) VALUES
(20, 'Root', NULL, 5, 1),
(21, 'HR Documents', 20, 5, 1),
(22, 'Templates', 20, 5, 2);

-- Cập nhật root_dir_id cho các `groups`
UPDATE `groups` SET root_dir_id = 1 WHERE group_id = 1;
UPDATE `groups` SET root_dir_id = 6 WHERE group_id = 2;
UPDATE `groups` SET root_dir_id = 11 WHERE group_id = 3;
UPDATE `groups` SET root_dir_id = 16 WHERE group_id = 4;
UPDATE `groups` SET root_dir_id = 20 WHERE group_id = 5;

-- ============================================
-- 5. THÊM FILES (File)
-- ============================================
-- Thêm các file mẫu vào các thư mục
-- Lưu ý: file_path là đường dẫn vật lý trên server

INSERT INTO files (file_name, file_path, file_size, dir_id, group_id, uploaded_by) VALUES
-- Files trong Project Alpha
('project_proposal.pdf', '/uploads/group1/project_proposal.pdf', 2048576, 4, 1, 1),
('budget_report.xlsx', '/uploads/group1/budget_report.xlsx', 512000, 5, 1, 3),
('team_photo.jpg', '/uploads/group1/team_photo.jpg', 1024000, 3, 1, 2),
('meeting_notes.docx', '/uploads/group1/meeting_notes.docx', 256000, 2, 1, 1),

-- Files trong Marketing Team
('campaign_brief.pdf', '/uploads/group2/campaign_brief.pdf', 1536000, 9, 2, 2),
('logo_design.png', '/uploads/group2/logo_design.png', 2048000, 10, 2, 5),
('banner_template.psd', '/uploads/group2/banner_template.psd', 15360000, 8, 2, 4),

-- Files trong Development Team
('api_documentation.md', '/uploads/group3/api_documentation.md', 128000, 13, 3, 1),
('server_config.json', '/uploads/group3/server_config.json', 64000, 14, 3, 6),
('main_app.js', '/uploads/group3/main_app.js', 256000, 15, 3, 3),
('database_schema.sql', '/uploads/group3/database_schema.sql', 128000, 13, 3, 3),

-- Files trong Study Group IT4062
('lecture_week1.pdf', '/uploads/group4/lecture_week1.pdf', 3072000, 17, 4, 4),
('assignment1.pdf', '/uploads/group4/assignment1.pdf', 512000, 18, 4, 4),
('project_guidelines.docx', '/uploads/group4/project_guidelines.docx', 768000, 19, 4, 5),

-- Files trong Company Resources
('employee_handbook.pdf', '/uploads/group5/employee_handbook.pdf', 4096000, 21, 5, 1),
('invoice_template.xlsx', '/uploads/group5/invoice_template.xlsx', 256000, 22, 5, 2);

-- ============================================
-- 6. THÊM USER_SESSIONS (Phiên đăng nhập)
-- ============================================
-- Tạo các session tokens cho users đang active
-- Token được generate ngẫu nhiên (trong thực tế dùng UUID hoặc JWT)
INSERT INTO user_sessions (user_id, token, expires_at) VALUES
(1, 'token_admin_abc123xyz789', DATE_ADD(NOW(), INTERVAL 7 DAY)),
(2, 'token_john_def456uvw012', DATE_ADD(NOW(), INTERVAL 7 DAY)),
(3, 'token_jane_ghi789rst345', DATE_ADD(NOW(), INTERVAL 7 DAY)),
(4, 'token_bob_jkl012mno678', DATE_ADD(NOW(), INTERVAL 7 DAY));

-- ============================================
-- 7. THÊM ACTIVITY_LOG (Nhật ký hoạt động)
-- ============================================
-- Ghi lại các hoạt động đã diễn ra trong hệ thống
INSERT INTO activity_log (user_id, description, group_id) VALUES
-- Hoạt động trong Project Alpha
(1, 'create_group', 1),
(1, 'create_directory', 1),
(1, 'upload_file', 1),
(2, 'join_group', 1),
(3, 'upload_file', 1),

-- Hoạt động trong Marketing Team
(2, 'create_group', 2),
(2, 'upload_file', 2),
(4, 'join_group', 2),
(5, 'upload_file', 2),

-- Hoạt động trong Development Team
(3, 'create_group', 3),
(3, 'upload_file', 3),
(6, 'join_group', 3),
(1, 'download_file', 3),

-- Hoạt động trong Study Group
(4, 'create_group', 4),
(4, 'upload_file', 4),
(5, 'join_group', 4),
(6, 'download_file', 4);

-- ============================================
-- 8. THÊM GROUP_REQUESTS (Yêu cầu gia nhập)
-- ============================================
-- Tạo một số yêu cầu gia nhập và lời mời
INSERT INTO group_requests (user_id, group_id, request_type, status) VALUES
-- Yêu cầu đang chờ xử lý
(6, 1, 'join_request', 'pending'),        -- mike_brown muốn vào Project Alpha
(5, 3, 'join_request', 'pending'),        -- alice_chen muốn vào Development Team

-- Lời mời đang chờ
(4, 3, 'invitation', 'pending'),          -- Mời bob_wilson vào Development Team

-- Yêu cầu đã được chấp nhận
(2, 1, 'join_request', 'accepted'),       -- john_doe đã được chấp nhận vào Project Alpha
(4, 2, 'invitation', 'accepted'),         -- bob_wilson đã chấp nhận lời mời vào Marketing

-- Yêu cầu đã bị từ chối
(3, 2, 'join_request', 'rejected');       -- jane_smith bị từ chối vào Marketing Team

-- ============================================
-- KIỂM TRA DỮ LIỆU ĐÃ THÊM
-- ============================================
SELECT 'Users count:' as info, COUNT(*) as count FROM users;
SELECT 'Groups count:' as info, COUNT(*) as count FROM `groups`;
SELECT 'Directories count:' as info, COUNT(*) as count FROM directories;
SELECT 'Files count:' as info, COUNT(*) as count FROM files;
SELECT 'Activity logs count:' as info, COUNT(*) as count FROM activity_log;
SELECT 'Group requests count:' as info, COUNT(*) as count FROM group_requests;

-- ============================================
-- QUERIES MẪU ĐỂ TEST
-- ============================================

-- Xem tất cả nhóm và số lượng thành viên
SELECT
    g.group_name,
    g.description,
    COUNT(ug.user_id) as member_count,
    u.username as created_by
FROM `groups` g
LEFT JOIN user_groups ug ON g.group_id = ug.group_id
JOIN users u ON g.created_by = u.user_id
GROUP BY g.group_id;

-- Xem cấu trúc thư mục của một nhóm (ví dụ: group_id = 1)
SELECT
    d.dir_id,
    d.dir_name,
    d.parent_dir_id,
    u.username as created_by,
    COUNT(f.file_id) as file_count
FROM directories d
LEFT JOIN files f ON d.dir_id = f.dir_id
JOIN users u ON d.created_by = u.user_id
WHERE d.group_id = 1
GROUP BY d.dir_id
ORDER BY d.parent_dir_id, d.dir_id;

-- Xem tất cả files trong một nhóm
SELECT
    f.file_name,
    f.file_size,
    d.dir_name as directory,
    u.username as uploaded_by,
    f.uploaded_at
FROM files f
JOIN directories d ON f.dir_id = d.dir_id
JOIN users u ON f.uploaded_by = u.user_id
WHERE f.group_id = 1
ORDER BY f.uploaded_at DESC;
