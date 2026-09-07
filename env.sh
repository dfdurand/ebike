#!/usr/bin/env bash
# Activate the Nordic toolchain bundle for this project.
#
# `west` is not on the system PATH: it lives inside the nRF Connect SDK
# toolchain bundle. This script rebuilds the environment that
# `nrfutil toolchain-manager launch` would set up, from the bundle's
# environment.json, so a plain shell can run west directly.
#
# Usage:  source env.sh   then   west build ...
#
# Override the defaults if your SDK lives elsewhere:
#   NCS_TOOLCHAIN=/opt/ncs/toolchains/<hash> NCS_VERSION=v3.4.0 source env.sh

NCS_TOOLCHAIN="${NCS_TOOLCHAIN:-$HOME/ncs/toolchains/fbf7391cab}"
NCS_VERSION="${NCS_VERSION:-v3.4.0}"
NCS_WORKSPACE="${NCS_WORKSPACE:-$HOME/ncs/$NCS_VERSION}"

if [ ! -d "$NCS_TOOLCHAIN" ]; then
	echo "[eBike] Toolchain not found: $NCS_TOOLCHAIN" >&2
	echo "[eBike] Set NCS_TOOLCHAIN to your bundle (see ~/ncs/toolchains/)." >&2
	return 1 2>/dev/null || exit 1
fi
if [ ! -d "$NCS_WORKSPACE/zephyr" ]; then
	echo "[eBike] NCS workspace not found: $NCS_WORKSPACE" >&2
	echo "[eBike] Set NCS_WORKSPACE to the directory holding zephyr/ and nrf/." >&2
	return 1 2>/dev/null || exit 1
fi

TC="$NCS_TOOLCHAIN"

export PATH="$TC/bin:$TC/usr/bin:$TC/usr/local/bin:$TC/opt/bin:$TC/opt/nanopb/generator-bin:$TC/nrfutil/bin:$TC/opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin:$TC/opt/zephyr-sdk/gnu/riscv64-zephyr-elf/bin:$PATH"
export LD_LIBRARY_PATH="$TC/lib:$TC/lib/x86_64-linux-gnu:$TC/usr/local/lib:$LD_LIBRARY_PATH"
export GIT_EXEC_PATH="$TC/usr/local/libexec/git-core"
export GIT_TEMPLATE_DIR="$TC/usr/local/share/git-core/templates"
export PYTHONHOME="$TC/usr/local"
export PYTHONPATH="$TC/usr/local/lib/python3.12:$TC/usr/local/lib/python3.12/site-packages"
export PERL5LIB="$TC/etc/perl:$TC/usr/share/perl5:$TC/usr/lib/x86_64-linux-gnu/perl/5.30.0:$TC/usr/share/perl/5.30.0:$TC/usr/lib/x86_64-linux-gnu/perl-base"
export PERLLIB="$TC/usr/share/perl5:$TC/usr/lib/x86_64-linux-gnu/perl/5.30.0:$TC/usr/share/perl/5.30.0:$TC/usr/lib/x86_64-linux-gnu/perl-base"
export TCL_LIBRARY="$TC/usr/local/lib/tcl8.6"
export TK_LIBRARY="$TC/usr/local/lib/tk8.6"
export NRFUTIL_HOME="$TC/nrfutil/home"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr/gnu
export ZEPHYR_SDK_INSTALL_DIR="$TC/opt/zephyr-sdk"

# The application lives outside the NCS workspace, so ZEPHYR_BASE has to point
# at the workspace explicitly for west to pick the right Zephyr tree.
export ZEPHYR_BASE="$NCS_WORKSPACE/zephyr"

echo "[eBike] Nordic toolchain $NCS_VERSION activated (west from $TC)"
echo "[eBike] ZEPHYR_BASE=$ZEPHYR_BASE"
