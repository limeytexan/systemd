# SPDX-License-Identifier: LGPL-2.1-or-later
# Portable Service Manager - cross-platform (Linux + macOS) minimal service manager
#
# Provides: systemd (user instance daemon), systemctl, journalctl
#
# This component is automatically included on Darwin (macOS), and can be
# explicitly enabled on Linux with -Dportable-manager=true.
#
# It does NOT depend on any Linux-specific systemd internals, making it
# easy to rebase from upstream without conflicts.

psm_c_args = []

if host_machine.system() == 'darwin'
  psm_c_args += ['-DPSM_BUILD_TARGET="darwin"']
elif host_machine.system() == 'linux'
  # _GNU_SOURCE must be a compiler flag, not a #define in a header, because
  # some .c files include system headers before psm.h and glibc's include
  # guards prevent re-processing once _GNU_SOURCE is defined late.
  psm_c_args += ['-DPSM_BUILD_TARGET="linux"', '-D_GNU_SOURCE']
else
  psm_c_args += ['-DPSM_BUILD_TARGET="' + host_machine.system() + '"']
endif

# Common sources shared between daemon and tools
psm_common_sources = files(
  'psm-util.c',
  'event.c',
  'parse-unit.c',
  'journal.c',
  'ipc.c',
  'manager.c',
)

# ---- systemd user-instance daemon ----
executable(
  'systemd',
  sources : psm_common_sources + files('portabled.c'),
  c_args  : psm_c_args + ['-DPSM_DAEMON'],
  install : true,
  install_dir : get_option('bindir'),
)

# ---- systemctl ----
executable(
  'systemctl',
  sources : psm_common_sources + files('systemctl.c'),
  c_args  : psm_c_args + ['-DPSM_SYSTEMCTL'],
  install : true,
  install_dir : get_option('bindir'),
)

# ---- journalctl ----
executable(
  'journalctl',
  sources : psm_common_sources + files('journalctl.c'),
  c_args  : psm_c_args + ['-DPSM_JOURNALCTL'],
  install : true,
  install_dir : get_option('bindir'),
)
