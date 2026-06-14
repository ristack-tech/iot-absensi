<?php
require_once 'db.php';

$db = new Database();
$conn = $db->getConnection();

// Handle POST - add attendance log
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $data = json_decode(file_get_contents('php://input'), true);
    $fingerprint_id = $data['fingerprint_id'] ?? 0;
    $status = $data['status'] ?? 'in';

    if ($fingerprint_id > 0) {
        $stmt = $conn->prepare("INSERT INTO attendance_logs (fingerprint_id, status) VALUES (?, ?)");
        $stmt->bind_param("is", $fingerprint_id, $status);
        $stmt->execute();
        echo json_encode(['success' => true, 'id' => $stmt->insert_id]);
    } else {
        echo json_encode(['success' => false, 'error' => 'Invalid fingerprint_id']);
    }
    exit;
}

// Handle GET - fetch attendance logs
$result = $conn->query("SELECT * FROM attendance_logs ORDER BY timestamp DESC LIMIT 100");
$logs = [];
while ($row = $result->fetch_assoc()) {
    $logs[] = $row;
}
?>
<!DOCTYPE html>
<html>
<head>
    <title>Attendance Logs</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }
        h1 { color: #333; }
        table { width: 100%; border-collapse: collapse; background: white; }
        th, td { padding: 10px; border: 1px solid #ddd; text-align: left; }
        th { background: #4CAF50; color: white; }
        tr:nth-child(even) { background: #f9f9f9; }
        .status-in { color: green; font-weight: bold; }
        .status-out { color: red; font-weight: bold; }
        form { margin-bottom: 20px; background: white; padding: 15px; border-radius: 5px; }
        input, select, button { padding: 8px; margin: 5px; }
    </style>
</head>
<body>
    <h1>Attendance Logs</h1>

    <form id="addForm">
        <h3>Add Attendance (Testing)</h3>
        <input type="number" name="fingerprint_id" placeholder="Fingerprint ID" required>
        <select name="status">
            <option value="in">In</option>
            <option value="out">Out</option>
        </select>
        <button type="submit">Add</button>
    </form>

    <table>
        <thead>
            <tr>
                <th>ID</th>
                <th>Fingerprint ID</th>
                <th>Timestamp</th>
                <th>Status</th>
            </tr>
        </thead>
        <tbody>
            <?php foreach ($logs as $log): ?>
            <tr>
                <td><?= $log['id'] ?></td>
                <td><?= $log['fingerprint_id'] ?></td>
                <td><?= $log['timestamp'] ?></td>
                <td class="status-<?= $log['status'] ?>"><?= strtoupper($log['status']) ?></td>
            </tr>
            <?php endforeach; ?>
        </tbody>
    </table>

    <script>
    document.getElementById('addForm').onsubmit = async (e) => {
        e.preventDefault();
        const form = e.target;
        const data = {
            fingerprint_id: form.fingerprint_id.value,
            status: form.status.value
        };
        const res = await fetch('index.php', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify(data)
        });
        const result = await res.json();
        if (result.success) {
            location.reload();
        } else {
            alert('Error: ' + result.error);
        }
    };
    </script>
</body>
</html>
