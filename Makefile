# Dilithion Cryptocurrency Makefile
# Copyright (c) 2025 The Dilithion Core developers
# Distributed under the MIT software license

# ============================================================================
# Configuration
# ============================================================================

# Version detection from git tags
export PATH := $(PATH):/c/Program Files/Git/cmd
GIT_VERSION := $(shell git describe --tags --abbrev=0 2>/dev/null || echo "dev")
GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE := $(shell date +%Y-%m-%d 2>/dev/null || echo "unknown")

# Detect operating system
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

# Compiler and flags
CXX := g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -pipe -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security
CXXFLAGS += -DDILITHION_VERSION='"$(GIT_VERSION)"'
CXXFLAGS += -MMD -MP
CFLAGS ?= -O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security
CFLAGS += -MMD -MP

# Include paths (base)
INCLUDES := -I components/node \
            -I components/wallet \
            -I components/miner \
            -I components/common \
            -I depends/randomx/src \
            -I depends/dilithium/ref \
            -I depends/chiavdf/src \
            -I depends/chiavdf/src/c_bindings \
            -I depends/libzmq/include

# Library paths and libraries (base)
LDFLAGS ?=

ifeq ($(UNAME_S),Windows)
    RANDOMX_BUILD_DIR := depends/randomx/build-windows
    LIBZMQ_BUILD_DIR := depends/libzmq/build-windows/lib
else ifneq (,$(findstring MINGW,$(UNAME_S)))
    RANDOMX_BUILD_DIR := depends/randomx/build-windows
    LIBZMQ_BUILD_DIR := depends/libzmq/build-windows/lib
else ifneq (,$(findstring MSYS,$(UNAME_S)))
    RANDOMX_BUILD_DIR := depends/randomx/build-windows
    LIBZMQ_BUILD_DIR := depends/libzmq/build-windows/lib
else
    RANDOMX_BUILD_DIR := depends/randomx/build
    LIBZMQ_BUILD_DIR := depends/libzmq/build/lib
endif

LDFLAGS += -L $(RANDOMX_BUILD_DIR) \
           -L $(LIBZMQ_BUILD_DIR) \
           -L depends/dilithium/ref \
           -L /mingw64/lib \
           -L C:/msys64/mingw64/lib \
           -L .

LIBS := -lrandomx -lzmq -lleveldb -lpthread -lssl -lcrypto -lminiupnpc -lgmpxx -lgmp

CXXFLAGS += -DZMQ_STATIC

ifeq ($(UNAME_S),Darwin)
    HOMEBREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
    INCLUDES += -I$(HOMEBREW_PREFIX)/opt/leveldb/include
    LDFLAGS += -L$(HOMEBREW_PREFIX)/opt/leveldb/lib
else ifeq ($(UNAME_S),Windows)
    LIBS += -lws2_32 -lbcrypt -ldbghelp -liphlpapi
    INCLUDES += -I depends/leveldb/include -I /mingw64/include -I C:/msys64/mingw64/include
else ifneq (,$(findstring MINGW,$(UNAME_S)))
    LIBS += -lws2_32 -lbcrypt -ldbghelp -liphlpapi
    INCLUDES += -I depends/leveldb/include -I /mingw64/include -I C:/msys64/mingw64/include
else ifneq (,$(findstring MSYS,$(UNAME_S)))
    LIBS += -lws2_32 -lbcrypt -ldbghelp -liphlpapi
    INCLUDES += -I depends/leveldb/include -I /mingw64/include -I C:/msys64/mingw64/include
endif

DILITHIUM_DIR := depends/dilithium/ref
DILITHIUM_SOURCES := $(DILITHIUM_DIR)/sign.c \
                     $(DILITHIUM_DIR)/packing.c \
                     $(DILITHIUM_DIR)/polyvec.c \
                     $(DILITHIUM_DIR)/poly.c \
                     $(DILITHIUM_DIR)/ntt.c \
                     $(DILITHIUM_DIR)/reduce.c \
                     $(DILITHIUM_DIR)/rounding.c \
                     $(DILITHIUM_DIR)/symmetric-shake.c \
                     $(DILITHIUM_DIR)/fips202.c \
                     $(DILITHIUM_DIR)/randombytes.c
DILITHIUM_OBJECTS := $(DILITHIUM_SOURCES:.c=.o)

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj

ifdef TSAN
    TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer
    CXXFLAGS += $(TSAN_FLAGS)
    CFLAGS += $(TSAN_FLAGS)
    LDFLAGS += $(TSAN_FLAGS)
    BUILD_DIR := build-tsan
    OBJ_DIR := $(BUILD_DIR)/obj
endif

COLOR_RESET := \033[0m
COLOR_GREEN := \033[32m
COLOR_BLUE := \033[34m
COLOR_YELLOW := \033[33m

# ============================================================================
# Source Files
# ============================================================================

CONSENSUS_SOURCES := components/node/consensus/fees.cpp \
                     components/node/consensus/pow.cpp \
                     components/node/consensus/chain.cpp \
                     components/node/consensus/reorg_wal.cpp \
                     components/node/consensus/chain_verifier.cpp \
                     components/node/consensus/tx_validation.cpp \
                     components/node/consensus/signature_batch_verifier.cpp \
                     components/node/consensus/validation.cpp \
                     components/node/consensus/vdf_validation.cpp \
                     components/node/consensus/port/chain_selector_impl.cpp

CORE_SOURCES_UTIL := components/node/core/chainparams.cpp \
                     components/node/core/globals.cpp \
                     components/node/core/node_context.cpp \
                     components/node/core/version.cpp

DB_SOURCES := components/node/db/db_errors.cpp

CRYPTO_SOURCES := components/common/crypto/randomx_hash.cpp \
                  components/common/crypto/sha3.cpp \
                  components/common/crypto/hmac_sha3.cpp \
                  components/common/crypto/pbkdf2_sha3.cpp \
                  components/common/crypto/siphash.cpp

INDEX_SOURCES := components/node/index/tx_index.cpp \
                 components/node/index/coinstatsindex.cpp \
                 components/node/kernel/coinstats.cpp

MINER_SOURCES := components/miner/miner/controller.cpp \
                 components/miner/miner/vdf_miner.cpp

DFMP_SOURCES := components/miner/dfmp/dfmp.cpp \
                components/miner/dfmp/identity_db.cpp \
                components/miner/dfmp/mik.cpp \
                components/miner/dfmp/mik_registration_file.cpp

DIGITAL_DNA_SOURCES := components/node/digital_dna/digital_dna.cpp \
                       components/node/digital_dna/latency_fingerprint.cpp \
                       components/node/digital_dna/timing_signature.cpp \
                       components/node/digital_dna/perspective_proof.cpp \
                       components/node/digital_dna/digital_dna_rpc.cpp \
                       components/node/digital_dna/behavioral_profile.cpp \
                       components/node/digital_dna/memory_fingerprint.cpp \
                       components/node/digital_dna/clock_drift.cpp \
                       components/node/digital_dna/bandwidth_proof.cpp \
                       components/node/digital_dna/ml_detector.cpp \
                       components/node/digital_dna/dna_registry_db.cpp \
                       components/node/digital_dna/trust_score.cpp \
                       components/node/digital_dna/dna_verification.cpp \
                       components/node/digital_dna/verification_manager.cpp \
                       components/node/digital_dna/sample_rate_limiter.cpp \
                       components/node/digital_dna/sample_envelope.cpp \
                       components/node/digital_dna/mik_pubkey_cache.cpp

VDF_SOURCES := components/node/vdf/vdf.cpp \
               components/node/vdf/cooldown_tracker.cpp

CHIAVDF_OBJECTS := $(OBJ_DIR)/chiavdf/c_wrapper.o $(OBJ_DIR)/chiavdf/lzcnt.o

NET_SOURCES := components/node/net/protocol.cpp \
               components/node/net/serialize.cpp \
               components/node/net/net.cpp \
               components/node/net/peer_discovery.cpp \
               components/node/net/peers.cpp \
               components/node/net/socket.cpp \
               components/node/net/dns.cpp \
               components/node/net/tx_relay.cpp \
               components/node/net/async_broadcaster.cpp \
               components/node/net/headers_manager.cpp \
               components/node/net/orphan_manager.cpp \
               components/node/net/block_fetcher.cpp \
               components/node/net/netaddress.cpp \
               components/node/net/addrman.cpp \
               components/node/net/port/addrman_v2.cpp \
               components/node/net/port/addrman_migrator.cpp \
               components/node/net/port/peer_scorer.cpp \
               components/node/net/port/sync_coordinator_adapter.cpp \
               components/node/net/banman.cpp \
               components/node/net/headerssync.cpp \
               components/node/net/blockencodings.cpp \
               components/node/net/feeler.cpp \
               components/node/net/bandwidth_throttle.cpp \
               components/node/net/connection_quality.cpp \
               components/node/net/partition_detector.cpp \
               components/node/net/connman.cpp \
               components/node/net/asn_database.cpp \
               components/node/net/node.cpp \
               components/node/net/sock.cpp \
               components/node/net/upnp.cpp \
               components/node/attestation/seed_attestation.cpp

NODE_SOURCES := components/node/node/block_index.cpp \
                components/node/node/blockchain_storage.cpp \
                components/node/node/block_processing.cpp \
                components/node/node/fork_manager.cpp \
                components/node/node/mempool.cpp \
                components/node/node/mempool_persist.cpp \
                components/node/node/genesis.cpp \
                components/node/node/utxo_set.cpp \
                components/node/node/ibd_coordinator.cpp \
                components/node/node/block_validation_queue.cpp \
                components/node/node/validation_watchdog.cpp \
                components/node/node/resource_monitor.cpp \
                components/node/node/peer_mik_tracker.cpp \
                components/node/node/registration_manager.cpp \
                components/node/node/startup_checkpoint_validator.cpp \
                components/node/node/chainstate_integrity_monitor.cpp

PRIMITIVES_SOURCES := components/common/primitives/block.cpp \
                      components/common/primitives/transaction.cpp

RPC_SOURCES := components/node/rpc/server.cpp \
               components/node/rpc/auth.cpp \
               components/node/rpc/ratelimiter.cpp \
               components/node/rpc/permissions.cpp \
               components/node/rpc/logger.cpp \
               components/node/rpc/ssl_wrapper.cpp \
               components/node/rpc/websocket.cpp \
               components/node/rpc/rest_api.cpp

API_SOURCES := components/node/api/http_server.cpp \
               components/node/api/cached_stats.cpp

X402_SOURCES := components/node/x402/x402_types.cpp \
                components/node/x402/vma.cpp \
                components/node/x402/facilitator.cpp

SCRIPT_SOURCES := components/node/script/interpreter.cpp \
                  components/node/script/htlc.cpp \
                  components/node/script/atomic_swap.cpp

ZMQ_SOURCES := components/node/zmq/zmqutil.cpp \
               components/node/zmq/zmqabstractnotifier.cpp \
               components/node/zmq/zmqpublishnotifier.cpp

POLICY_SOURCES := components/node/policy/fees.cpp \
                  components/node/policy/fee_persist.cpp

WALLET_SOURCES := components/wallet/wallet/wallet.cpp \
                  components/wallet/wallet/crypter.cpp \
                  components/wallet/wallet/passphrase_validator.cpp \
                  components/wallet/wallet/mnemonic.cpp \
                  components/wallet/wallet/hd_derivation.cpp \
                  components/wallet/wallet/wallet_manager.cpp \
                  components/wallet/wallet/wallet_manager_wizard.cpp \
                  components/wallet/wallet/wallet_init.cpp \
                  components/wallet/wallet/wal.cpp \
                  components/wallet/wallet/wal_recovery.cpp

UTIL_SOURCES := components/common/util/strencodings.cpp \
                components/common/util/stacktrace.cpp \
                components/common/util/base58.cpp \
                components/common/util/system.cpp \
                components/common/util/assert.cpp \
                components/common/util/logging.cpp \
                components/common/util/config.cpp \
                components/common/util/config_validator.cpp \
                components/common/util/error_format.cpp \
                components/common/util/bench.cpp \
                components/common/util/pidfile.cpp \
                components/common/util/chain_reset.cpp

CORE_SOURCES := $(CONSENSUS_SOURCES) \
                $(CORE_SOURCES_UTIL) \
                $(DB_SOURCES) \
                $(CRYPTO_SOURCES) \
                $(INDEX_SOURCES) \
                $(MINER_SOURCES) \
                $(DFMP_SOURCES) \
                $(DIGITAL_DNA_SOURCES) \
                $(VDF_SOURCES) \
                $(NET_SOURCES) \
                $(NODE_SOURCES) \
                $(PRIMITIVES_SOURCES) \
                $(RPC_SOURCES) \
                $(API_SOURCES) \
                $(X402_SOURCES) \
                $(SCRIPT_SOURCES) \
                $(POLICY_SOURCES) \
                $(ZMQ_SOURCES) \
                $(UTIL_SOURCES) \
                $(WALLET_SOURCES)

CORE_OBJECTS := $(CONSENSUS_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(CORE_SOURCES_UTIL:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(DB_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(CRYPTO_SOURCES:components/common/%.cpp=$(OBJ_DIR)/%.o) \
                $(INDEX_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(MINER_SOURCES:components/miner/%.cpp=$(OBJ_DIR)/%.o) \
                $(DFMP_SOURCES:components/miner/%.cpp=$(OBJ_DIR)/%.o) \
                $(DIGITAL_DNA_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(VDF_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(NET_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(NODE_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(PRIMITIVES_SOURCES:components/common/%.cpp=$(OBJ_DIR)/%.o) \
                $(RPC_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(API_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(X402_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(SCRIPT_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(POLICY_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(ZMQ_SOURCES:components/node/%.cpp=$(OBJ_DIR)/%.o) \
                $(UTIL_SOURCES:components/common/%.cpp=$(OBJ_DIR)/%.o) \
                $(WALLET_SOURCES:components/wallet/%.cpp=$(OBJ_DIR)/%.o)

DILITHION_NODE_SOURCE := components/node/node/dilithion-node.cpp
GENESIS_GEN_SOURCE := components/tests/test/genesis_test.cpp

all: dilithion-node dilv-node genesis_gen check-wallet-balance
	@echo "$(COLOR_GREEN)✓ Build complete!$(COLOR_RESET)"

dilithion-node: $(CORE_OBJECTS) $(OBJ_DIR)/node/dilithion-node.o $(DILITHIUM_OBJECTS) $(CHIAVDF_OBJECTS) | libzmq
	@echo "$(COLOR_BLUE)[LINK]$(COLOR_RESET) $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

dilv-node: $(CORE_OBJECTS) $(OBJ_DIR)/node/dilv-node.o $(DILITHIUM_OBJECTS) $(CHIAVDF_OBJECTS) | libzmq
	@echo "$(COLOR_BLUE)[LINK]$(COLOR_RESET) $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

genesis_gen: $(CORE_OBJECTS) $(OBJ_DIR)/test/genesis_test.o $(DILITHIUM_OBJECTS) $(CHIAVDF_OBJECTS)
	@echo "$(COLOR_BLUE)[LINK]$(COLOR_RESET) $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

inspect_db: $(CORE_OBJECTS) $(OBJ_DIR)/tools/inspect_db.o $(DILITHIUM_OBJECTS) $(CHIAVDF_OBJECTS)
	@echo "$(COLOR_BLUE)[LINK]$(COLOR_RESET) $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

leveldb_state_hash: $(CORE_OBJECTS) $(OBJ_DIR)/tools/leveldb_state_hash.o $(DILITHIUM_OBJECTS) $(CHIAVDF_OBJECTS)
	@echo "$(COLOR_BLUE)[LINK]$(COLOR_RESET) $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

check-wallet-balance: $(CORE_OBJECTS) $(OBJ_DIR)/wallet/check-wallet-balance.o $(DILITHIUM_OBJECTS) $(CHIAVDF_OBJECTS)
	@echo "$(COLOR_BLUE)[LINK]$(COLOR_RESET) $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

dilv-genesis-vdf: $(CORE_OBJECTS) $(OBJ_DIR)/tools/dilv_genesis_vdf.o $(DILITHIUM_OBJECTS) $(CHIAVDF_OBJECTS)
	@echo "$(COLOR_BLUE)[LINK]$(COLOR_RESET) $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

# Compilation rules
OBJ_DIR_TARGETS := $(OBJ_DIR)/attestation \
    $(OBJ_DIR)/consensus \
    $(OBJ_DIR)/consensus/port \
    $(OBJ_DIR)/core \
    $(OBJ_DIR)/crypto \
    $(OBJ_DIR)/db \
    $(OBJ_DIR)/dfmp \
    $(OBJ_DIR)/index \
    $(OBJ_DIR)/kernel \
    $(OBJ_DIR)/miner \
    $(OBJ_DIR)/net \
    $(OBJ_DIR)/net/port \
    $(OBJ_DIR)/node \
    $(OBJ_DIR)/primitives \
    $(OBJ_DIR)/rpc \
    $(OBJ_DIR)/wallet \
    $(OBJ_DIR)/util \
    $(OBJ_DIR)/api \
    $(OBJ_DIR)/vdf \
    $(OBJ_DIR)/digital_dna \
    $(OBJ_DIR)/script \
    $(OBJ_DIR)/policy \
    $(OBJ_DIR)/tools \
    $(OBJ_DIR)/x402 \
    $(OBJ_DIR)/zmq \
    $(OBJ_DIR)/test

$(OBJ_DIR_TARGETS):
	@mkdir -p $@

$(OBJ_DIR)/%.o: components/node/%.cpp | $(OBJ_DIR_TARGETS)
	@echo "$(COLOR_BLUE)[CXX]$(COLOR_RESET)  $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%.o: components/wallet/%.cpp | $(OBJ_DIR_TARGETS)
	@echo "$(COLOR_BLUE)[CXX]$(COLOR_RESET)  $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%.o: components/miner/%.cpp | $(OBJ_DIR_TARGETS)
	@echo "$(COLOR_BLUE)[CXX]$(COLOR_RESET)  $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%.o: components/common/%.cpp | $(OBJ_DIR_TARGETS)
	@echo "$(COLOR_BLUE)[CXX]$(COLOR_RESET)  $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%.o: components/tests/%.cpp | $(OBJ_DIR_TARGETS)
	@echo "$(COLOR_BLUE)[CXX]$(COLOR_RESET)  $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/chiavdf/c_wrapper.o: depends/chiavdf/src/c_bindings/c_wrapper.cpp
	@mkdir -p $(OBJ_DIR)/chiavdf
	@$(CXX) -std=c++17 -O2 -pipe -w $(CPPFLAGS) -I depends/chiavdf/src -I depends/chiavdf/src/c_bindings -c $< -o $@

$(OBJ_DIR)/chiavdf/lzcnt.o: depends/chiavdf/src/refcode/lzcnt.c
	@mkdir -p $(OBJ_DIR)/chiavdf
	@gcc $(CFLAGS) -w -c $< -o $@

$(DILITHIUM_DIR)/%.o: $(DILITHIUM_DIR)/%.c
	@gcc $(CFLAGS) -DDILITHIUM_MODE=3 -I $(DILITHIUM_DIR) -c $< -o $@

libzmq:
	@if [ ! -f $(LIBZMQ_BUILD_DIR)/libzmq.a ]; then \
		cd depends/libzmq && mkdir -p build && cd build && \
		cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=OFF -DBUILD_STATIC=ON -DBUILD_TESTS=OFF -DENABLE_CPACK=OFF -DENABLE_DRAFTS=OFF -DWITH_DOC=OFF -DWITH_DOCS=OFF -DWITH_LIBSODIUM=OFF -DZMQ_BUILD_TESTS=OFF .. && \
		$(MAKE) -j4; \
	fi

clean:
	@rm -rf $(BUILD_DIR)
	@rm -f dilithion-node dilv-node genesis_gen check-wallet-balance

.PHONY: all clean libzmq
