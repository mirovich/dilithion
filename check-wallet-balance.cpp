// Copyright (c) 2025 The Dilithion Core developers
// Distributed under the MIT software license

/**
 * Wallet Balance Checker Utility
 *
 * Simple command-line tool to check wallet balances across multiple datadirs
 * Usage: ./check-wallet-balance [datadir1] [datadir2] [datadir3] ...
 *
 * If no datadirs are specified, checks default testnet datadirs:
 * - .dilithion-testnet
 * - .dilithion-testnet-node2
 * - .dilithion-testnet-node3
 */

#include <wallet/wallet.h>
#include <rpc/auth.h>
#include <util/config.h>
#include <util/strencodings.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstdlib>
#include <fstream>
#include <cstring>

#ifdef _WIN32
    #include <windows.h>
    #include <shlobj.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
#endif

// Forward declaration of RPC helper
bool CallRPC(const std::string& method, const std::string& params, 
             const std::string& auth, uint16_t port, std::string& response);
std::string GetRPCCredentials(const std::string& datadir);
uint16_t GetRPCPort(const std::string& datadir, const std::string& unit);


// Get the home/appdata directory (Unicode-safe on Windows)
std::string GetHomeDir() {
#ifdef _WIN32
    // Use wide API to handle non-ASCII usernames (Cyrillic, CJK, etc.)
    wchar_t widePath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, widePath))) {
        // Convert to 8.3 short path (always ASCII-safe)
        DWORD shortLen = GetShortPathNameW(widePath, NULL, 0);
        if (shortLen > 0) {
            std::wstring shortPath(shortLen, L'\0');
            DWORD result = GetShortPathNameW(widePath, &shortPath[0], shortLen);
            if (result > 0 && result < shortLen) {
                shortPath.resize(result);
                return std::string(shortPath.begin(), shortPath.end());
            }
        }
    }
    // Fallback to narrow API (works for ASCII usernames)
    char path[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, path) == S_OK) {
        return std::string(path);
    }
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        return std::string(userprofile);
    }
    return ".";
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home);
    }
    return ".";
#endif
}

// Get full path to datadir
std::string GetFullDataDir(const std::string& name) {
    std::string home = GetHomeDir();
#ifdef _WIN32
    return home + "\\" + name;
#else
    return home + "/" + name;
#endif
}

// Coinbase maturity constant (must match consensus/params.h)
static const unsigned int COINBASE_MATURITY = 100;

// Detect chain from datadir path
std::string DetectUnit(const std::string& datadir) {
    // Check if path contains ".dilv" (DilV chain)
    if (datadir.find(".dilv") != std::string::npos &&
        datadir.find(".dilithion") == std::string::npos) {
        return "DilV";
    }
    return "DIL";
}

void print_wallet_info(const std::string& datadir) {
    std::string unit = DetectUnit(datadir);
    uint16_t rpcport = GetRPCPort(datadir, unit);
    std::string auth = GetRPCCredentials(datadir);
    
    bool rpc_success = false;
    std::string response;

    // Try RPC first
    if (!auth.empty()) {
        if (CallRPC("getbalance", "[]", auth, rpcport, response)) {
            // Very simple JSON parsing for getbalance result
            size_t resPos = response.find("\"result\":");
            if (resPos != std::string::npos) {
                std::string balanceStr = response.substr(resPos + 9);
                size_t commaPos = balanceStr.find_first_of(",}");
                if (commaPos != std::string::npos) {
                    balanceStr = balanceStr.substr(0, commaPos);
                    double balance = std::stod(balanceStr);
                    
                    std::cout << "  Chain: " << unit << " (via RPC)" << std::endl;
                    std::cout << "  Total Balance: " << std::fixed << std::setprecision(8) 
                              << balance << " " << unit << std::endl;
                    
                    // Also get addresses for completeness
                    if (CallRPC("getaddresses", "[]", auth, rpcport, response)) {
                         size_t count = 0;
                         size_t pos = 0;
                         while ((pos = response.find("\"", pos)) != std::string::npos) {
                             count++;
                             pos = response.find("\"", pos + 1);
                             if (pos != std::string::npos) pos++;
                         }
                         // Each address has 2 quotes, result is an array of strings
                         // Rough estimate of address count
                         std::cout << "  Addresses: ~" << (count / 2) << std::endl;
                    }
                    
                    rpc_success = true;
                }
            }
        }
    }

    if (rpc_success) return;

    // Fallback to direct file access if RPC fails
    std::cout << "  (Node not responding on port " << rpcport << ", falling back to direct file access)" << std::endl;

#ifdef _WIN32
    std::string wallet_file = datadir + "\\wallet.dat";
#else
    std::string wallet_file = datadir + "/wallet.dat";
#endif

    CWallet wallet;

    // Try to load wallet
    if (!wallet.Load(wallet_file)) {
        std::cout << "  Status: Wallet file not found or could not be loaded" << std::endl;
        return;
    }

    // Get wallet addresses
    auto addresses = wallet.GetAddresses();
    if (addresses.empty()) {
        std::cout << "  Status: Empty wallet (no addresses)" << std::endl;
        return;
    }

    // Get best block height from wallet (for maturity calculation)
    int32_t bestHeight = wallet.GetBestBlockHeight();

    // Get unspent transaction outputs and calculate mature/immature balance
    auto utxos = wallet.GetUnspentTxOuts();

    int64_t matureBalance = 0;
    int64_t immatureBalance = 0;
    int64_t totalBalance = 0;

    for (const auto& utxo : utxos) {
        totalBalance += utxo.nValue;

        // Calculate confirmations
        // If bestHeight is not initialized (-1), treat all as mature for display
        if (bestHeight >= 0 && utxo.nHeight > 0) {
            unsigned int confirmations = static_cast<unsigned int>(bestHeight - utxo.nHeight + 1);
            if (confirmations >= COINBASE_MATURITY) {
                matureBalance += utxo.nValue;
            } else {
                immatureBalance += utxo.nValue;
            }
        } else {
            // No height info available, treat as mature
            matureBalance += utxo.nValue;
        }
    }

    double totalCoins = static_cast<double>(totalBalance) / 100000000.0;
    double matureCoins = static_cast<double>(matureBalance) / 100000000.0;
    double immatureCoins = static_cast<double>(immatureBalance) / 100000000.0;

    std::cout << "  Chain: " << unit << std::endl;
    std::cout << "  Addresses: " << addresses.size() << std::endl;
    std::cout << "  UTXOs: " << utxos.size() << std::endl;
    if (bestHeight >= 0) {
        std::cout << "  Synced Height: " << bestHeight << std::endl;
    }
    std::cout << std::endl;
    std::cout << "  Balance Breakdown:" << std::endl;
    std::cout << "    Total:    " << std::fixed << std::setprecision(8)
              << totalCoins << " " << unit << std::endl;
    std::cout << "    Mature:   " << std::fixed << std::setprecision(8)
              << matureCoins << " " << unit << " (spendable)" << std::endl;
    std::cout << "    Immature: " << std::fixed << std::setprecision(8)
              << immatureCoins << " " << unit << " (needs " << COINBASE_MATURITY << " confirmations)" << std::endl;

    // Show first address
    std::cout << std::endl;
    if (!addresses.empty()) {
        std::cout << "  Primary Address: " << addresses[0].ToString() << std::endl;
    }
}

int main(int argc, char* argv[]) {
    std::cout << "=========================================" << std::endl;
    std::cout << "Dilithion Wallet Balance Checker" << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << std::endl;

    std::vector<std::string> datadirs;

    if (argc > 1) {
        // Use datadirs from command line
        for (int i = 1; i < argc; ++i) {
            datadirs.push_back(argv[i]);
        }
    } else {
        // Auto-detect: check both DIL and DilV default datadirs
        std::string dilDir = GetFullDataDir(".dilithion");
        std::string dilvDir = GetFullDataDir(".dilv");

#ifdef _WIN32
        std::string dilWallet = dilDir + "\\wallet.dat";
        std::string dilvWallet = dilvDir + "\\wallet.dat";
#else
        std::string dilWallet = dilDir + "/wallet.dat";
        std::string dilvWallet = dilvDir + "/wallet.dat";
#endif

        // Check which wallets exist
        bool hasDil = false, hasDilv = false;
        {
            std::ifstream f(dilWallet);
            hasDil = f.good();
        }
        {
            std::ifstream f(dilvWallet);
            hasDilv = f.good();
        }

        if (hasDil) datadirs.push_back(dilDir);
        if (hasDilv) datadirs.push_back(dilvDir);

        if (datadirs.empty()) {
            // Fallback to DIL default even if not found (will show error message)
            datadirs.push_back(dilDir);
        }
    }

    // Check each wallet
    for (size_t i = 0; i < datadirs.size(); ++i) {
        std::cout << "Node " << (i + 1) << " (" << datadirs[i] << "):" << std::endl;
        print_wallet_info(datadirs[i]);
        std::cout << std::endl;
    }

    std::cout << "=========================================" << std::endl;
    std::cout << std::endl;
    return 0;
}

// RPC Helper implementation
bool CallRPC(const std::string& method, const std::string& params, 
             const std::string& auth, uint16_t port, std::string& response) {
    std::string jsonBody = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" + method + "\",\"params\":" + params + "}";
    
#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
#endif

#ifdef _WIN32
    DWORD timeout_ms = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return false;
    }

    std::string authBase64 = RPCAuth::Base64Encode(reinterpret_cast<const uint8_t*>(auth.c_str()), auth.size());
    std::string httpRequest = 
        "POST / HTTP/1.1\r\n"
        "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
        "Content-Type: application/json\r\n"
        "X-Dilithion-RPC: 1\r\n"
        "Authorization: Basic " + authBase64 + "\r\n"
        "Content-Length: " + std::to_string(jsonBody.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + jsonBody;

    send(sock, httpRequest.c_str(), httpRequest.size(), 0);

    char buf[4096];
    int bytes;
    response.clear();
    while ((bytes = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[bytes] = '\0';
        response += buf;
    }

#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    
    // Find body
    size_t bodyPos = response.find("\r\n\r\n");
    if (bodyPos != std::string::npos) {
        response = response.substr(bodyPos + 4);
    }
    
    return !response.empty();
}

std::string GetRPCCredentials(const std::string& datadir) {
    // 1. Try .cookie
    std::string cookiePath = datadir + "/.cookie";
    std::ifstream cookieFile(cookiePath);
    if (cookieFile.is_open()) {
        std::string creds;
        std::getline(cookieFile, creds);
        return creds;
    }
    
    // 2. Try dilithion.conf
    std::string confPath = datadir + "/dilithion.conf";
    CConfigParser config;
    if (config.LoadConfigFile(confPath)) {
        std::string user = config.GetString("rpcuser", "");
        std::string pass = config.GetString("rpcpassword", "");
        if (!user.empty() && !pass.empty()) {
            return user + ":" + pass;
        }
    }
    
    return "";
}

uint16_t GetRPCPort(const std::string& datadir, const std::string& unit) {
    std::string confPath = datadir + "/dilithion.conf";
    CConfigParser config;
    if (config.LoadConfigFile(confPath)) {
        int64_t port = config.GetInt64("rpcport", 0);
        if (port > 0 && port <= 65535) return (uint16_t)port;
    }
    return (unit == "DilV") ? 9332 : 8332;
}

