use axum::{
    extract::State,
    http::StatusCode,
    response::{IntoResponse, Json},
    routing::get,
    Router,
};
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::collections::HashMap;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use tower_http::cors::CorsLayer;
use tower_http::services::ServeDir;

#[derive(Debug, Serialize, Deserialize, Clone)]
#[serde(rename_all = "camelCase")]
struct NodeStats {
    online: bool,
    block_height: u64,
    peer_count: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    hashrate: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    difficulty: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    total_supply: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    block_reward: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    blocks_until_halving: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    last_block_time: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

#[derive(Debug, Serialize, Clone)]
#[serde(rename_all = "camelCase")]
struct StatsResponse {
    status: String,
    block_height: u64,
    peer_count: u64,
    timestamp: u64,
    nodes: HashMap<String, NodeStats>,
    #[serde(skip_serializing_if = "Option::is_none")]
    network_hash_rate: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    hash_rate: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    difficulty: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    total_supply: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    block_reward: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    blocks_until_halving: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    last_block_time: Option<u64>,
}

struct AppState {
    client: reqwest::Client,
    seed_nodes: HashMap<String, SeedNode>,
}

#[derive(Clone)]
struct SeedNode {
    #[allow(dead_code)]
    name: String,
    ip: String,
    api_port: u16,
}

#[tokio::main]
async fn main() {
    let mut seed_nodes = HashMap::new();
    seed_nodes.insert(
        "nyc".to_string(),
        SeedNode {
            name: "NYC (Primary)".to_string(),
            ip: "138.197.68.128".to_string(),
            api_port: 8334,
        },
    );
    seed_nodes.insert(
        "ldn".to_string(),
        SeedNode {
            name: "London".to_string(),
            ip: "167.172.56.119".to_string(),
            api_port: 8334,
        },
    );
    seed_nodes.insert(
        "sgp".to_string(),
        SeedNode {
            name: "Singapore".to_string(),
            ip: "165.22.103.114".to_string(),
            api_port: 8334,
        },
    );
    seed_nodes.insert(
        "syd".to_string(),
        SeedNode {
            name: "Sydney".to_string(),
            ip: "134.199.159.83".to_string(),
            api_port: 8334,
        },
    );

    let state = Arc::new(AppState {
        client: reqwest::Client::builder()
            .timeout(std::time::Duration::from_secs(3))
            .connect_timeout(std::time::Duration::from_secs(2))
            .build()
            .unwrap(),
        seed_nodes,
    });

    let app = Router::new()
        .route("/api/stats.php", get(get_stats))
        .route("/api/stats", get(get_stats))
        .fallback_service(ServeDir::new("static"))
        .layer(CorsLayer::permissive())
        .with_state(state);

    let addr = "0.0.0.0:3001";
    let listener = tokio::net::TcpListener::bind(addr).await.unwrap();
    println!("Listening on http://{}", addr);
    axum::serve(listener, app).await.unwrap();
}

async fn get_stats(State(state): State<Arc<AppState>>) -> impl IntoResponse {
    let mut futures = Vec::new();
    
    for (node_id, config) in &state.seed_nodes {
        let client = state.client.clone();
        let node_id = node_id.clone();
        let ip = config.ip.clone();
        let port = config.api_port;
        
        futures.push(tokio::spawn(async move {
            let stats = fetch_node_stats(&client, &ip, port).await;
            (node_id, stats)
        }));
    }

    let results = futures::future::join_all(futures).await;
    
    let mut nodes = HashMap::new();
    let mut max_block_height = 0;
    let mut total_peers = 0;
    let mut network_online = false;

    for res in results {
        if let Ok((node_id, node_stats)) = res {
            if node_stats.online {
                network_online = true;
                if node_stats.block_height > max_block_height {
                    max_block_height = node_stats.block_height;
                }
                total_peers += node_stats.peer_count;
            }
            nodes.insert(node_id, node_stats);
        }
    }

    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs();

    let mut response = StatsResponse {
        status: if network_online { "live" } else { "offline" }.to_string(),
        block_height: max_block_height,
        peer_count: total_peers,
        timestamp,
        nodes: nodes.clone(),
        network_hash_rate: None,
        hash_rate: None,
        difficulty: None,
        total_supply: None,
        block_reward: None,
        blocks_until_halving: None,
        last_block_time: None,
    };

    // Include additional info from first online node
    // To match PHP behavior, we can try to find the same node or just the first online one
    let mut node_keys: Vec<_> = nodes.keys().collect();
    node_keys.sort(); // Consistent order
    
    for key in node_keys {
        let node_data = &nodes[key];
        if node_data.online {
            if let Some(hr) = node_data.hashrate {
                if hr > 0.0 {
                    response.network_hash_rate = Some(hr);
                    response.hash_rate = Some(hr);
                }
            }
            if let Some(diff) = node_data.difficulty {
                if diff > 0.0 {
                    response.difficulty = Some(diff);
                }
            }
            if let Some(supply) = node_data.total_supply {
                if supply > 0.0 {
                    response.total_supply = Some(supply);
                }
            }
            if let Some(reward) = node_data.block_reward {
                response.block_reward = Some(reward);
            }
            if let Some(halving) = node_data.blocks_until_halving {
                response.blocks_until_halving = Some(halving);
            }
            if let Some(lbt) = node_data.last_block_time {
                if lbt > 0 {
                    response.last_block_time = Some(lbt);
                }
            }
            break;
        }
    }

    Json(response)
}

async fn fetch_node_stats(client: &reqwest::Client, ip: &str, port: u16) -> NodeStats {
    let url = format!("http://{}:{}/api/stats", ip, port);
    
    match client.get(&url).send().await {
        Ok(resp) => {
            if resp.status().is_success() {
                if let Ok(data) = resp.json::<serde_json::Value>().await {
                    return NodeStats {
                        online: true,
                        block_height: data["blockHeight"].as_u64().unwrap_or(0),
                        peer_count: data["peerCount"].as_u64().unwrap_or(0),
                        hashrate: data["networkHashRate"]
                            .as_f64()
                            .or_else(|| data["hashRate"].as_f64())
                            .or_else(|| data["hashrate"].as_f64()),
                        difficulty: data["difficulty"].as_f64(),
                        total_supply: data["totalSupply"].as_f64(),
                        block_reward: data["blockReward"].as_f64().or(Some(50.0)),
                        blocks_until_halving: data["blocksUntilHalving"].as_u64().or(Some(210000)),
                        last_block_time: data["lastBlockTime"].as_u64(),
                        error: None,
                    };
                }
            }
            NodeStats {
                online: false,
                block_height: 0,
                peer_count: 0,
                hashrate: None,
                difficulty: None,
                total_supply: None,
                block_reward: None,
                blocks_until_halving: None,
                last_block_time: None,
                error: Some("Failed to parse response".to_string()),
            }
        }
        Err(e) => NodeStats {
            online: false,
            block_height: 0,
            peer_count: 0,
            hashrate: None,
            difficulty: None,
            total_supply: None,
            block_reward: None,
            blocks_until_halving: None,
            last_block_time: None,
            error: Some(e.to_string()),
        },
    }
}
