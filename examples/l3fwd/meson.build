# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2017 Intel Corporation

# meson file, for building this example as part of a main DPDK build.
#
# To build this example as a standalone application with an already-installed
# DPDK instance, use 'make'

allow_experimental_apis = true
deps += ['acl', 'hash', 'lpm', 'fib']
sources = files(
        'l3fwd_acl.c',
        'l3fwd_em.c',
        'l3fwd_event.c',
        'l3fwd_event_internal_port.c',
        'l3fwd_event_generic.c',
        'l3fwd_fib.c',
        'l3fwd_lpm.c',
        'main.c',
)
if dpdk_conf.has('RTE_LIB_EVENTDEV')
    deps += 'eventdev'
endif
cflags += no_wvla_cflag
