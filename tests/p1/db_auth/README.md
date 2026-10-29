Run from the repository root:

```sh
python3 tests/p1/db_auth/run.py
SANITIZE=1 python3 tests/p1/db_auth/run.py
```

This compiles the actual pool and connection implementation with Clang C++17 (`-Wall -Wextra -Werror`). It uses offline connector fixtures to control ping blocking, connect/reconnect failure, statement/result lifetime, captured parameters, and affected-row counts. The auth fixture compiles the exact `insertUser`, `isUserExist`, and `queryUserId` definitions copied from the handler sources; only app-specific includes and HTTP/session handling are replaced. Password hashing uses the real OpenSSL crypto library. The macOS runner uses `/opt/homebrew/opt/openssl@3`; `OPENSSL_PREFIX` can select another installation.

Eight cases cover exclusive health checks, pool capacity recovery after database outages, lease return after pool-owner destruction, parameterized auth with hostile inputs, distinct random salts, correct and wrong passwords, tampering and malformed encodings, an independent PBKDF2 SHA256 vector, embedded NUL bytes, legacy upgrade including failed writes and concurrent-change rejection, connection locking during result consumption, RAII on normal and exceptional callback exits, mutable string parameter binding, MysqlUtil lease retention through callback completion, shutdown without connector destruction under the pool lock, and an in-flight checkout already holding shared state when its owner shuts down.

Connector options are captured as typed pre-connect values: integer connect/read/write timeouts, selected schema and credential strings, and false booleans for automatic reconnect and `CLIENT_MULTI_STATEMENTS`. The legacy JDBC connector exposes `driver->connect(ConnectOptionsMap&)`; its implementation reads timeout options as integers and connection flags as booleans before connecting. It does not apply these options via the old post-connect `setClientOption` strings. See the [official driver implementation](https://github.com/mysql/mysql-connector-cpp/blob/trunk/jdbc/driver/mysql_connection.cpp) and [driver API](https://github.com/mysql/mysql-connector-cpp/blob/trunk/jdbc/cppconn/driver.h). Automatic reconnect is disabled because the pool owns reconnect attempts and must never silently retry a statement after an uncertain connection failure.

To reproduce baseline failures without changing the working tree:

```sh
python3 tests/p1/db_auth/run.py de1731b
```

The baseline is expected to fail. RED was first recorded before implementation with four cases failing (pool health overlap, unrecoverable capacity loss, invalid lease return after owner destruction, SQL concatenation). A fifth baseline case later verifies the missing connection mutex during result consumption. Each run prints its temporary build directory; it never contacts MySQL or RabbitMQ. These fixture tests do not establish production driver compatibility, a full application build, actual HTTP/session integration, or deployment-schema application.
