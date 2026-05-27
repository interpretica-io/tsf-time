# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Interpretica Unipessoal Lda
#
# External TE library (see TE_EXT_REPO): running a program on an agent
# under a clock that is not the machine's. Built as a shared library on
# the engine platform and as a static library when agents or agent apps
# are built.

if get_option('engine')
    build_lib_shared = true
endif
build_lib_static = not get_option('engine') \
    or get_option('agents').split().length() != 0 \
    or get_option('agent-apps').split().length() != 0
install_lib = install_dev

headers += files(
    'tapi_time.h',
)

sources += files(
    'tapi_time.c',
)

te_libs += [
    'tapi',
    'tapi_job',
    'tapi_devtool',
]
