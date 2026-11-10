# syntax=docker/dockerfile:1
#
# Per-architecture build/test image. The base image selects the CPU we compile
# and run for: with `platform:` set in docker-compose.yaml the whole container
# (gcc, libc, the resulting luajit binary) is for that architecture.
ARG BASE_IMAGE=debian:bookworm-slim
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive \
    SRC=/src \
    WORK=/work

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential ca-certificates file tar \
 && rm -rf /var/lib/apt/lists/*

COPY docker/ci.sh /usr/local/bin/ci.sh
RUN chmod 0755 /usr/local/bin/ci.sh

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/ci.sh"]
CMD ["native"]
