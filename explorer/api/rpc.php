<?php
/**
 * Shared RPC helper for Dilithion Block Explorer API
 * Proxies JSON-RPC calls to DIL (127.0.0.1:8332) or DilV (127.0.0.1:9332)
 */

// Chain configuration: ports, units, rewards
function getChainConfig() {
    $chain = strtolower($_GET['chain'] ?? 'dil');
    if ($chain !== 'dilv') $chain = 'dil';  // sanitize: anything non-dilv → dil

    $configs = [
        'dil'  => ['rpc_port' => 8332, 'rest_port' => 8334, 'unit' => 'DIL', 'reward' => 50, 'name' => 'Dilithion'],
        'dilv' => ['rpc_port' => 9332, 'rest_port' => 9334, 'unit' => 'DilV', 'reward' => 100, 'name' => 'DilV'],
    ];

    return array_merge(['chain' => $chain], $configs[$chain]);
}

// Persistent curl handle for connection reuse (keep-alive)
$_rpcCurlHandle = null;
$_rpcPort = null;
$_rpcAuth = null;

function getRPCCredentials($port) {
    // Phase 10: Dynamic credential discovery (.cookie or dilithion.conf)
    $home = getenv('HOME');
    if (!$home) {
        $home = getenv('APPDATA') ?: '/var/lib/dilithion';
    }
    
    $chain = $port == 9332 ? 'dilv' : 'dil';
    $dirName = $port == 9332 ? '.dilv' : '.dilithion';
    $datadir = "{$home}/{$dirName}";
    
    // 1. Try .cookie file (Bitcoin Core model)
    $cookieFile = "{$datadir}/.cookie";
    if (file_exists($cookieFile)) {
        return trim(file_get_contents($cookieFile));
    }
    
    // 2. Try dilithion.conf
    $confFile = "{$datadir}/dilithion.conf";
    if (file_exists($confFile)) {
        $conf = file_get_contents($confFile);
        $user = ''; $pass = '';
        if (preg_match('/^rpcuser=(.*)$/m', $conf, $m)) $user = trim($m[1]);
        if (preg_match('/^rpcpassword=(.*)$/m', $conf, $m)) $pass = trim($m[1]);
        if ($user && $pass) return "{$user}:{$pass}";
    }
    
    // 3. Last resort fallback (v4.0 legacy default)
    return "rpc:rpc";
}

function dilithionRPC($method, $params = []) {
    global $_rpcCurlHandle, $_rpcPort, $_rpcAuth;

    $config = getChainConfig();
    $port = $config['rpc_port'];

    // Re-create handle if port changed
    if ($_rpcCurlHandle === null || $_rpcPort !== $port) {
        if ($_rpcCurlHandle !== null) curl_close($_rpcCurlHandle);
        $_rpcPort = $port;
        $_rpcAuth = getRPCCredentials($port);
        $_rpcCurlHandle = curl_init();
        curl_setopt($_rpcCurlHandle, CURLOPT_URL, "http://127.0.0.1:{$port}/");
        curl_setopt($_rpcCurlHandle, CURLOPT_RETURNTRANSFER, true);
        // 15s read timeout: long enough to catch a 7s getrawtransaction
        // chain-scan when txindex isn't enabled. Drop back to 5s once
        // txindex=1 is on the node and scans are O(1).
        curl_setopt($_rpcCurlHandle, CURLOPT_TIMEOUT, 15);
        curl_setopt($_rpcCurlHandle, CURLOPT_CONNECTTIMEOUT, 5);
        curl_setopt($_rpcCurlHandle, CURLOPT_POST, true);
        curl_setopt($_rpcCurlHandle, CURLOPT_HTTPHEADER, [
            'Content-Type: application/json',
            'X-Dilithion-RPC: 1',
            'Authorization: Basic ' . base64_encode($_rpcAuth),
            'Connection: keep-alive'
        ]);
    }

    $payload = json_encode([
        'jsonrpc' => '2.0',
        'id' => 1,
        'method' => $method,
        'params' => $params
    ]);

    curl_setopt($_rpcCurlHandle, CURLOPT_POSTFIELDS, $payload);
    $response = curl_exec($_rpcCurlHandle);
    $httpCode = curl_getinfo($_rpcCurlHandle, CURLINFO_HTTP_CODE);

    if ($httpCode !== 200 || !$response) {
        return null;
    }

    $data = json_decode($response, true);
    if (isset($data['error']) && $data['error'] !== null) {
        return null;
    }

    return $data['result'] ?? null;
}

function sendJSON($data) {
    header('Access-Control-Allow-Origin: *');
    header('Access-Control-Allow-Methods: GET, OPTIONS');
    header('Access-Control-Allow-Headers: Content-Type');
    header('Content-Type: application/json');
    header('Cache-Control: no-cache, no-store, must-revalidate');

    if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
        http_response_code(200);
        exit;
    }

    echo json_encode($data, JSON_PRETTY_PRINT);
    exit;
}

function sendError($message, $code = 400) {
    http_response_code($code);
    sendJSON(['error' => $message]);
}
