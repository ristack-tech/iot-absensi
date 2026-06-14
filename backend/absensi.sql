-- Database: absensi

CREATE DATABASE IF NOT EXISTS absensi;
USE absensi;

-- Table: attendance_logs
CREATE TABLE IF NOT EXISTS attendance_logs (
    id INT AUTO_INCREMENT PRIMARY KEY,
    fingerprint_id INT NOT NULL,
    timestamp DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    status ENUM('in', 'out') DEFAULT 'in',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Sample data for testing
INSERT INTO attendance_logs (fingerprint_id, status) VALUES
(1, 'in'),
(2, 'in'),
(1, 'out'),
(3, 'in');
