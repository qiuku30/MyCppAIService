-- Run in the selected application database (DB_NAME / MYSQL_DATABASE).
-- Existing databases require the explicit migrations; do not recreate their tables.

CREATE TABLE IF NOT EXISTS users (
    id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(128) NOT NULL UNIQUE,
    password VARCHAR(255) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS chat_message (
    message_id VARCHAR(64) CHARACTER SET ascii NULL,
    id BIGINT NOT NULL COMMENT '用户ID',
    username VARCHAR(128) NOT NULL COMMENT '用户名',
    session_id VARCHAR(64) NOT NULL COMMENT '会话ID',
    is_user TINYINT NOT NULL DEFAULT 1 COMMENT '1=用户消息 0=AI回复',
    content TEXT NOT NULL COMMENT '消息内容',
    ts BIGINT NOT NULL COMMENT 'Unix毫秒时间戳',
    UNIQUE KEY uq_chat_message_id (message_id),
    INDEX idx_id_session (id, session_id),
    INDEX idx_ts (ts)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
