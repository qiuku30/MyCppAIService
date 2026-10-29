# Compatibility wrapper; build the local dependency image first (deploy/README.md).
# Dockerfile.build is the sole dependency recipe. The source and Gemma trees
# are mounted exactly as in docker-compose.yml.
ARG BUILD_IMAGE=mycppaiservice-build:local
FROM ${BUILD_IMAGE}
WORKDIR /workspace/build
CMD ["sh", "/workspace/deploy/build-and-run.sh"]
