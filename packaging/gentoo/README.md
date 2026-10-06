# Gentoo packaging

A local overlay with a live ebuild of the `modernize` branch:

```sh
sudo eselect repository create hsfmodem           # or any local overlay
sudo cp -r packaging/gentoo/* /var/db/repos/hsfmodem/
sudo cp third_party/linuxant/LICENSE /var/db/repos/hsfmodem/licenses/Conexant
sudo mkdir -p /etc/portage/package.accept_keywords
echo 'net-dialup/hsfmodem **' | sudo tee /etc/portage/package.accept_keywords/hsfmodem
sudo emerge -av net-dialup/hsfmodem
```

The kernel module is signed by `linux-mod-r1` when `USE=modules-sign` is set
(the default with `CONFIG_MODULE_SIG_FORCE`); see `MODULES_SIGN_KEY` in
`linux-mod-r1.eclass` for a key outside the kernel tree.
