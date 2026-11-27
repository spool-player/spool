#!/usr/bin/env python3
"""Install a split iOS kit with aqtinstall 3.3 (predates the split layout)."""

from aqt import installer
from aqt.archives import QtArchives
from aqt.updater import Updater


class IosArchives(QtArchives):
    def _arch_ext(self):
        return "_" + self.arch.removeprefix("ios_")

    def _base_package_names(self):
        kit = self.arch.removeprefix("ios_")
        return (f"qt.qt{self.version.major}.{self._version_str()}.qtforios.{kit}",)

    def _parse_update_xml(self, folder, xml, packages):
        # The base package ends in .device or .simulator_arm64, but the
        # selected add-ons and the installation directory use the ios_ prefix.
        arch = self.arch
        self.arch = arch.removeprefix("ios_")
        try:
            super()._parse_update_xml(folder, xml, packages)
        finally:
            self.arch = arch


class IosUpdater(Updater):
    @classmethod
    def update(cls, target, base_path, installed_desktop_arch_dir):
        arch = target.arch
        prefix = base_path / target.version / arch
        updater = cls(prefix, installer.getLogger("aqt.updater"))
        updater.set_license(str(base_path), target.version, arch)
        updater.patch_qt_scripts(
            str(base_path), target.version, target.os_name, "macos", installer.Version(target.version)
        )
        updater.patch_target_qt_conf(str(base_path), target.version, arch, target.os_name, "macos")


if __name__ == "__main__":
    installer.QtArchives = IosArchives
    installer.Updater = IosUpdater
    raise SystemExit(installer.Cli().run())
