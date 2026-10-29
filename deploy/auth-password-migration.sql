-- Run before starting the version that writes PBKDF2 credentials.
-- Select the existing application database with the mysql DATABASE argument.

CREATE TABLE IF NOT EXISTS users (
    id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(128) NOT NULL,
    password VARCHAR(255) NOT NULL,
    UNIQUE KEY uq_users_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Existing accounts keep their exact stored credential. No reset or rewrite here.
ALTER TABLE users MODIFY COLUMN password VARCHAR(255) NOT NULL;
