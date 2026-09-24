# SPDX-License-Identifier: GPL-2.0
#
# Resolve the kernel tree the QEMU and KVM harnesses boot: the one the caller
# names, then the one scripts/internal/fetch_kernel.sh builds, then the running
# kernel's if it is 6.16.5.  Sourced; the caller sets ROOT.

EXT5_KERNEL_VERSION=${EXT5_KERNEL_VERSION:-6.16.5}

ext5_resolve_kernel_root()
{
	local candidate

	if [ -n "${KERNEL_ROOT:-}" ]; then
		echo "$KERNEL_ROOT"
		return 0
	fi
	for candidate in \
		"$ROOT/deploy/kernel/linux-$EXT5_KERNEL_VERSION" \
		"/lib/modules/$(uname -r)/build"; do
		[ -s "$candidate/arch/x86/boot/bzImage" ] || continue
		echo "$candidate"
		return 0
	done
	return 1
}

ext5_require_kernel_root()
{
	KERNEL_ROOT=$(ext5_resolve_kernel_root) || {
		cat >&2 <<MSG
ERROR: no kernel tree with a built bzImage.

  Looked for KERNEL_ROOT, then $ROOT/deploy/kernel/linux-$EXT5_KERNEL_VERSION,
  then /lib/modules/$(uname -r)/build.

  Build one with:   bash scripts/internal/fetch_kernel.sh
  Or point at your own:   KERNEL_ROOT=/path/to/linux-$EXT5_KERNEL_VERSION $0
MSG
		exit 1
	}
	export KERNEL_ROOT
}

# Fail early when the modules were built for another kernel than the one booted.
ext5_require_module_match()
{
	local release built

	[ -s "$KERNEL_ROOT/include/config/kernel.release" ] || return 0
	command -v modinfo >/dev/null 2>&1 || return 0
	read -r release < "$KERNEL_ROOT/include/config/kernel.release"
	built=$(modinfo -F vermagic "$ROOT/src/ext5/ext5.ko" 2>/dev/null |
		awk '{print $1}')
	[ -n "$built" ] || return 0
	[ "$built" = "$release" ] && return 0
	cat >&2 <<MSG
ERROR: the modules were built for a different kernel than this harness boots.

  kernel to boot : $release   ($KERNEL_ROOT)
  modules built  : $built     ($ROOT/src/ext5/ext5.ko)

  Rebuild against the kernel being booted:

      make -C "$ROOT/src" clean
      make -C "$ROOT/src" KDIR="$KERNEL_ROOT"

  Or boot the kernel the modules were built for:

      KERNEL_ROOT=/lib/modules/$built/build $0
MSG
	exit 1
}
