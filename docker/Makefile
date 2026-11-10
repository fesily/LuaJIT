# Thin make wrapper around docker-compose.yaml.
#
#   make -f docker/Makefile                    # this help
#   make -f docker/Makefile binfmt             # register qemu-user (once per boot)
#   make -f docker/Makefile arm64              # build + smoke + tests, one target
#   make -f docker/Makefile all                # binfmt, arm64, armhf, cross
#   make -f docker/Makefile cross              # full cross matrix
#   make -f docker/Makefile clean              # drop images, containers, cache
#
# Passthrough variables:
#   STAGE=build|test|all   stop after the build / only run tests
#   MAKE_JOBS=4            parallel make jobs inside the container
#   MAKE_FLAGS="XCFLAGS=-DLUAJIT_DISABLE_JIT"
#   TEST_CMD="./tests/run.sh"   replaces the tests/run.sh hook
#   CROSS_TOOLCHAINS="gcc-x86-64-linux-gnu ..."  trim the cross image

SHELL := /bin/sh

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
COMPOSE := docker compose -f $(ROOT)/docker-compose.yaml
PASS := $(if $(STAGE),-e STAGE=$(STAGE)) $(if $(MAKE_JOBS),-e MAKE_JOBS=$(MAKE_JOBS)) \
        $(if $(MAKE_FLAGS),-e MAKE_FLAGS=$(MAKE_FLAGS)) \
        $(if $(TEST_CMD),-e TEST_CMD=$(TEST_CMD))
XARG := $(if $(CROSS_TOOLCHAINS),--build-arg CROSS_TOOLCHAINS=$(CROSS_TOOLCHAINS))

.PHONY: help binfmt arm64 armhf amd64 i386 cross all clean

help:
	@echo "make -f docker/Makefile <target>"
	@echo
	@echo "  binfmt   register qemu-user binfmt handlers (once per boot)"
	@echo "  arm64    build + smoke + tests in a linux/arm64 container"
	@echo "  armhf    build + smoke + tests in a linux/arm/v7 container (qemu-arm)"
	@echo "  amd64    build + smoke + tests in a linux/amd64 container"
	@echo "  i386     build + smoke + tests in a linux/386 container"
	@echo "  cross    build every target in docker/matrix.txt, run what qemu can"
	@echo "  all      binfmt, then arm64, armhf, cross"
	@echo "  clean    remove this project's containers, images and build cache"
	@echo
	@echo "Vars: STAGE=build|test|all MAKE_JOBS=N MAKE_FLAGS=... TEST_CMD=..."
	@echo "      CROSS_TOOLCHAINS=\"gcc-x86-64-linux-gnu ...\""
	@echo
	@echo "Note: amd64/i386 run under qemu-x86_64 on an arm64 host, where a"
	@echo "gcc-12 toolchain crashes intermittently; prefer the cross target."

binfmt:
	$(COMPOSE) run --rm binfmt

arm64:
	$(COMPOSE) run --build --rm $(PASS) arm64

armhf:
	$(COMPOSE) run --build --rm $(PASS) armhf

amd64:
	$(COMPOSE) --profile qemu-x86 run --build --rm $(PASS) amd64

i386:
	$(COMPOSE) --profile qemu-x86 run --build --rm $(PASS) i386

cross:
	$(COMPOSE) build $(XARG) cross
	$(COMPOSE) run --rm $(PASS) cross

all:
	$(COMPOSE) run --rm binfmt
	$(COMPOSE) run --build --rm $(PASS) arm64
	$(COMPOSE) run --build --rm $(PASS) armhf
	$(MAKE) -f $(lastword $(MAKEFILE_LIST)) cross

clean:
	-$(COMPOSE) down --remove-orphans --rmi local
	-docker image rm -f $$(docker images -q --filter=reference='luajit-multiarch-*')
	-docker builder prune -af
	@docker run --rm --privileged debian:bookworm-slim df -h / 2>/dev/null | tail -1 || true
