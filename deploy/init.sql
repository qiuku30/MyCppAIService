-- AI 聊天服务平台数据库初始化
CREATE DATABASE IF NOT EXISTS ChatHttpServer;
USE ChatHttpServer;

CREATE TABLE IF NOT EXISTS chat_message (
    id BIGINT NOT NULL COMMENT '用户ID',
    username VARCHAR(128) NOT NULL COMMENT '用户名',
    session_id VARCHAR(64) NOT NULL COMMENT '会话ID',
    is_user TINYINT NOT NULL DEFAULT 1 COMMENT '1=用户消息 0=AI回复',
    content TEXT NOT NULL COMMENT '消息内容',
    ts BIGINT NOT NULL COMMENT 'Unix毫秒时间戳',
    INDEX idx_id_session (id, session_id),
    INDEX idx_ts (ts)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
