use axum::{
    extract::{Query, State},
    http::StatusCode,
    response::{IntoResponse, Json},
    routing::get,
    Router,
};
use base64::Engine;
use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::Arc;
use tower_http::cors::CorsLayer;
use tower_http::services::ServeDir;

#[derive(Clone)]
struct AppState {
    rpc_configs: HashMap<String, RpcConfig>,
}

#[derive(Clone)]
struct RpcConfig {
    url: String,
    auth: String,
    reward: f64,
}

#[derive(Deserialize)]
struct ChainQuery {
    chain: Option<String>,
}

#[derive(Deserialize)]
struct BlocksQuery {
    chain: Option<String>,
    hash: Option<String>,
    height: Option<u64>,
    page: Option<usize>,
    limit: Option<usize>,
    verbosity: Option<i32>,
    shape: Option<String>,
}

#[derive(Deserialize)]
struct TxQuery {
    chain: Option<String>,
    txid: Option<String>,
    shape: Option<String>,
}

#[derive(Deserialize)]
struct TransactionsQuery {
    chain: Option<String>,
    page: Option<usize>,
    limit: Option<usize>,
    #[allow(dead_code)]
    shape: Option<String>,
}

#[derive(Deserialize)]
struct AddressQuery {
    chain: Option<String>,
    address: String,
    #[allow(dead_code)]
    page: Option<usize>,
    #[allow(dead_code)]
    limit: Option<usize>,
}

#[derive(Deserialize)]
struct SearchQuery {
    chain: Option<String>,
    q: String,
}

#[tokio::main]
async fn main() {
    let mut rpc_configs = HashMap::new();
    rpc_configs.insert(
        "dil".to_string(),
        RpcConfig {
            url: "http://127.0.0.1:8332/".to_string(),
            auth: format!(
                "Basic {}",
                base64::engine::general_purpose::STANDARD.encode("rpc:rpc")
            ),
            reward: 50.0,
        },
    );
    rpc_configs.insert(
        "dilv".to_string(),
        RpcConfig {
            url: "http://127.0.0.1:9332/".to_string(),
            auth: format!(
                "Basic {}",
                base64::engine::general_purpose::STANDARD.encode("rpc:rpc")
            ),
            reward: 100.0,
        },
    );

    let state = Arc::new(AppState { rpc_configs });

    let api_routes = Router::new()
        .route("/blocks.php", get(get_blocks))
        .route("/transactions.php", get(get_transactions))
        .route("/tx.php", get(get_tx))
        .route("/address.php", get(get_address))
        .route("/stats.php", get(get_stats))
        .route("/search.php", get(get_search))
        .route("/holders.php", get(get_holders))
        .route("/nodes.php", get(get_nodes))
        .route("/mempool.php", get(get_mempool))
        .route("/supply.php", get(get_supply))
        .with_state(state);

    let app = Router::new()
        .nest("/api", api_routes)
        .fallback_service(ServeDir::new("static"))
        .layer(CorsLayer::permissive());

    let addr = "0.0.0.0:3000";
    let listener = tokio::net::TcpListener::bind(addr).await.unwrap();
    println!("Listening on http://{}", addr);
    axum::serve(listener, app).await.unwrap();
}

async fn call_rpc(
    config: &RpcConfig,
    method: &str,
    params: Value,
) -> Result<Value, (StatusCode, String)> {
    let client = reqwest::Client::new();
    let payload = json!({
        "jsonrpc": "2.0",
        "id": 1,
        "method": method,
        "params": params
    });

    let response = client
        .post(&config.url)
        .header("Authorization", &config.auth)
        .header("Content-Type", "application/json")
        .json(&payload)
        .send()
        .await
        .map_err(|e| (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?;

    if response.status() != StatusCode::OK {
        return Err((
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("Node returned status {}", response.status()),
        ));
    }

    let data: Value = response
        .json()
        .await
        .map_err(|e| (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?;

    if let Some(error) = data.get("error") {
        if !error.is_null() {
            return Err((StatusCode::INTERNAL_SERVER_ERROR, error.to_string()));
        }
    }

    Ok(data["result"].clone())
}

fn get_chain_config<'a>(
    state: &'a AppState,
    chain: Option<&String>,
) -> Result<(&'a String, &'a RpcConfig), (StatusCode, String)> {
    let chain_name = chain.map(|s| s.as_str()).unwrap_or("dil");
    let chain_name = if chain_name == "dilv" { "dilv" } else { "dil" };
    state
        .rpc_configs
        .get_key_value(chain_name)
        .map(|(k, v)| (k, v))
        .ok_or((StatusCode::BAD_REQUEST, "Invalid chain".to_string()))
}

// --- Transformers ---

fn transform_block_v0(block: Value, reward_base: f64) -> Value {
    let h = block["height"].as_u64().unwrap_or(0);
    let epoch = h / 210000;
    let subsidy = if epoch >= 64 {
        0.0
    } else {
        reward_base / (1u64 << epoch) as f64
    };

    let mut reward_subunits = 0.0;
    let mut miner_address = String::new();
    let mut coinbase_txid = String::new();
    let mut first_non_coinbase_txid = Value::Null;
    let mut tx_count = 0;

    if let Some(txs) = block["tx"].as_array() {
        tx_count = txs.len();
        if tx_count > 0 {
            let cb = &txs[0];
            if let Some(txid) = cb.as_str() {
                coinbase_txid = txid.to_string();
            } else {
                coinbase_txid = cb["txid"].as_str().unwrap_or("").to_string();
                if let Some(vouts) = cb["vout"].as_array() {
                    for vout in vouts {
                        reward_subunits += vout["value"].as_f64().unwrap_or(0.0);
                        if miner_address.is_empty() {
                            if let Some(addr) = vout["address"].as_str() {
                                miner_address = addr.to_string();
                            }
                        }
                    }
                }
            }
            if tx_count > 1 {
                let first = &txs[1];
                first_non_coinbase_txid = if let Some(txid) = first.as_str() {
                    json!(txid)
                } else {
                    first["txid"].clone()
                };
            }
        }
    }

    let reward_dil = reward_subunits / 100000000.0;
    let fees_dil = (reward_dil - subsidy).max(0.0);

    json!({
        "height": h,
        "hash": block["hash"],
        "timestamp": block["time"],
        "size": block["size"],
        "txCount": tx_count,
        "difficulty": block["difficulty"],
        "reward": reward_dil,
        "fees": fees_dil,
        "miner": miner_address,
        "minerAddress": miner_address,
        "coinbaseTxid": coinbase_txid,
        "firstNonCoinbaseTxid": first_non_coinbase_txid,
        "mik": block["mik"],
        "previousBlockHash": block["previousblockhash"],
    })
}

fn transform_tx_v0(tx: Value) -> Value {
    let mut is_coinbase = false;
    let mut total_out_subunits = 0.0;
    let mut to = String::new();

    if let Some(vins) = tx["vin"].as_array() {
        for vin in vins {
            if vin.get("coinbase").is_some() {
                is_coinbase = true;
                break;
            }
        }
    }

    if let Some(vouts) = tx["vout"].as_array() {
        for vout in vouts {
            total_out_subunits += vout["value"].as_f64().unwrap_or(0.0);
            if to.is_empty() {
                if let Some(addr) = vout["address"].as_str() {
                    to = addr.to_string();
                }
            }
        }
    }

    json!({
        "id": tx["txid"],
        "from": if is_coinbase { json!({"address": "coinbase"}) } else { Value::Null },
        "to": to,
        "amount": total_out_subunits / 100000000.0,
        "fee": 0.0,
        "timestamp": tx["blocktime"],
        "blockHeight": tx["blockheight"],
        "kind": if is_coinbase { "coinbase" } else { "transfer" },
        "confirmations": tx["confirmations"],
    })
}

// --- Handlers ---

async fn get_blocks(
    State(state): State<Arc<AppState>>,
    Query(query): Query<BlocksQuery>,
) -> impl IntoResponse {
    let (chain_name, config) = match get_chain_config(&state, query.chain.as_ref()) {
        Ok(res) => res,
        Err(e) => return e.into_response(),
    };

    let verbosity = query.verbosity.unwrap_or(1);
    let shape = query.shape.as_deref().unwrap_or("");

    if let Some(hash) = query.hash {
        match call_rpc(config, "getblock", json!([hash, verbosity])).await {
            Ok(block) => {
                if shape == "v0" {
                    return Json(json!({"block": transform_block_v0(block, config.reward)}))
                        .into_response();
                }
                return Json(json!({ "block": block })).into_response();
            }
            Err(e) => return e.into_response(),
        }
    }

    if let Some(height) = query.height {
        match call_rpc(config, "getblockhash", json!([height])).await {
            Ok(hash) => {
                match call_rpc(config, "getblock", json!([hash, verbosity])).await {
                    Ok(block) => {
                        if shape == "v0" {
                            return Json(json!({"block": transform_block_v0(block, config.reward)}))
                                .into_response();
                        }
                        return Json(json!({ "block": block })).into_response();
                    }
                    Err(e) => return e.into_response(),
                }
            }
            Err(e) => return e.into_response(),
        }
    }

    let page = query.page.unwrap_or(1).max(1);
    let limit = query.limit.unwrap_or(20).min(50).max(1);

    match call_rpc(config, "getblockcount", json!([])).await {
        Ok(tip_height_val) => {
            let tip_height = tip_height_val.as_u64().unwrap_or(0);
            let start_height = if tip_height >= ((page - 1) * limit) as u64 {
                tip_height - ((page - 1) * limit) as u64
            } else {
                0
            };

            let mut blocks = Vec::new();
            match call_rpc(config, "getblockhash", json!([start_height])).await {
                Ok(mut current_hash) => {
                    for _ in 0..limit {
                        match call_rpc(config, "getblock", json!([current_hash, verbosity])).await {
                            Ok(block) => {
                                let prev_hash = block["previousblockhash"].clone();
                                if shape == "v0" {
                                    blocks.push(transform_block_v0(block, config.reward));
                                } else {
                                    blocks.push(block);
                                }
                                if prev_hash.is_null() {
                                    break;
                                }
                                current_hash = prev_hash;
                            }
                            Err(_) => break,
                        }
                    }
                }
                Err(e) => return e.into_response(),
            }

            Json(json!({
                "blocks": blocks,
                "page": page,
                "limit": limit,
                "totalHeight": tip_height,
                "chain": chain_name,
                "cached": false
            }))
            .into_response()
        }
        Err(e) => e.into_response(),
    }
}

async fn get_transactions(
    State(state): State<Arc<AppState>>,
    Query(query): Query<TransactionsQuery>,
) -> impl IntoResponse {
    let (chain_name, _config) = match get_chain_config(&state, query.chain.as_ref()) {
        Ok(res) => res,
        Err(e) => return e.into_response(),
    };

    Json(json!({
        "transactions": [],
        "page": query.page.unwrap_or(1),
        "limit": query.limit.unwrap_or(20),
        "chain": chain_name
    }))
    .into_response()
}

async fn get_tx(
    State(state): State<Arc<AppState>>,
    Query(query): Query<TxQuery>,
) -> impl IntoResponse {
    let (_chain_name, config) = match get_chain_config(&state, query.chain.as_ref()) {
        Ok(res) => res,
        Err(e) => return e.into_response(),
    };

    let txid = match query.txid {
        Some(id) => id,
        None => return (StatusCode::BAD_REQUEST, "Missing txid").into_response(),
    };

    match call_rpc(config, "getrawtransaction", json!([txid, 1])).await {
        Ok(tx) => {
            if query.shape.as_deref() == Some("v0") {
                return Json(json!({ "transaction": transform_tx_v0(tx) })).into_response();
            }
            Json(json!({ "transaction": tx })).into_response()
        }
        Err(e) => e.into_response(),
    }
}

async fn get_address(
    State(state): State<Arc<AppState>>,
    Query(query): Query<AddressQuery>,
) -> impl IntoResponse {
    let (_chain_name, config) = match get_chain_config(&state, query.chain.as_ref()) {
        Ok(res) => res,
        Err(e) => return e.into_response(),
    };

    match call_rpc(config, "getaddressinfo", json!([query.address])).await {
        Ok(info) => Json(json!({ "address": query.address, "info": info })).into_response(),
        Err(e) => e.into_response(),
    }
}

async fn get_stats(
    State(state): State<Arc<AppState>>,
    Query(query): Query<ChainQuery>,
) -> impl IntoResponse {
    let (chain_name, config) = match get_chain_config(&state, query.chain.as_ref()) {
        Ok(res) => res,
        Err(e) => return e.into_response(),
    };

    let blockchain_info = match call_rpc(config, "getblockchaininfo", json!([])).await {
        Ok(info) => info,
        Err(e) => return e.into_response(),
    };

    let height = blockchain_info["blocks"].as_u64().unwrap_or(0);
    let difficulty = blockchain_info["difficulty"].as_f64().unwrap_or(0.0);

    let mut supply = height as f64 * config.reward;
    if chain_name == "dilv" {
        supply += 2681636.92;
    }

    Json(json!({
        "blocks": height,
        "difficulty": difficulty,
        "supply": supply,
        "chain": chain_name,
        "unit": if chain_name == "dilv" { "DilV" } else { "DIL" },
        "chainName": if chain_name == "dilv" { "DilV" } else { "Dilithion" },
        "consensusType": if chain_name == "dilv" { "VDF" } else { "RandomX" },
    }))
    .into_response()
}

async fn get_search(
    State(state): State<Arc<AppState>>,
    Query(query): Query<SearchQuery>,
) -> impl IntoResponse {
    let (_chain_name, config) = match get_chain_config(&state, query.chain.as_ref()) {
        Ok(res) => res,
        Err(e) => return e.into_response(),
    };

    let q = query.q.trim();

    if let Ok(height) = q.parse::<u64>() {
        return Json(json!({ "type": "block", "url": format!("#/block/{}", height) }))
            .into_response();
    }

    if q.len() == 64 && q.chars().all(|c| c.is_ascii_hexdigit()) {
        if call_rpc(config, "getblock", json!([q, 0])).await.is_ok() {
            return Json(json!({ "type": "block", "url": format!("#/block/{}", q) }))
                .into_response();
        }
        if call_rpc(config, "getrawtransaction", json!([q, 0]))
            .await
            .is_ok()
        {
            return Json(json!({ "type": "tx", "url": format!("#/tx/{}", q) })).into_response();
        }
    }

    if q.len() >= 30 && q.len() <= 40 {
        return Json(json!({ "type": "address", "url": format!("#/address/{}", q) }))
            .into_response();
    }

    (StatusCode::NOT_FOUND, "Not found").into_response()
}

async fn get_holders() -> impl IntoResponse {
    Json(json!({ "holders": [] }))
}

async fn get_nodes() -> impl IntoResponse {
    Json(json!({ "nodesOnline": 0 }))
}

async fn get_mempool() -> impl IntoResponse {
    Json(json!({ "size": 0 }))
}

async fn get_supply() -> impl IntoResponse {
    Json(json!({ "supply": 0 }))
}
