# NOSaic — top-level build entry point.
#
# The gate this file exists to satisfy: a fresh clone on a machine with only
# Docker and make must pass `make check`. Every target therefore runs inside
# the pinned builder container by default. Set NATIVE=1 to use host tools
# instead, which is faster for local iteration but is not what CI does.

include builder/images.env

# Host-specific overrides (docker network, proxies, apt flags). Git-ignored:
# a local quirk must never become everyone's default.
-include local.mk

export

SHELL      := /bin/bash

# Resource limits.
#
# A toolchain build will otherwise take the whole machine: GCC's memory use
# scales with -j, and on Docker's vfs storage driver the I/O multiplies too.
# On a 6-core box, -j6 drove load average past 70 and exhausted memory. These
# defaults are deliberately conservative -- a build that makes the machine
# unusable is a broken build, however fast it would have been.
#
# Raise them in local.mk on a machine with headroom.
NPROC      := $(shell nproc 2>/dev/null || echo 2)
MEM_TOTAL  := $(shell free -m 2>/dev/null | awk '/^Mem:/{print $$2}' || echo 4096)
CPUS       ?= $(shell v=$$(( $(NPROC) / 3 )); [ $$v -lt 1 ] && v=1; echo $$v)
MEMORY     ?= $(shell v=$$(( $(MEM_TOTAL) / 3 )); [ $$v -lt 2048 ] && v=2048; echo $${v}m)
JOBS       ?= $(CPUS)
REPO_ROOT  := $(shell pwd)
OUT        := $(REPO_ROOT)/out
VERSION    := $(shell cat VERSION 2>/dev/null || echo 0.0.0-dev)
COMMIT     := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
LDFLAGS    := -X github.com/salvaged-silicon/nosaic-switch/internal/version.Version=$(VERSION) \
              -X github.com/salvaged-silicon/nosaic-switch/internal/version.Commit=$(COMMIT)

# Run a command in the builder container, or on the host when NATIVE=1.
ifeq ($(NATIVE),1)
  RUN :=
else
  RUN := docker run --rm -t $(if $(DOCKER_NETWORK),--network $(DOCKER_NETWORK),) \
           --cpus=$(CPUS) --memory=$(MEMORY) \
           -e JOBS=$(JOBS) \
           -v $(REPO_ROOT):/src -w /src \
           -v nosaic-gocache:/root/.cache/go-build \
           -v nosaic-gomod:/root/go/pkg/mod \
           -e HOME=/root \
           $(BUILDER_IMAGE)
endif

.DEFAULT_GOAL := help

## help: list targets
help:
	@echo "NOSaic $(VERSION) ($(COMMIT))"
	@echo
	@sed -n 's/^## //p' $(MAKEFILE_LIST) | awk -F': ' '{printf "  %-14s %s\n", $$1, $$2}'
	@echo
	@echo "  NATIVE=1 <target>   use host tools instead of the builder container"
	@echo
	@echo "resource limits (override in local.mk):"
	@echo "  CPUS=$(CPUS)  MEMORY=$(MEMORY)  JOBS=$(JOBS)   (host: $(NPROC) cores, $(MEM_TOTAL)m)"

## builder: build the pinned build container
builder:
	docker build $(if $(DOCKER_NETWORK),--network=$(DOCKER_NETWORK),) \
	  --build-arg APT_FORCE_IPV4=$(if $(APT_FORCE_IPV4),$(APT_FORCE_IPV4),false) \
	  --build-arg UBUNTU_REF=$(UBUNTU_REF) \
	  --build-arg GO_VERSION=$(GO_VERSION) \
	  --build-arg GO_SHA256=$(GO_SHA256) \
	  -t $(BUILDER_IMAGE) -f builder/Dockerfile.build builder/

# Targets that run in the container need it to exist first.
ifeq ($(NATIVE),1)
  BUILDER_DEP :=
else
  BUILDER_DEP := builder-if-missing
endif

builder-if-missing:
	@docker image inspect $(BUILDER_IMAGE) >/dev/null 2>&1 || $(MAKE) builder

## check: validate the repo against the design invariants
check: $(BUILDER_DEP) fmt-check vet test
	@$(RUN) go run ./cmd/nosaic check

## test: run unit tests
test: $(BUILDER_DEP)
	@$(RUN) go test ./...

## vet: static analysis
vet: $(BUILDER_DEP)
	@$(RUN) go vet ./...

## fmt: format the Go source
fmt: $(BUILDER_DEP)
	@$(RUN) gofmt -w -s cmd internal

fmt-check: $(BUILDER_DEP)
	@out=$$($(RUN) gofmt -l -s cmd internal); \
	if [ -n "$$out" ]; then echo "not gofmt-clean:"; echo "$$out"; exit 1; fi

## nosaic: build the CLI for this host
nosaic: $(BUILDER_DEP)
	@mkdir -p $(OUT)
	@$(RUN) go build -buildvcs=false -ldflags "$(LDFLAGS)" -o out/nosaic ./cmd/nosaic
	@echo "built $(OUT)/nosaic"

# --- toolchains (M1) -------------------------------------------------------
# ARCHES is derived from arch/, not listed here: a new architecture is a
# directory, the same way a new board is.
ARCHES := $(notdir $(wildcard arch/*))

## toolchains: build every architecture's toolchain
toolchains: $(addprefix toolchain-,$(ARCHES))

## toolchain: build one toolchain, e.g. make toolchain ARCH=x86_64
toolchain: $(BUILDER_DEP)
	@test -n "$(ARCH)" || { echo "usage: make toolchain ARCH=<one of: $(ARCHES)>"; exit 2; }
	@$(RUN) bootstrap/build.sh build $(ARCH)

toolchain-%: $(BUILDER_DEP)
	@$(RUN) bootstrap/build.sh build $*

## toolchain-seed: regenerate a defconfig from the upstream sample
toolchain-seed: $(BUILDER_DEP)
	@test -n "$(ARCH)" || { echo "usage: make toolchain-seed ARCH=<one of: $(ARCHES)>"; exit 2; }
	@$(RUN) bootstrap/build.sh seed $(ARCH)

## toolchain-test: prove a toolchain produces binaries that run
toolchain-test: $(BUILDER_DEP)
	@test -n "$(ARCH)" || { echo "usage: make toolchain-test ARCH=<one of: $(ARCHES)>"; exit 2; }
	@$(RUN) bootstrap/build.sh test $(ARCH)

toolchains-test: $(addprefix toolchain-test-,$(ARCHES))
toolchain-test-%: $(BUILDER_DEP)
	@$(RUN) bootstrap/build.sh test $*

# --- packages (M2) ---------------------------------------------------------
RECIPES := $(notdir $(patsubst %/,%,$(dir $(wildcard recipes/*/recipe.yml))))

## pkg: build one package, e.g. make pkg PKG=zlib ARCH=x86_64
pkg: $(BUILDER_DEP)
	@test -n "$(PKG)"  || { echo "usage: make pkg PKG=<one of: $(RECIPES)> ARCH=<arch>"; exit 2; }
	@test -n "$(ARCH)" || { echo "usage: make pkg PKG=$(PKG) ARCH=<one of: $(ARCHES)>"; exit 2; }
	@$(RUN) go run ./cmd/nosaic pkg build $(PKG) --arch $(ARCH) --jobs $(JOBS)

## packages: build every recipe for one architecture
packages: $(BUILDER_DEP)
	@test -n "$(ARCH)" || { echo "usage: make packages ARCH=<one of: $(ARCHES)>"; exit 2; }
	@for p in $(RECIPES); do \
	   $(RUN) go run ./cmd/nosaic pkg build $$p --arch $(ARCH) --jobs $(JOBS) || exit 1; \
	 done

## kernel-boot: boot a built kernel under QEMU and run its own userspace
kernel-boot: $(BUILDER_DEP)
	@test -n "$(ARCH)" || { echo "usage: make kernel-boot ARCH=<one of: $(ARCHES)>"; exit 2; }
	@$(RUN) boot/virt/smoketest.sh $(ARCH)

## image: assemble a board's image, e.g. make image BOARD=virt-x86_64
image: $(BUILDER_DEP)
	@test -n "$(BOARD)" || { echo "usage: make image BOARD=<board>"; exit 2; }
	@$(RUN) go run ./cmd/nosaic build $(BOARD)

## image-boot: boot a built image under QEMU
image-boot: $(BUILDER_DEP)
	@test -n "$(BOARD)" || { echo "usage: make image-boot BOARD=<board>"; exit 2; }
	@$(RUN) boot/virt/bootimage.sh $(BOARD)

## clean: remove build output
clean:
	rm -rf $(OUT)

## clean-toolchains: remove built toolchains
# crosstool-NG finalises each toolchain read-only, so a plain rm -rf fails on
# every file: you cannot unlink from a directory you cannot write.
clean-toolchains:
	@if [ -d toolchain ]; then chmod -R u+w toolchain; rm -rf toolchain; fi
	@echo "toolchains removed (sources kept in dl/, so a rebuild is offline)"

.PHONY: help builder builder-if-missing check test vet fmt fmt-check nosaic \
        toolchains toolchain toolchain-seed toolchain-test toolchains-test \
        pkg packages kernel-boot image image-boot clean clean-toolchains
