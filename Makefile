# Shortcuts for the release and CI flows (release.yml, ci.yml): `make build` for
# the dev binary, `make appimage` for the player's AppImage. Each recipe runs
# the same commands those workflows run; this file only names them. `make help`
# lists the targets; variables on the command line override the defaults, e.g.
# `make build EXEFS=~/dump/exefs` or `make appimage VERSION=0.1.12`.
#
# Every target is phony and always runs its commands: build.py/ninja is
# already incremental ("seconds after"), while file targets would go stale
# (an edited runtime with an existing build/dist, a clean that misses a
# directory). The sysroot script is stamp-guarded, so depending on it is a
# no-op once built and rebuilds a partial download.

EXEFS ?= game/exefs
GEN ?= build/generated
PATCHES ?= recomp/patches-mkw.txt
# The release tag without its v (0.1.11), or tag-commits-hash between tags:
# the same source as the binary's own version (port_version in
# tools/build/build.py: describe --tags --always --dirty, "dev" outside git).
# patsubst, not sed: the Windows CI image has make but no sed.
VERSION ?= $(patsubst v%,%,$(shell git describe --tags --always --dirty 2>/dev/null || echo dev))

.PHONY: help build generated shaders selftest check sysroot dist appimage appimage-aarch64 windows clean

help:
	@echo "build            dev binary build/mkw (needs generated C, see generated)"
	@echo "generated        AArch64 to C from your dump (EXEFS, GEN and PATCHES)"
	@echo "shaders          extract and translate the shaders"
	@echo "selftest         build, then build/mkw --selftest"
	@echo "check            the CI checks job except its build (ruff, docs, cfg rows, no-game-code grep, pytest)"
	@echo "sysroot          glibc 2.31 sysroot for releases (no-op once built)"
	@echo "dist             player's folder build/dist/linux (runtime and setup/)"
	@echo "appimage         dist packed as build/release/mkw-recomp-VERSION-x86_64.AppImage"
	@echo "appimage-aarch64 dist for AArch64, packed as build/release/mkw-recomp-VERSION-aarch64.AppImage"
	@echo "windows          player's zip build/release/mkw-recomp-VERSION-windows-x64.zip"
	@echo "clean            remove build objects and packed releases, keep sources and caches"
	@echo "Overrides: EXEFS, GEN, PATCHES (1.8.0: recomp/patches-mkw-180.txt), VERSION"

build:
	uv run tools/build/build.py --gen "$(GEN)" --exefs "$(EXEFS)"

generated:
	uv run python recomp/gen.py --exefs "$(EXEFS)" --out "$(GEN)" --jobs 8 --patches "$(PATCHES)"

shaders:
	uv run tools/shaders/extract.py && uv run tools/shaders/translate.py

selftest: build
	./build/mkw --selftest

check:
	uv run ruff check tools sm86
	uv run tools/docs/check.py
	uv run tools/git/cfg_rows.py
	marker='MKW_RECOMP_'"GENERATED_FROM_GAME_CODE"; \
	if git grep -l -F "$$marker" -- ':!recomp/gen.py' ':!tools/git/pre-commit'; then exit 1; fi; \
	if git ls-files | grep -Ei '\.(nso|nca|nsp|xci|keys|bfsha|bntx|zs)$$|(^|/)(main|sdk|subsdk[0-9]|rtld)$$'; then exit 1; fi
	uv run pytest tests -q

sysroot:
	uv run tools/build/sysroot.py --arch x86_64

dist: sysroot
	uv run tools/build/build.py --dist --runtime-only --sysroot build/sysroot
	uv run tools/build/dist.py --target linux

appimage: dist
	uv run tools/build/release.py --target linux --version "$(VERSION)"

appimage-aarch64:
	uv run tools/build/sysroot.py --arch aarch64
	uv run tools/build/build.py --dist --runtime-only --arch aarch64 --sysroot "build/sysroot-aarch64"
	uv run tools/build/dist.py --target linux-aarch64
	uv run tools/build/release.py --target linux-aarch64 --version "$(VERSION)"

windows:
	uv run tools/build/windeps.py
	uv run tools/build/build.py --target windows --dist --runtime-only
	uv run tools/build/dist.py --target windows
	uv run tools/build/release.py --target windows --version "$(VERSION)"

clean:
	rm -rf build/obj build/obj-dist build/obj-dist-win build/obj-dist-aarch64 build/release
