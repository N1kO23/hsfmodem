# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

PYTHON_COMPAT=( python3_{11..14} )
inherit git-r3 linux-mod-r1 meson python-single-r1

DESCRIPTION="Conexant HSF HD-audio softmodem: codec driver and userspace engine daemon"
HOMEPAGE="https://github.com/N1kO23/hsfmodem"
EGIT_REPO_URI="https://github.com/N1kO23/hsfmodem.git"
EGIT_BRANCH="modernize"

# MIT: daemon and tools; GPL-2: kernel module; Conexant: the engine blobs
# linked into hsfmodemd (third_party/linuxant/LICENSE), which forbid
# redistributing modified or linked forms.
LICENSE="MIT GPL-2 Conexant"
SLOT="0"
IUSE="systemd test"
REQUIRED_USE="${PYTHON_REQUIRED_USE}"
RESTRICT="bindist mirror !test? ( test )"

RDEPEND="
	${PYTHON_DEPS}
	acct-user/hsfmodem
"
BDEPEND="
	${PYTHON_DEPS}
	dev-lang/perl
	virtual/pkgconfig
"

# The Conexant blobs are prebuilt, x86 only, and linked non-PIE.
QA_PREBUILT="usr/sbin/hsfmodemd"
MODULES_KERNEL_MIN=6.17
CONFIG_CHECK="SND_HDA_INTEL SND_HWDEP"

pkg_setup() {
	linux-mod-r1_pkg_setup
	python-single-r1_pkg_setup
}

src_configure() {
	local emesonargs=(
		-Dopenrc=true
		$(meson_use systemd)
		$(meson_use test tests)
	)
	meson_src_configure
}

src_compile() {
	meson_src_compile

	local modlist=( snd-hda-codec-hsfmodem=updates:kernel )
	local modargs=( KDIR="${KV_OUT_DIR}" )
	linux-mod-r1_src_compile
}

src_test() {
	meson_src_test
}

src_install() {
	meson_src_install
	linux-mod-r1_src_install

	python_fix_shebang "${ED}"/usr/sbin/hsfmodem-config
	keepdir /var/lib/hsfmodem/dynamic
	fowners -R hsfmodem:hsfmodem /var/lib/hsfmodem
	fperms 0700 /var/lib/hsfmodem/dynamic
	dodoc README.md docs/*.md
}

pkg_postinst() {
	linux-mod-r1_pkg_postinst

	elog "ThinkPad X/T/R60: let snd-hda-intel probe the modem slot with"
	elog "  hsfmodem-config setup"
	elog "then reboot or reload snd-hda-intel. Choose your region with"
	elog "  hsfmodem-config region AUTO   (or a name from 'hsfmodem-config regions')"
	elog "and start the service (rc-service hsfmodemd start). The modem is"
	elog "/dev/ttySHSF0. See /usr/share/doc/${PF}/HARDWARE-TESTING.md*."
}
