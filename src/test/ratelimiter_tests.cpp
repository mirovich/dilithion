// Copyright (c) 2026 The Dilithion Core developers
// Distributed under the MIT software license
//
// Regression tests for CRateLimiter — added 2026-05-22 per red-team F-05
// during v4.4.3 hotfix work. The fix changed two surfaces:
//   1. Auth credential cache (commit a) — covered by rpc_auth_tests + live
//      behavioral checks.
//   2. Per-IP rate limit exemption for seed-mesh IPs (commit b) — this file.
//
// What we lock in:
//   - 127.0.0.1 / ::1 stay exempt (no regression in localhost behavior).
//   - Seed-mesh IPs are exempt at the PER-IP layer (AllowRequest) — read
//     from chainparams.seedAttestationIPs.
//   - Seed-mesh IPs are NOT exempt at the PER-METHOD layer (AllowMethodRequest)
//     — defense-in-depth against recycled-IP attack on wallet-tier methods.
//   - Non-seed, non-localhost IPs drain after the 10-token burst capacity.
//   - Non-seed IPs recover after a refill window.
//
// Per memory/feedback_fold_dont_defer_test_infra.md, the test infra was
// folded into the same PR as the production fix rather than queued.

#include <rpc/ratelimiter.h>
#include <core/chainparams.h>

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

// Minimal in-memory ChainParams shim for tests. The production
// g_chainParams is normally initialized by InitChainParams at node
// startup; tests need to populate seedAttestationIPs without dragging in
// the full chainparams init path. We allocate a ChainParams on the heap
// and point g_chainParams at it for the duration of the test.
struct TestParamsHandle {
    Dilithion::ChainParams* m_owned;
    Dilithion::ChainParams* m_prev;

    TestParamsHandle(std::vector<std::string> seedIPs) {
        m_owned = new Dilithion::ChainParams();
        m_owned->seedAttestationIPs = std::move(seedIPs);
        m_prev = Dilithion::g_chainParams;
        Dilithion::g_chainParams = m_owned;
    }
    ~TestParamsHandle() {
        Dilithion::g_chainParams = m_prev;
        delete m_owned;
    }
};

const std::string SEED_NYC = "138.197.68.128";
const std::string SEED_LDN = "167.172.56.119";
const std::string SEED_SGP = "165.22.103.114";
const std::string SEED_SYD = "134.199.159.83";
const std::string OUTSIDER = "203.0.113.42";  // RFC 5737 TEST-NET-3

} // namespace

void TestLocalhostAlwaysExempt() {
    std::cout << "Testing 127.0.0.1 / ::1 stay exempt..." << std::endl;
    TestParamsHandle params({SEED_NYC, SEED_LDN, SEED_SGP, SEED_SYD});

    CRateLimiter limiter;
    // 100 rapid calls — would drain the 10-token bucket twice over for a
    // non-exempt IP. Exempt IPs must never return false.
    for (int i = 0; i < 100; i++) {
        assert(limiter.AllowRequest("127.0.0.1"));
        assert(limiter.AllowRequest("::1"));
    }
    std::cout << "  ✓ 127.0.0.1 and ::1 never rate-limited" << std::endl;
}

void TestSeedMeshIPsExemptInAllowRequest() {
    std::cout << "\nTesting seed-mesh IPs exempt from per-IP token bucket..." << std::endl;
    TestParamsHandle params({SEED_NYC, SEED_LDN, SEED_SGP, SEED_SYD});

    CRateLimiter limiter;
    for (const auto& seedIP : {SEED_NYC, SEED_LDN, SEED_SGP, SEED_SYD}) {
        for (int i = 0; i < 50; i++) {
            assert(limiter.AllowRequest(seedIP));
        }
    }
    std::cout << "  ✓ All 4 seed IPs from chainparams exempt at AllowRequest layer" << std::endl;
}

void TestNonSeedIPDrains() {
    std::cout << "\nTesting non-seed IP drains after 10-burst capacity..." << std::endl;
    TestParamsHandle params({SEED_NYC, SEED_LDN, SEED_SGP, SEED_SYD});

    CRateLimiter limiter;

    // Bucket capacity is 10; refill 1/sec. Burst 10 requests fast → all
    // should pass, the 11th should fail (no time for any refill in a tight
    // loop).
    int allowed = 0;
    for (int i = 0; i < 11; i++) {
        if (limiter.AllowRequest(OUTSIDER)) allowed++;
    }
    assert(allowed >= 10 && allowed <= 11);
    // 11th call could squeak through if a millisecond of refill happened
    // between calls (very tight margin). The invariant we care about: not
    // all 11 pass when sustained — verify by a 12th call after no sleep.
    bool eleventh_or_twelfth_blocked = false;
    for (int i = 0; i < 3 && !eleventh_or_twelfth_blocked; i++) {
        if (!limiter.AllowRequest(OUTSIDER)) eleventh_or_twelfth_blocked = true;
    }
    assert(eleventh_or_twelfth_blocked);
    std::cout << "  ✓ Outsider IP (" << OUTSIDER << ") rate-limited after burst" << std::endl;
}

void TestNonSeedIPRefills() {
    std::cout << "\nTesting non-seed IP recovers after refill window..." << std::endl;
    TestParamsHandle params({SEED_NYC, SEED_LDN, SEED_SGP, SEED_SYD});

    CRateLimiter limiter;
    const std::string ip = "198.51.100.7";  // RFC 5737 TEST-NET-2

    // Drain the bucket.
    while (limiter.AllowRequest(ip)) { /* drain */ }

    // Sleep ~1.2 seconds — should refill at least 1 token.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    assert(limiter.AllowRequest(ip));
    std::cout << "  ✓ Drained outsider IP recovers after refill interval" << std::endl;
}

void TestSeedMeshNOTExemptPerMethod() {
    std::cout << "\nTesting seed-mesh IPs are NOT exempt at per-method layer..." << std::endl;
    TestParamsHandle params({SEED_NYC, SEED_LDN, SEED_SGP, SEED_SYD});

    CRateLimiter limiter;
    // 'walletpassphrase' has a strict per-method limit. Drain it from a
    // seed IP; it MUST eventually rate-limit. If seed IPs were exempt at
    // the per-method layer, this loop would never terminate.
    int allowed = 0;
    for (int i = 0; i < 200 && limiter.AllowMethodRequest(SEED_NYC, "walletpassphrase"); i++) {
        allowed++;
    }
    assert(allowed < 200);  // must hit the limit
    bool blocked = !limiter.AllowMethodRequest(SEED_NYC, "walletpassphrase");
    assert(blocked);
    std::cout << "  ✓ Seed IP " << SEED_NYC << " rate-limited on walletpassphrase ("
              << "allowed " << allowed << " then blocked) — recycled-IP wallet "
              << "attack surface stays bounded" << std::endl;
}

void TestEmptyChainparamsFailsClosed() {
    std::cout << "\nTesting g_chainParams=nullptr falls closed (no IP exempt)..." << std::endl;
    // Clear g_chainParams to simulate pre-init state. IsSeedMeshIP must
    // return false → no over-grant.
    Dilithion::ChainParams* saved = Dilithion::g_chainParams;
    Dilithion::g_chainParams = nullptr;

    CRateLimiter limiter;
    // Drain a "seed" IP — under nullptr params, it's no longer exempt,
    // so the 10-token bucket must drain.
    int allowed = 0;
    for (int i = 0; i < 15 && limiter.AllowRequest(SEED_NYC); i++) {
        allowed++;
    }

    Dilithion::g_chainParams = saved;
    assert(allowed <= 11);
    std::cout << "  ✓ Missing chainparams → no IP gets exempt status" << std::endl;
}

void TestEmptyIPRejected() {
    std::cout << "\nTesting empty IP string is not exempt..." << std::endl;
    TestParamsHandle params({SEED_NYC, SEED_LDN, SEED_SGP, SEED_SYD});

    CRateLimiter limiter;
    int allowed = 0;
    for (int i = 0; i < 15 && limiter.AllowRequest(""); i++) {
        allowed++;
    }
    assert(allowed <= 11);
    std::cout << "  ✓ Empty IP string drained by bucket like any other non-exempt source" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "CRateLimiter Regression Tests (v4.4.3)" << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        TestLocalhostAlwaysExempt();
        TestSeedMeshIPsExemptInAllowRequest();
        TestNonSeedIPDrains();
        TestNonSeedIPRefills();
        TestSeedMeshNOTExemptPerMethod();
        TestEmptyChainparamsFailsClosed();
        TestEmptyIPRejected();

        std::cout << "\n========================================" << std::endl;
        std::cout << "All CRateLimiter regression tests PASSED" << std::endl;
        std::cout << "========================================" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed: " << e.what() << std::endl;
        return 1;
    }
}
