/*
 * model_client.c — C client for the AI-OMB inference sidecar.
 *
 * Communicates with the Python inference_server.py over a UNIX socket
 * using a simple length-prefixed JSON protocol:
 *
 *   [4 bytes big-endian length][JSON payload]
 *
 * Every public function returns 0 on success and -1 when the model
 * server is unavailable or the response indicates fallback.  Callers
 * must always have a default path for the -1 case.
 */

#include "common.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <jansson.h>
#include "model_client.h"
#include "logger.h"

/* Maximum JSON response we'll accept (4 MB). */
#define MODEL_RESP_MAX (4 * 1024 * 1024)

/* Connect timeout in seconds. */
#define MODEL_CONNECT_TIMEOUT 2

/* Read/write timeout in seconds. */
#define MODEL_IO_TIMEOUT 5

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static int model_connect(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    /* Set send/recv timeouts so we don't block the daemon indefinitely */
    struct timeval tv;
    tv.tv_sec = MODEL_IO_TIMEOUT;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Send a length-prefixed JSON message. */
static int send_json(int fd, json_t *obj)
{
    char *payload = json_dumps(obj, JSON_COMPACT);
    if (!payload)
        return -1;

    uint32_t len = htonl((uint32_t)strlen(payload));
    int rc = -1;

    if (write(fd, &len, 4) == 4 &&
        write(fd, payload, strlen(payload)) == (ssize_t)strlen(payload))
        rc = 0;

    free(payload);
    return rc;
}

/* Receive a length-prefixed JSON message.  Caller must json_decref(). */
static json_t *recv_json(int fd)
{
    uint8_t header[4];
    ssize_t n = 0;
    while (n < 4) {
        ssize_t r = read(fd, header + n, 4 - (size_t)n);
        if (r <= 0)
            return NULL;
        n += r;
    }

    uint32_t length = ((uint32_t)header[0] << 24) |
                      ((uint32_t)header[1] << 16) |
                      ((uint32_t)header[2] <<  8) |
                      ((uint32_t)header[3]);

    if (length == 0 || length > MODEL_RESP_MAX)
        return NULL;

    char *buf = malloc(length + 1);
    if (!buf)
        return NULL;

    size_t got = 0;
    while (got < length) {
        ssize_t r = read(fd, buf + got, length - got);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += (size_t)r;
    }
    buf[length] = '\0';

    json_error_t jerr;
    json_t *root = json_loads(buf, 0, &jerr);
    free(buf);
    return root;
}

/*
 * Send a request and receive a response.  Returns the parsed JSON
 * response (caller must json_decref) or NULL on failure.
 */
static json_t *model_rpc(const char *sock_path, json_t *request)
{
    int fd = model_connect(sock_path);
    if (fd < 0) {
        json_decref(request);
        return NULL;
    }

    if (send_json(fd, request) < 0) {
        json_decref(request);
        close(fd);
        return NULL;
    }
    json_decref(request);

    json_t *response = recv_json(fd);
    close(fd);

    /* If the server says "fallback", treat as failure */
    if (response && json_object_get(response, "fallback")) {
        json_decref(response);
        return NULL;
    }
    /* If the server returned an error string, log and treat as failure */
    if (response && json_object_get(response, "error")) {
        const char *err = json_string_value(json_object_get(response, "error"));
        log_write("Model server error: %s\n", err ? err : "(unknown)");
        json_decref(response);
        return NULL;
    }

    return response;
}

/* Helper: build a JSON array of doubles. */
static json_t *doubles_to_json_array(const double *arr, int n)
{
    json_t *ja = json_array();
    for (int i = 0; i < n; i++)
        json_array_append_new(ja, json_real(arr[i]));
    return ja;
}

/* Helper: build a JSON 2D array (rows × cols) from flat row-major doubles. */
static json_t *doubles_to_json_2d(const double *arr, int rows, int cols)
{
    json_t *outer = json_array();
    for (int r = 0; r < rows; r++) {
        json_t *row = json_array();
        for (int c = 0; c < cols; c++)
            json_array_append_new(row, json_real(arr[r * cols + c]));
        json_array_append_new(outer, row);
    }
    return outer;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int model_select_server(const char *sock_path,
                        const double *telemetry,
                        const double *predicted_load,
                        const double *connections,
                        int policy_index,
                        int num_servers,
                        int num_policies,
                        struct model_server_result *result)
{
    json_t *params = json_object();
    json_object_set_new(params, "telemetry",
                        doubles_to_json_2d(telemetry, num_servers, 11));
    json_object_set_new(params, "predicted_load",
                        doubles_to_json_array(predicted_load, num_servers));
    json_object_set_new(params, "connections",
                        doubles_to_json_array(connections, num_servers));
    json_object_set_new(params, "policy_index", json_integer(policy_index));
    json_object_set_new(params, "num_policies", json_integer(num_policies));

    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string("select_server"));
    json_object_set_new(req, "params", params);

    json_t *resp = model_rpc(sock_path, req);
    if (!resp)
        return -1;

    result->server_index = (int)json_integer_value(
        json_object_get(resp, "server_index"));
    result->num_servers = num_servers;

    json_t *qv = json_object_get(resp, "q_values");
    if (json_is_array(qv)) {
        for (int i = 0; i < num_servers && i < MODEL_MAX_SERVERS; i++)
            result->q_values[i] = json_real_value(json_array_get(qv, (size_t)i));
    }

    json_t *hs = json_object_get(resp, "health_scores");
    if (json_is_array(hs)) {
        for (int i = 0; i < num_servers && i < MODEL_MAX_SERVERS; i++)
            result->health_scores[i] = json_real_value(json_array_get(hs, (size_t)i));
    }

    json_decref(resp);
    return 0;
}

int model_check_anomaly(const char *sock_path,
                        const double *nfs_metrics,
                        int num_servers,
                        struct model_anomaly_result *result)
{
    json_t *params = json_object();
    json_object_set_new(params, "nfs_metrics",
                        doubles_to_json_2d(nfs_metrics, num_servers, 11));

    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string("check_anomaly"));
    json_object_set_new(req, "params", params);

    json_t *resp = model_rpc(sock_path, req);
    if (!resp)
        return -1;

    result->any_anomalous = json_is_true(
        json_object_get(resp, "any_anomalous")) ? 1 : 0;

    json_t *servers = json_object_get(resp, "servers");
    result->count = 0;
    if (json_is_array(servers)) {
        size_t n = json_array_size(servers);
        if (n > MODEL_MAX_SERVERS)
            n = MODEL_MAX_SERVERS;
        result->count = (int)n;
        for (size_t i = 0; i < n; i++) {
            json_t *s = json_array_get(servers, i);
            result->servers[i].server_index = (int)json_integer_value(
                json_object_get(s, "server_index"));
            result->servers[i].error = json_real_value(
                json_object_get(s, "error"));
            result->servers[i].threshold = json_real_value(
                json_object_get(s, "threshold"));
            result->servers[i].anomalous = json_is_true(
                json_object_get(s, "anomalous")) ? 1 : 0;
        }
    }

    json_decref(resp);
    return 0;
}

int model_predict_load(const char *sock_path,
                       const double *history,
                       int timesteps,
                       int features,
                       struct model_predict_result *result)
{
    json_t *params = json_object();
    json_object_set_new(params, "history",
                        doubles_to_json_2d(history, timesteps, features));

    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string("predict_load"));
    json_object_set_new(req, "params", params);

    json_t *resp = model_rpc(sock_path, req);
    if (!resp)
        return -1;

    json_t *preds = json_object_get(resp, "predictions");
    result->count = 0;
    if (json_is_array(preds)) {
        size_t n = json_array_size(preds);
        if (n > 64)
            n = 64;
        result->count = (int)n;
        for (size_t i = 0; i < n; i++)
            result->predictions[i] = (int)json_integer_value(
                json_array_get(preds, i));
    }

    json_decref(resp);
    return 0;
}

int model_optimize_policy(const char *sock_path,
                          const double *state,
                          int state_dim,
                          struct model_policy_result *result)
{
    json_t *params = json_object();
    json_object_set_new(params, "state",
                        doubles_to_json_array(state, state_dim));

    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string("optimize_policy"));
    json_object_set_new(req, "params", params);

    json_t *resp = model_rpc(sock_path, req);
    if (!resp)
        return -1;

    result->action = (int)json_integer_value(
        json_object_get(resp, "action"));

    const char *name = json_string_value(
        json_object_get(resp, "action_name"));
    if (name)
        snprintf(result->action_name, sizeof(result->action_name), "%s", name);
    else
        result->action_name[0] = '\0';

    json_t *qv = json_object_get(resp, "q_values");
    result->num_actions = 0;
    if (json_is_array(qv)) {
        size_t n = json_array_size(qv);
        if (n > 16)
            n = 16;
        result->num_actions = (int)n;
        for (size_t i = 0; i < n; i++)
            result->q_values[i] = json_real_value(json_array_get(qv, i));
    }

    json_decref(resp);
    return 0;
}

int model_health_check(const char *sock_path)
{
    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string("health"));

    json_t *resp = model_rpc(sock_path, req);
    if (!resp)
        return -1;

    const char *status = json_string_value(json_object_get(resp, "status"));
    int ok = (status && strcmp(status, "ok") == 0) ? 0 : -1;
    json_decref(resp);
    return ok;
}
