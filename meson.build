# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2017 Intel Corporation

project('fahren', 'c',
	version: '1.0.0',
	license: 'BSD-3-Clause',
	default_options: ['buildtype=release', 'c_std=c17'])

if host_machine.system() != 'linux'
	error('This application can only be built on Linux')
endif

# Find DPDK using pkg-config
dpdk = dependency('libdpdk', required: true)

include_dirs = include_directories('include')

sources = files(
	'lib/utils/log.c',
	'lib/utils/rng.c',
	'lib/utils/utils.c',
	'src/config/config.c',
	# 'src/fast/arp.c',
	# 'src/fast/fastpath.c',
	# 'src/fast/flow.c',
	# 'src/fast/from_eth.c',
	# 'src/fast/from_vm.c',
	# 'src/network/nat.c',
	# 'src/network/network.c',
	# 'src/network/tap.c',
	# 'src/vhost/device.c',
	# 'src/vhost/queues.c',
	'src/main.c',
	# 'src/shm.c',
	'src/dataplane.c',
	'src/ring.c',
)

# Compiler flags
c_args = [
	'-DALLOW_EXPERIMENTAL_API',
	'-D_FILE_OFFSET_BITS=64',
	'-D_GNU_SOURCE',  # For ssize_t and other POSIX extensions
	'-O3'
]

# Build executable
executable('vhost-switch',
	sources,
	c_args: c_args,
	include_directories: include_dirs,
	dependencies: [dpdk],
	install: true,
	install_dir: get_option('bindir')
)
