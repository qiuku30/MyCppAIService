# Real Linux integration

`run.py` talks to the actual running application, MySQL server, and RabbitMQ
server. It uses only Python's standard library on the host and the real `mysql`
and `curl` clients inside the test containers. It does not build/start the stack,
install dependencies, emulate an upstream model, or remove data.

`compose.yml` defines the isolated `mycpp-it` project. Only HTTP is published, at
`127.0.0.1:18117`; database, broker and management ports stay on the private Docker
network. Resources use dedicated named volumes and the
`io.mycpp.integration=true` label. Existing deployment resources are not reused.
The idle `control` service uses the same dependency image as the app so its real
`curl` client can inspect/publish to RabbitMQ while the app is stopped.

## Setup and execution

The parent build workflow must first build the real Linux dependency image as
`mycppaiservice-build:integration`, build compatible Gemma libraries, and supply a
private environment file containing dedicated test values:

```text
MYSQL_ROOT_PASSWORD=<generated test-only value>
RABBITMQ_USER=mycpp_it
RABBITMQ_PASS=<generated test-only value>
GEMMA_SOURCE_HOST_DIR=<absolute real Gemma source directory>
GEMMA_BUILD_HOST_DIR=<absolute compatible Linux Gemma build directory>
GEMMA_MODELS_HOST_DIR=<absolute real model directory, or an empty directory>
BUILD_JOBS=1
```

An empty model directory supports non-model integration and causes the real
Gemma stage to report unavailable. It cannot establish successful generation.
No provider keys are required or passed by this Compose file.

From the repository root, after the dependency build is ready:

```sh
export DOCKER_CONFIG=/private/tmp/mycpp-integration/docker-config
export IT_DOCKER_CONTEXT=colima-mycppaiservice
docker --context "$IT_DOCKER_CONTEXT" compose \
  --env-file /private/tmp/mycpp-integration/stack.env \
  -f tests/integration/compose.yml config --quiet
docker --context "$IT_DOCKER_CONTEXT" compose \
  --env-file /private/tmp/mycpp-integration/stack.env \
  -f tests/integration/compose.yml up -d mysql rabbitmq control app
python3 tests/integration/run.py --probe
python3 tests/integration/run.py --stage prepare,http,auth,persistence,restart,db-restart,broker
python3 tests/integration/run.py --stage producer
python3 tests/integration/run.py --stage gemma --gemma
```

`config --quiet` validates without printing interpolated credentials. A sandbox
may require permission for loopback networking and Docker access. The readiness
probe performs no HTTP mutation or container lifecycle operation.

## Stages and evidence

| Stage | Actual checks/actions |
| --- | --- |
| `prepare` | Label/container/image identity, loopback HTTP binding, no DB/MQ host bindings, real schema, durable `sql_queue`, HTTP readiness. |
| `http` | Default 404 and CORS 204 status/framing; mixed-case headers; invalid, conflicting, overflowing and oversized Content-Length; unsupported/ambiguous transfer coding; invalid header syntax/control bytes; coalesced and fragmented requests; body reset. |
| `auth` | Register and log in three generated accounts, including an SQL-literal username; duplicate/wrong-password rejection; exact stored username; independently verify actual stored PBKDF2 hashes and different salts; unauthenticated chat rejection. |
| `producer` (optional) | Compile the current production `MQManager.cpp` against real muduo/AMQP-CPP in the app container; publish one generated exchange three times through that manager, require real broker queue publication counters and exactly two database rows, then wait for queue drain. No model needed. |
| `persistence` | Publish structured exchanges through the real broker, deliver each three times, wait for queue drain, compare exactly two persisted rows and original roles/content/timestamps; seed explicit synthetic legacy rows with roles `0,1,1`; verify old SQL payload is quarantined and never executed. |
| `restart` | Restart the actual app, compare database snapshot digests, relogin, compare HTTP history with every persisted role/content/timestamp, enforce per-user session lists, reject cross-user sends/reads without durable changes. |
| `db-restart` | Restart MySQL preserving its volume; compare rows, verify live app login/pool recovery before restarting the app; restore and compare HTTP history again. |
| `broker` | Stop the app, queue a persistent message, restart RabbitMQ and inspect retained payload/delivery mode; start app and verify persistence/dedup; restart RabbitMQ while app stays running and verify consumer reconnection/new persistence; restore HTTP history again. |
| `gemma` | With `--gemma`, request a real modelType `5` SSE answer; require content and exactly one completion, match persisted question/answer/roles/timestamps, then restart and compare history. Unloaded weights are unavailable. |

Each stage writes a mode-0600 JSON receipt containing individual passed checks,
status, timings, database hashes/counts and resource identifiers. A failure stops
later requested stages. No receipt includes test passwords, cookies or service
credentials. Exit codes are `0` for requested stages passed, `1` for failure, and
`2` when a requested stage is unavailable. Default stages include `gemma`, so a
non-model run cannot be presented as complete model integration.

Default artifacts:

```text
/private/tmp/mycpp-integration/harness-state.json
/private/tmp/mycpp-integration/evidence/<run>-<stage>-<timestamp>.json
```

The private state contains generated test account passwords and login cookies
needed for relogin/restarts; do not print or commit it. Use the same `--state`
when resuming stages. Use a new path for a new account set. The default containers
can be changed with `IT_{MYSQL,RABBIT,APP,CONTROL}_CONTAINER`; all must retain the
`mycpp-it-` name prefix and integration label. `BASE_URL`, `IT_DOCKER_CONTEXT`,
`IT_DOCKER`, and `IT_DB_NAME` are configurable; the DB must be `mycpp_it` or have a
`mycpp_it_` prefix. Before each non-probe stage, the runner verifies the app's
loopback binding and its `DB_NAME` against these settings.

Direct management publications are explicitly labelled as synthetic protocol
inputs. They prove actual consumer/database behavior and broker durability, not
model output, application producer confirms, load capacity or power-loss safety.
The optional `producer` stage exercises the real production MQ manager without
requiring a model. The real Gemma stage also includes normal chat orchestration
when weights are available.
Legacy SQL rows are explicit inputs for real restart/history restoration; they
are never represented as model-generated conversations. Named volumes and all
test accounts are retained for evidence; cleanup is a separate explicit action.

RabbitMQ 3.13's primary [publisher handler source](https://github.com/rabbitmq/rabbitmq-server/blob/v3.13.7/deps/rabbitmq_management/src/rabbit_mgmt_wm_exchange_publish.erl)
and [HTTP API reference](https://github.com/rabbitmq/rabbitmq-server/blob/v3.13.7/deps/rabbitmq_management/priv/www/api/index.html)
define the POST publication and `ack_requeue_true` inspection used here. These
management operations are restricted to the dedicated development/test broker.

## Optional production MQ producer

After `auth`, run `python3 tests/integration/run.py --stage producer` using the
same state file. The stage executes `mq_producer.sh` in the actual app container,
compiling `mq_producer.cpp` together with
`AIApps/ChatServer/src/AIUtil/MQManager.cpp`. It links the dependency image's real
`muduo_net`, `muduo_base` and `amqpcpp` libraries, with no stub include paths. The
binary and private build log are `/workspace/build/it-mq-producer` and
`/workspace/build/it-mq-producer-build.log` inside that container/build volume.

The driver receives one structured exchange through stdin. It starts the real
manager with an independent `mycpp_it_producer_probe_<message_id>` consumer queue
and an ACK callback; it never consumes `sql_queue`. It publishes the same body
to `sql_queue` three times, runs the real EventLoop for 12 seconds by default,
and shuts down the manager on its owning loop before deferred cleanup and loop
destruction. `--producer-seconds` can select 1-60 seconds. The driver does not
print the body, environment or credentials and does not restart services.

`publish()` returning true is recorded only as bounded memory admission. A
producer stage pass additionally requires the actual application consumer to
persist precisely the expected two rows, the dedicated real broker to count at
least three queue publications, and the queue to drain. The receipt includes
the binary checksum and exact database digest. Internal publisher-confirm
callbacks are not directly exposed by the public MQManager API and are not
claimed as directly observed. Build/driver errors are unavailable (exit 2),
never a product RED. This stage is optional and does not alter default stages.

## Current execution status

The parent completed the real Linux dependency image, Gemma static libraries,
and all 42 application C++ translation units. Eight live stages passed with 56
named checks; real image C++ and HTTP contracts also passed. A full VM cold
restart retained the exact 13-row database digest and passed history/image
revalidation. See [the local execution report](RESULTS.md) for commands and receipts.

Real Gemma generation remains unavailable because its weights/tokenizer are on
Windows; cloud calls have no local keys. Additional image sanitizer runs completed
functional assertions but exited 1 on LSan's 48-byte ORT initialization report,
independently reproduced with bare Ort::Env on 1.18.0 and isolated 1.18.1.
These results must not be described as a clean sanitizer run or complete model
integration. The private SDK/configuration is retained in a separate persistent
user directory; project source remains read-only inside the VM.
