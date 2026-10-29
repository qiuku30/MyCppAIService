# Deployment P2 checks

Run from the repository root:

```sh
python3 tests/p2/deploy/run.py
sh -n deploy/build-and-run.sh
bash -n run.sh
```

Requires Python 3, CMake 3.16+ and a C++ compiler for CMake's compiler detection.
The suite checks deployment contracts and invokes the production root CMake
configuration with temporary empty headers/archives and package fixtures. It
checks nonstandard dependency prefixes, paths with spaces, cache precedence,
explicit missing-AMQP diagnostics, and generated compile/link paths. It never
builds the executable using those fake files.

Launcher checks execute the real scripts with a fake binary and a recording
CMake command, verifying the source/build paths, working directory, preserved
environment and forwarded arguments. Docker/Compose/schema contracts are static
checks, not container or SQL execution.

The original P1 tree failed all nine initial checks; see the session artifact
`/private/tmp/mycpp-p2-deploy-red.log`. The updated tree passes those cases plus
an additional Docker instruction/RUN shell syntax check. Two later image-asset
checks were RED before adding the opt-in override, and bring the total to 12.
The YAML binding check uses Ruby's standard YAML parser when Ruby is available;
it is explicitly skipped otherwise. On the validation host all 12 checks run.
No Docker daemon/CLI, live MySQL, RabbitMQ or actual Gemma model is available on
the macOS validation host. Real target Linux compilation, linking, image build,
database restart/migration and model inference remain separate acceptance work.
