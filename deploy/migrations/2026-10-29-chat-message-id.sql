-- Apply once, after a backup, to the existing database selected with mysql DATABASE.
-- NULL preserves every old record; only new messages use stable unique IDs.
ALTER TABLE chat_message
    ADD COLUMN message_id VARCHAR(64) CHARACTER SET ascii NULL,
    ADD UNIQUE KEY uq_chat_message_id (message_id);
