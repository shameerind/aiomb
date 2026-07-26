#pragma once

/*
 * model_client.h — Client interface for the AI-OMB inference sidecar.
 *
 * All functions return 0 on success.  On failure (model server down,
 * parse error, or the model signals "fallback") they return -1 and
 * the caller should use its default/hardcoded logic.
 */

#define MODEL_SOCKET_DEFAULT "/run/mrepod/model.sock"
#define MODEL_MAX_SERVERS    16

/* ---- Server Selection ---- */

struct model_server_result {
    int  server_index;                     /* chosen server (0..N-1) */
    double q_values[MODEL_MAX_SERVERS];    /* Q-value per server     */
    double health_scores[MODEL_MAX_SERVERS];
    int  num_servers;
};

/*
 * Ask the inference server to choose the best NFS server.
 *
 * telemetry       : num_servers × 11 flattened array (row-major)
 * predicted_load  : num_servers floats
 * connections     : num_servers floats (current active mounts)
 * policy_index    : which mount policy type (0-based)
 * num_servers     : number of NFS servers
 * num_policies    : number of policy types (default 6)
 * result          : populated on success
 *
 * Returns 0 on success, -1 on failure (caller should fall back).
 */
int model_select_server(const char *sock_path,
                        const double *telemetry,
                        const double *predicted_load,
                        const double *connections,
                        int policy_index,
                        int num_servers,
                        int num_policies,
                        struct model_server_result *result);

/* ---- Anomaly Detection ---- */

struct model_anomaly_entry {
    int    server_index;
    double error;
    double threshold;
    int    anomalous;    /* 1 = anomalous, 0 = normal */
};

struct model_anomaly_result {
    int any_anomalous;
    int count;
    struct model_anomaly_entry servers[MODEL_MAX_SERVERS];
};

/*
 * Check NFS server telemetry for anomalies.
 *
 * nfs_metrics     : num_servers × 11 flattened (row-major)
 * num_servers     : number of servers
 * result          : populated on success
 */
int model_check_anomaly(const char *sock_path,
                        const double *nfs_metrics,
                        int num_servers,
                        struct model_anomaly_result *result);

/* ---- Load Prediction ---- */

struct model_predict_result {
    int predictions[64];   /* binary predictions for mount-policy pairs */
    int count;             /* number of predictions returned            */
};

/*
 * Predict upcoming mount load.
 *
 * history : 288 × 16 flattened (row-major) — 24-hour window
 * result  : populated on success
 */
int model_predict_load(const char *sock_path,
                       const double *history,
                       int timesteps,
                       int features,
                       struct model_predict_result *result);

/* ---- Policy Optimization ---- */

struct model_policy_result {
    int    action;
    char   action_name[64];
    double q_values[16];
    int    num_actions;
};

/*
 * Get a DQN policy action.
 *
 * state     : 18-dim state vector
 * state_dim : length (should be 18)
 */
int model_optimize_policy(const char *sock_path,
                          const double *state,
                          int state_dim,
                          struct model_policy_result *result);

/* ---- Health Check ---- */

/*
 * Ping the inference server.  Returns 0 if alive, -1 otherwise.
 */
int model_health_check(const char *sock_path);
