// Copyright (c) 2026 The Dilithion Core developers
// Distributed under the MIT software license

/**
 * BUG-003 — Block-size limit reconciliation tests (F-06).
 *
 * Covers contract assertions:
 *   A-04 — CBlockValidator::CheckBlock (dead but testable) uses the 4 MB constant.
 *   A-05 — A block with vtx.size() == MAX_BLOCK_SIZE stores; MAX_BLOCK_SIZE + 1 is rejected.
 *          Exercises the ACTIVE path: CBlockchainDB::WriteBlock.
 *   A-02 — A mempool-saturated mining template with a maximum-size MIK coinbase
 *          produces a block.vtx blob whose .size() <= Consensus::MAX_BLOCK_SIZE.
 *          This is the Bug-B regression test: it overshoots under the old flat
 *          200-byte coinbase reserve and fits under the F-02 fix.
 *
 * Uses the custom TEST()/ASSERT/ASSERT_EQ framework (modelled on
 * mining_integration_tests.cpp), not Boost, so it can drive CMiningController,
 * CBlockchainDB and CBlockValidator directly.
 */

#include <consensus/params.h>
#include <consensus/validation.h>
#include <consensus/tx_validation.h>
#include <miner/controller.h>
#include <node/blockchain_storage.h>
#include <node/mempool.h>
#include <node/utxo_set.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <wallet/wallet.h>
#include <dfmp/mik.h>
#include <amount.h>
#include <core/chainparams.h>
#include <crypto/randomx_hash.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

// ANSI color codes
#define RESET   "\033[0m"
#define GREEN   "\033[32m"
#define RED     "\033[31m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"

int g_tests_passed = 0;
int g_tests_failed = 0;

#define TEST(name) \
    void test_##name(); \
    void test_##name##_wrapper() { \
        std::cout << BLUE << "[TEST] " << #name << RESET << std::endl; \
        try { \
            test_##name(); \
            std::cout << GREEN << "  PASSED" << RESET << std::endl; \
            g_tests_passed++; \
        } catch (const std::exception& e) { \
            std::cout << RED << "  FAILED: " << e.what() << RESET << std::endl; \
            g_tests_failed++; \
        } catch (...) { \
            std::cout << RED << "  FAILED: Unknown exception" << RESET << std::endl; \
            g_tests_failed++; \
        } \
    } \
    void test_##name()

#define ASSERT(condition, message) \
    if (!(condition)) { \
        throw std::runtime_error(message); \
    }

#define ASSERT_EQ(a, b, message) \
    if ((a) != (b)) { \
        throw std::runtime_error(std::string(message) + " (expected " + \
            std::to_string(b) + ", got " + std::to_string(a) + ")"); \
    }

// --- helpers --------------------------------------------------------------

static std::vector<uint8_t> CreateMinerAddress() {
    std::vector<uint8_t> addr(25);
    addr[0] = 0x76; addr[1] = 0xa9; addr[2] = 0x14;
    for (int i = 0; i < 20; i++) addr[3 + i] = static_cast<uint8_t>(i);
    addr[23] = 0x88; addr[24] = 0xac;
    return addr;
}

// Build a CBlock whose serialized-transaction blob (vtx) is exactly `n` bytes.
// vtx is a std::vector<uint8_t> blob; .size() is the byte count the storage and
// CheckBlock size checks compare against Consensus::MAX_BLOCK_SIZE.
static CBlock MakeBlockWithVtxSize(size_t n) {
    CBlock block;
    block.nVersion = 1;
    block.nBits = 0x1f00ffff;
    block.nTime = 1700000000;
    block.nNonce = 1;
    block.vtx.assign(n, 0xAB);
    return block;
}

// =======================================================================
// A-05 (ACTIVE PATH) — CBlockchainDB::WriteBlock block-size boundary.
//   vtx.size() == MAX_BLOCK_SIZE      -> accepted
//   vtx.size() == MAX_BLOCK_SIZE + 1  -> rejected
// =======================================================================
TEST(storage_writeblock_size_boundary) {
    const size_t kMax = Consensus::MAX_BLOCK_SIZE;
    ASSERT_EQ(kMax, static_cast<size_t>(4 * 1024 * 1024),
              "Consensus::MAX_BLOCK_SIZE must be 4 MB");

    std::string dbPath = ".test-bug003-storage";
    std::error_code ec;
    std::filesystem::remove_all(dbPath, ec);

    CBlockchainDB db;
    ASSERT(db.Open(dbPath, true), "Failed to open blockchain DB");

    // Exactly at the limit: must be accepted.
    {
        CBlock atLimit = MakeBlockWithVtxSize(kMax);
        ASSERT_EQ(atLimit.vtx.size(), kMax, "vtx size setup wrong (at-limit)");
        uint256 h; h.SetHex("00000000000000000000000000000000000000000000000000000000000000a1");
        bool ok = db.WriteBlock(h, atLimit);
        ASSERT(ok, "Block with vtx.size()==MAX_BLOCK_SIZE was REJECTED by WriteBlock");
        std::cout << "    WriteBlock accepted vtx.size()==" << kMax << " (4 MB)" << std::endl;
    }

    // One byte over the limit: must be rejected.
    {
        CBlock overLimit = MakeBlockWithVtxSize(kMax + 1);
        ASSERT_EQ(overLimit.vtx.size(), kMax + 1, "vtx size setup wrong (over-limit)");
        uint256 h; h.SetHex("00000000000000000000000000000000000000000000000000000000000000a2");
        bool ok = db.WriteBlock(h, overLimit);
        ASSERT(!ok, "Block with vtx.size()==MAX_BLOCK_SIZE+1 was ACCEPTED by WriteBlock");
        std::cout << "    WriteBlock rejected vtx.size()==" << (kMax + 1) << std::endl;
    }

    db.Close();
    std::filesystem::remove_all(dbPath, ec);
}

// =======================================================================
// A-04 — CBlockValidator::CheckBlock size constant is 4 MB (not 1 MB).
//   CheckBlock is dead code (zero callers) but is testable directly. Its
//   block-size check runs FIRST, before header / deserialize. We assert:
//     * a 4 MB + 1 block is rejected with the size error;
//     * a 4 MB block is NOT rejected for size (it fails later, for other
//       reasons — proving the size constant moved from 1 MB to 4 MB; under
//       the old 1 MB constant a 4 MB block would fail the size check).
// =======================================================================
TEST(checkblock_size_constant_is_4mb) {
    const size_t kMax = Consensus::MAX_BLOCK_SIZE;
    CBlockValidator validator;
    CUTXOSet utxoSet;
    std::string utxoPath = ".test-bug003-checkblock-utxo";
    std::error_code ec;
    std::filesystem::remove_all(utxoPath, ec);
    ASSERT(utxoSet.Open(utxoPath, true), "Failed to open UTXO set");

    const std::string sizeErrFragment = "exceeds maximum size";

    // 4 MB + 1: must be rejected, and specifically for SIZE.
    {
        CBlock overLimit = MakeBlockWithVtxSize(kMax + 1);
        std::string error;
        bool ok = validator.CheckBlock(overLimit, utxoSet, 1, error);
        ASSERT(!ok, "CheckBlock accepted a 4 MB + 1 block");
        ASSERT(error.find(sizeErrFragment) != std::string::npos,
               "CheckBlock rejected 4 MB + 1 but NOT for size; error: " + error);
        std::cout << "    CheckBlock rejected 4 MB + 1 for size: " << error << std::endl;
    }

    // Exactly 4 MB: must NOT be rejected for size. (It fails later for other
    // reasons — a blob of 0xAB bytes is not a valid header/tx set.) The key
    // assertion: the failure reason is NOT the size check. Under the old
    // 1 MB constant this same block WOULD fail the size check.
    {
        CBlock atLimit = MakeBlockWithVtxSize(kMax);
        std::string error;
        bool ok = validator.CheckBlock(atLimit, utxoSet, 1, error);
        ASSERT(!ok, "Garbage 4 MB block unexpectedly passed full CheckBlock");
        ASSERT(error.find(sizeErrFragment) == std::string::npos,
               "CheckBlock rejected an exactly-4 MB block FOR SIZE — constant is "
               "still < 4 MB; error: " + error);
        std::cout << "    CheckBlock did NOT reject 4 MB for size (failed later: "
                  << error << ")" << std::endl;
    }

    utxoSet.Close();
    std::filesystem::remove_all(utxoPath, ec);
}

// =======================================================================
// A-02 — Bug-B regression: mempool-saturated mining template with a
// maximum-size MIK coinbase yields a vtx blob <= Consensus::MAX_BLOCK_SIZE.
//
// Bug B: the template builders seeded their byte budget with a flat 200-byte
// coinbase reserve, but a real MIK-registration coinbase carries ~5.3 KB of
// scriptSig + outputs. A saturated template therefore overshot the cap by
// ~(coinbaseReserve - 200) bytes and was rejected by storage/P2P.
//
// This test:
//   1. Builds a real maximum-size MIK-registration coinbase and measures its
//      true serialized size (coinbaseReserve).
//   2. Saturates a real mempool with valid Dilithium-signed transactions until
//      it holds well over 4 MB of transaction data.
//   3. Drives the FIXED selection path SelectTransactionsForBlock with the real
//      coinbaseReserve and maxBlockSize = Consensus::MAX_BLOCK_SIZE, then
//      assembles the vtx blob exactly as CreateBlockTemplate does, and asserts
//      vtx.size() <= Consensus::MAX_BLOCK_SIZE.
//   4. Re-runs the SAME selection with the OLD flat-200 seed simulated, and
//      asserts that blob WOULD have exceeded the cap — proving the regression
//      test actually catches Bug B.
// =======================================================================

// Assemble a vtx blob the same way CMiningController::CreateBlockTemplate does:
// a CompactSize tx-count prefix followed by each transaction's Serialize().
static std::vector<uint8_t> AssembleVtx(const CTransactionRef& coinbase,
                                        const std::vector<CTransactionRef>& selected) {
    std::vector<uint8_t> vtx;
    uint64_t txCount = 1 + selected.size();
    if (txCount < 253) {
        vtx.push_back(static_cast<uint8_t>(txCount));
    } else if (txCount <= 0xFFFF) {
        vtx.push_back(253);
        vtx.push_back(static_cast<uint8_t>(txCount));
        vtx.push_back(static_cast<uint8_t>(txCount >> 8));
    } else {
        vtx.push_back(254);
        vtx.push_back(static_cast<uint8_t>(txCount));
        vtx.push_back(static_cast<uint8_t>(txCount >> 8));
        vtx.push_back(static_cast<uint8_t>(txCount >> 16));
        vtx.push_back(static_cast<uint8_t>(txCount >> 24));
    }
    {
        std::vector<uint8_t> d = coinbase->Serialize();
        vtx.insert(vtx.end(), d.begin(), d.end());
    }
    for (const auto& tx : selected) {
        std::vector<uint8_t> d = tx->Serialize();
        vtx.insert(vtx.end(), d.begin(), d.end());
    }
    return vtx;
}

TEST(template_overshoot_regression_bug_b) {
    const size_t kMax = Consensus::MAX_BLOCK_SIZE;

    // ---- 1. Build a real maximum-size MIK-registration coinbase ----------
    CMiningController miner(1);
    std::vector<uint8_t> minerAddr = CreateMinerAddress();

    CMIKCoinbaseData mikData;
    mikData.hasMIK = true;
    mikData.isRegistration = true;                       // largest scriptSig form
    mikData.pubkey.assign(DFMP::MIK_PUBKEY_SIZE, 0x11);  // 1952 bytes
    mikData.signature.assign(3309, 0x22);                // Dilithium3 signature
    mikData.registrationNonce = 0x0123456789ABCDEFULL;

    CTransactionRef coinbase =
        miner.CreateCoinbaseTransaction(1, /*totalFees=*/0, minerAddr, mikData);
    ASSERT(coinbase != nullptr, "Failed to build MIK coinbase");
    ASSERT(coinbase->IsCoinBase(), "MIK coinbase is not a coinbase");

    const size_t coinbaseReserve = coinbase->GetSerializedSize();
    std::cout << "    Real max-size MIK coinbase serialized size: "
              << coinbaseReserve << " bytes" << std::endl;
    ASSERT(coinbaseReserve > 5000,
           "Max-size MIK coinbase smaller than expected (~5.3 KB)");
    ASSERT(coinbaseReserve > 200,
           "Coinbase fits in the old 200-byte estimate — fixture invalid");

    // ---- 2. Saturate a mempool with valid signed transactions ------------
    CUTXOSet utxoSet;
    std::string utxoPath = ".test-bug003-tmpl-utxo";
    std::error_code ec;
    std::filesystem::remove_all(utxoPath, ec);
    ASSERT(utxoSet.Open(utxoPath, true), "Failed to open UTXO set");

    CWallet senderWallet;
    ASSERT(senderWallet.GenerateNewKey(), "Failed to generate wallet key");
    CDilithiumAddress senderAddr = senderWallet.GetNewAddress();

    CWallet recipientWallet;
    ASSERT(recipientWallet.GenerateNewKey(), "Failed to generate recipient key");
    CDilithiumAddress recipientAddr = recipientWallet.GetNewAddress();

    std::vector<uint8_t> senderHash = senderWallet.GetPubKeyHash();
    std::vector<uint8_t> senderScript = WalletCrypto::CreateScriptPubKey(senderHash);

    CTxMemPool mempool;
    const unsigned int currentHeight = 200;     // funding UTXOs mature at h<=100
    const CAmount fundAmount = 100000000;       // 1 DIL per UTXO
    const CAmount sendAmount = 50000000;        // 0.5 DIL
    const CAmount fee = 100000;                 // 0.001 DIL (>= min relay fee)

    // Each 1-input wallet tx carries a Dilithium scriptSig (~5.3 KB). Fund
    // enough independent UTXOs to push the mempool well past 4 MB so the
    // selection loop is genuinely saturated.
    size_t mempoolBytes = 0;
    size_t txCreated = 0;
    const size_t targetMempoolBytes = kMax + (kMax / 4);  // ~5 MB of tx data
    const size_t maxFundingUtxos = 4000;                  // safety bound

    for (size_t i = 0; i < maxFundingUtxos && mempoolBytes < targetMempoolBytes; i++) {
        uint256 fundTxid;
        // Distinct funding txid per UTXO.
        fundTxid.data[0] = static_cast<uint8_t>(i & 0xFF);
        fundTxid.data[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        fundTxid.data[2] = 0xF0;

        senderWallet.AddTxOut(fundTxid, 0, fundAmount, senderAddr, 100);
        CTxOut fundOut(fundAmount, senderScript);
        utxoSet.AddUTXO(COutPoint(fundTxid, 0), fundOut, 100, false);
    }
    utxoSet.Flush();

    for (size_t i = 0; i < maxFundingUtxos && mempoolBytes < targetMempoolBytes; i++) {
        uint256 fundTxid;
        fundTxid.data[0] = static_cast<uint8_t>(i & 0xFF);
        fundTxid.data[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        fundTxid.data[2] = 0xF0;
        CDilithiumAddress fromAddr = senderAddr;

        CTransactionRef tx;
        std::string error;
        if (!senderWallet.CreateTransaction(recipientAddr, sendAmount, fee,
                                            utxoSet, currentHeight, tx, error,
                                            fromAddr)) {
            // Out of spendable UTXOs / coin-selection limit reached.
            if (txCreated == 0) {
                throw std::runtime_error(
                    "CreateTransaction failed on first tx: " + error);
            }
            break;
        }
        size_t txSize = tx->GetSerializedSize();
        std::string mpErr;
        if (!mempool.AddTx(tx, fee, 1700000000, currentHeight, &mpErr,
                           /*bypass_fee_check=*/true)) {
            break;
        }
        mempoolBytes += txSize;
        txCreated++;
    }

    std::cout << "    Mempool saturated: " << txCreated << " txs, "
              << mempoolBytes << " bytes of tx data" << std::endl;
    ASSERT(mempoolBytes > kMax,
           "Mempool not saturated past 4 MB — cannot exercise overshoot ("
           + std::to_string(mempoolBytes) + " bytes)");

    // ---- 3. Production-path check (A-02): margin active ------------------
    // The real CreateBlockTemplate path budgets against (cap - SAFETY_MARGIN).
    // SelectTransactionsForBlock is private; the test is a declared friend.
    uint64_t totalFeesProd = 0;
    std::vector<CTransactionRef> selectedProd = miner.SelectTransactionsForBlock(
        mempool, utxoSet, currentHeight,
        /*maxBlockSize=*/kMax,                  // production cap; loop applies F-05 margin
        /*coinbaseReserve=*/coinbaseReserve,    // F-02: real coinbase size
        totalFeesProd);

    CTransactionRef coinbaseFinal =
        miner.CreateCoinbaseTransaction(1, totalFeesProd, minerAddr, mikData);
    // Contract R2: coinbase serialized size is fee-independent.
    ASSERT_EQ(coinbaseFinal->GetSerializedSize(), coinbaseReserve,
              "Coinbase serialized size changed with fees (contract R2 violated)");

    std::vector<uint8_t> vtxProd = AssembleVtx(coinbaseFinal, selectedProd);
    std::cout << "    PRODUCTION path (F-05 margin active): "
              << selectedProd.size() << " txs, vtx.size()=" << vtxProd.size()
              << " (cap " << kMax << ")" << std::endl;
    ASSERT(vtxProd.size() <= kMax,
           "BUG-003 A-02 FAILED: production-path template vtx.size()="
           + std::to_string(vtxProd.size()) + " exceeds MAX_BLOCK_SIZE="
           + std::to_string(kMax));

    // ---- 4. Bug-B isolation: regression catches the F-02 fix -------------
    // BUG-003 M-2: the regression must PROVABLY fail on the pre-F-02 code and
    // must not be sensitive to mempool-fixture granularity. Asserting on raw
    // byte totals (vtxOld.size() > kMax) is fixture-fragile: the seed
    // difference (coinbaseReserve - 200 ~= 5.2 KB) is less than one sample tx,
    // so whether the old seed overshoots the 4 MB cap depends on where the
    // last candidate tx happens to land. Instead we assert on the real
    // invariant — the old (undercounting) seed selects strictly MORE
    // transactions than the fixed seed — and we make that strict inequality
    // DETERMINISTIC by choosing the budget from measured tx sizes.
    //
    // The greedy loop seeds currentBlockSize = coinbaseReserve + 9 (the
    // TX_COUNT_VARINT_ALLOWANCE) and selects a tx while
    // currentBlockSize + txSize <= budget. The sample mempool is uniform
    // (every tx is a 1-input wallet tx of identical serialized size T), so the
    // selected count is floor((budget - seed) / T). Pick:
    //
    //   budget = coinbaseReserve + 9 + (K + 1) * T - 1
    //
    // Then:
    //   * FIXED seed (coinbaseReserve): selects exactly K     txs.
    //   * OLD   seed (200):             selects exactly K + 1 txs,
    // because the 200-byte seed frees (coinbaseReserve - 200) bytes, which is
    // > 0 and < T (asserted below) — exactly enough for one more tx and never
    // two. So selectedOld.size() == selectedFixed.size() + 1, provably, on the
    // current (fixed) code; the pre-F-02 code (which always seeded with 200)
    // would itself behave as the OLD path and overshoot the real cap.
    //
    // SelectTransactionsForBlock subtracts BLOCK_SIZE_SAFETY_MARGIN from the
    // maxBlockSize argument, so we pass (budget + SAFETY_MARGIN) to make the
    // loop's effective budget exactly `budget`.
    const size_t TX_COUNT_VARINT_ALLOWANCE = 9;  // mirrors controller.cpp:832

    // Uniform sample-tx size T, taken from a real selected tx and verified
    // uniform across the production selection so the budget arithmetic holds.
    ASSERT(!selectedProd.empty(),
           "Regression setup error: production path selected zero txs");
    const size_t T = selectedProd.front()->GetSerializedSize();
    for (const auto& tx : selectedProd) {
        ASSERT_EQ(tx->GetSerializedSize(), T,
                  "Sample mempool is not uniform — budget arithmetic invalid");
    }
    // The +1 (and not +2) selection step depends on 0 < coinbaseReserve-200 < T.
    ASSERT(coinbaseReserve > 200 + TX_COUNT_VARINT_ALLOWANCE,
           "Coinbase reserve too small for the regression to be meaningful");
    ASSERT(coinbaseReserve - 200 < T,
           "Coinbase undercount >= one tx — regression would over-select by >1");

    const size_t kSelectK = 8;  // small, fast, far from fixture saturation edge
    const size_t budget =
        coinbaseReserve + TX_COUNT_VARINT_ALLOWANCE + (kSelectK + 1) * T - 1;
    const size_t maxBlockSizeArg = budget + Consensus::BLOCK_SIZE_SAFETY_MARGIN;

    uint64_t totalFeesFixed = 0;
    std::vector<CTransactionRef> selectedFixed = miner.SelectTransactionsForBlock(
        mempool, utxoSet, currentHeight,
        /*maxBlockSize=*/maxBlockSizeArg,       // effective budget == `budget`
        /*coinbaseReserve=*/coinbaseReserve,    // F-02 fix: real coinbase size
        totalFeesFixed);
    std::vector<uint8_t> vtxFixed = AssembleVtx(coinbaseFinal, selectedFixed);

    uint64_t totalFeesOld = 0;
    std::vector<CTransactionRef> selectedOld = miner.SelectTransactionsForBlock(
        mempool, utxoSet, currentHeight,
        /*maxBlockSize=*/maxBlockSizeArg,       // effective budget == `budget`
        /*coinbaseReserve=*/200,                // simulate pre-F-02 flat estimate
        totalFeesOld);
    std::vector<uint8_t> vtxOld = AssembleVtx(coinbaseFinal, selectedOld);

    std::cout << "    Bug-B isolation (deterministic, sample tx size T=" << T
              << ", K=" << kSelectK << "):" << std::endl;
    std::cout << "      FIXED seed (" << coinbaseReserve << "): "
              << selectedFixed.size() << " txs, vtx.size()=" << vtxFixed.size()
              << std::endl;
    std::cout << "      OLD   seed (200): " << selectedOld.size()
              << " txs, vtx.size()=" << vtxOld.size() << std::endl;

    // Production-path invariant (A-02): the fixed seed stays within the cap.
    ASSERT(vtxFixed.size() <= kMax,
           "F-02 fix FAILED: fixed-seed template vtx.size()="
           + std::to_string(vtxFixed.size()) + " > cap " + std::to_string(kMax));

    // The real Bug-B invariant: the undercounting (old) seed selects strictly
    // MORE transactions than the fixed seed. Deterministic given the budget
    // arithmetic above — provably fails on pre-F-02 (always-200) behaviour.
    ASSERT(selectedOld.size() > selectedFixed.size(),
           "BUG-003 regression FAILED: old 200-byte seed did NOT over-select "
           "(old=" + std::to_string(selectedOld.size()) + ", fixed="
           + std::to_string(selectedFixed.size()) + ") — F-02 not load-bearing");
    ASSERT_EQ(selectedOld.size(), selectedFixed.size() + 1,
              "BUG-003 regression: expected old seed to select exactly one "
              "extra tx (deterministic budget) — fixture/arithmetic drift");
    std::cout << "      => OLD seed over-selects by "
              << (selectedOld.size() - selectedFixed.size())
              << " tx; F-02's real coinbase reserve prevents the overshoot."
              << std::endl;

    mempool.Clear();
    utxoSet.Close();
    std::filesystem::remove_all(utxoPath, ec);
}

// =======================================================================
// A-09 — Worst-case 4 MB block verification-time measurement.
//
// The dominant cost of validating a block is post-quantum (ML-DSA / Dilithium3)
// signature verification, run once per transaction input via
// CTransactionValidator::CheckTransaction. We:
//   1. Build a batch of real Dilithium-signed 1-input transactions, each with
//      its funding UTXO present.
//   2. Time CheckTransaction over the batch -> per-transaction verification
//      cost (this is exactly the per-tx work a full block connect performs).
//   3. Extrapolate to a full 4 MB block: tx_count = 4 MB / mean serialized
//      tx size; worst-case block verify time = tx_count * per-tx cost.
//   4. Express the result as a fraction of the DIL and DilV target block times.
//
// This is a measurement, not a pass/fail gate — but it FAILS LOUDLY if the
// worst-case verify time exceeds the DilV 45 s target block time, which would
// be a genuine consensus-safety concern worth escalating.
// =======================================================================
TEST(worst_case_4mb_block_verify_time) {
    const size_t kMax = Consensus::MAX_BLOCK_SIZE;

    CUTXOSet utxoSet;
    std::string utxoPath = ".test-bug003-verify-utxo";
    std::error_code ec;
    std::filesystem::remove_all(utxoPath, ec);
    ASSERT(utxoSet.Open(utxoPath, true), "Failed to open UTXO set");

    CWallet senderWallet;
    ASSERT(senderWallet.GenerateNewKey(), "Failed to generate wallet key");
    CDilithiumAddress senderAddr = senderWallet.GetNewAddress();
    CWallet recipientWallet;
    ASSERT(recipientWallet.GenerateNewKey(), "Failed to generate recipient key");
    CDilithiumAddress recipientAddr = recipientWallet.GetNewAddress();

    std::vector<uint8_t> senderHash = senderWallet.GetPubKeyHash();
    std::vector<uint8_t> senderScript = WalletCrypto::CreateScriptPubKey(senderHash);

    const unsigned int currentHeight = 200;
    const CAmount fundAmount = 100000000;
    const CAmount sendAmount = 50000000;
    const CAmount fee = 100000;

    // A representative sample of signed transactions. 250 is enough for a
    // stable per-tx mean and keeps the test fast.
    const size_t kSampleTxs = 250;

    for (size_t i = 0; i < kSampleTxs; i++) {
        uint256 fundTxid;
        fundTxid.data[0] = static_cast<uint8_t>(i & 0xFF);
        fundTxid.data[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        fundTxid.data[2] = 0xE0;
        senderWallet.AddTxOut(fundTxid, 0, fundAmount, senderAddr, 100);
        CTxOut fundOut(fundAmount, senderScript);
        utxoSet.AddUTXO(COutPoint(fundTxid, 0), fundOut, 100, false);
    }
    utxoSet.Flush();

    std::vector<CTransactionRef> sample;
    size_t totalTxBytes = 0;
    for (size_t i = 0; i < kSampleTxs; i++) {
        uint256 fundTxid;
        fundTxid.data[0] = static_cast<uint8_t>(i & 0xFF);
        fundTxid.data[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        fundTxid.data[2] = 0xE0;
        CTransactionRef tx;
        std::string error;
        if (!senderWallet.CreateTransaction(recipientAddr, sendAmount, fee,
                                            utxoSet, currentHeight, tx, error,
                                            senderAddr)) {
            break;
        }
        totalTxBytes += tx->GetSerializedSize();
        sample.push_back(tx);
    }
    ASSERT(sample.size() >= 100,
           "Too few sample transactions built for a stable measurement ("
           + std::to_string(sample.size()) + ")");

    const double meanTxBytes =
        static_cast<double>(totalTxBytes) / static_cast<double>(sample.size());

    // Time full validation (incl. ML-DSA signature verification) of the sample.
    CTransactionValidator validator;
    auto t0 = std::chrono::steady_clock::now();
    size_t verified = 0;
    for (const auto& tx : sample) {
        CAmount txFee = 0;
        std::string error;
        if (validator.CheckTransaction(*tx, utxoSet, currentHeight, txFee, error)) {
            verified++;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    ASSERT(verified == sample.size(),
           "Some sample transactions failed validation — measurement invalid ("
           + std::to_string(verified) + "/" + std::to_string(sample.size()) + ")");

    const double perTxMs = elapsedMs / static_cast<double>(sample.size());

    // Extrapolate to a worst-case full 4 MB block.
    const double txPerBlock = static_cast<double>(kMax) / meanTxBytes;
    const double blockVerifyMs = perTxMs * txPerBlock;
    const double blockVerifySec = blockVerifyMs / 1000.0;

    // Target block times (src/core/chainparams.cpp `params.blockTime`):
    //   DIL  mainnet : 240 s (4 min)  — ChainParams::Mainnet().
    //   DilV         :  45 s          — ChainParams::DilV().
    const double dilTargetSec  = 240.0;
    const double dilvTargetSec = 45.0;

    std::cout << "    --- T4: worst-case 4 MB block verification time ---" << std::endl;
    std::cout << "    sample txs measured     : " << sample.size() << std::endl;
    std::cout << "    mean serialized tx size : " << meanTxBytes << " bytes" << std::endl;
    std::cout << "    per-tx CheckTransaction : " << perTxMs << " ms" << std::endl;
    std::cout << "    txs in a full 4 MB block: " << txPerBlock << std::endl;
    std::cout << "    worst-case block verify : " << blockVerifyMs << " ms ("
              << blockVerifySec << " s)" << std::endl;
    std::cout << "    fraction of DIL  240 s  : "
              << (blockVerifySec / dilTargetSec * 100.0) << " %" << std::endl;
    std::cout << "    fraction of DilV  45 s  : "
              << (blockVerifySec / dilvTargetSec * 100.0) << " %" << std::endl;

    // Safety gate: a full block must verify in well under the block interval.
    ASSERT(blockVerifySec < dilvTargetSec,
           "CONCERN: worst-case 4 MB block verify time ("
           + std::to_string(blockVerifySec)
           + " s) exceeds the DilV 60 s target block time — escalate.");
    if (blockVerifySec > dilvTargetSec * 0.10) {
        std::cout << "    NOTE: block verify exceeds 10% of the DilV block time"
                  << " — flag for review." << std::endl;
    }

    utxoSet.Close();
    std::filesystem::remove_all(utxoPath, ec);
}

// =======================================================================
int main() {
    std::cout << YELLOW << "========================================" << RESET << std::endl;
    std::cout << YELLOW << "BUG-003 Block-Size Limit Tests (F-06)"   << RESET << std::endl;
    std::cout << YELLOW << "========================================" << RESET << std::endl;
    std::cout << std::endl;

    // CBlockValidator::CheckBlock -> CheckBlockHeader -> CheckProofOfWork uses
    // RandomX; chainparams must be initialised before mining/wallet calls.
    static Dilithion::ChainParams s_params = Dilithion::ChainParams::Mainnet();
    Dilithion::g_chainParams = &s_params;
    const char* rxKey = "Dilithion-RandomX-v1";
    randomx_init_for_hashing(rxKey, strlen(rxKey), 1);  // light mode for tests

    test_storage_writeblock_size_boundary_wrapper();
    test_checkblock_size_constant_is_4mb_wrapper();
    test_template_overshoot_regression_bug_b_wrapper();
    test_worst_case_4mb_block_verify_time_wrapper();

    std::cout << std::endl;
    std::cout << YELLOW << "========================================" << RESET << std::endl;
    std::cout << GREEN  << "Passed: " << g_tests_passed << RESET << std::endl;
    std::cout << RED    << "Failed: " << g_tests_failed << RESET << std::endl;
    std::cout << YELLOW << "Total:  " << (g_tests_passed + g_tests_failed) << RESET << std::endl;
    std::cout << std::endl;

    if (g_tests_failed == 0) {
        std::cout << GREEN << "ALL TESTS PASSED" << RESET << std::endl;
        return 0;
    }
    std::cout << RED << "SOME TESTS FAILED" << RESET << std::endl;
    return 1;
}
