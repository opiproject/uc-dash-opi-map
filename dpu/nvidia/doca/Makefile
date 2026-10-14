# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Build on the BlueField SoC (not the x86 host cross-compile).
#
#   make            # stub daemon — JSON protocol, no libdoca
#   make DOCA=1     # link DOCA Flow (pkg-config doca-flow + DPDK)
#
# Hugepages before DOCA=1 runtime:
#   echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=
LDLIBS ?=

DOCA_PC_PATHS := \
  /opt/mellanox/doca/lib/pkgconfig \
  /opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig \
  /opt/mellanox/doca/lib64/pkgconfig \
  /usr/lib/aarch64-linux-gnu/pkgconfig \
  /opt/mellanox/dpdk/lib/pkgconfig \
  /opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig

empty :=
space := $(empty) $(empty)
export PKG_CONFIG_PATH := $(PKG_CONFIG_PATH):$(subst $(space),:,$(strip $(DOCA_PC_PATHS)))

ifeq ($(DOCA),1)
  # Flow symbols are marked experimental on 2.7; the setter API is the supported one.
  CFLAGS += -DOPI_DOCA -Wno-deprecated-declarations
  PC_CFLAGS := $(shell pkg-config --cflags doca-flow doca-common 2>/dev/null)
  PC_LIBS := $(shell pkg-config --libs doca-flow doca-common 2>/dev/null)
  ifeq ($(PC_LIBS),)
    PC_CFLAGS := $(shell pkg-config --cflags libdoca-sdk-flow libdoca-sdk-common 2>/dev/null)
    PC_LIBS := $(shell pkg-config --libs libdoca-sdk-flow libdoca-sdk-common 2>/dev/null)
  endif
  ifeq ($(PC_LIBS),)
    $(error pkg-config doca-flow not found under PKG_CONFIG_PATH=$(PKG_CONFIG_PATH). Install DOCA SDK or use `make` stub / DPU_DOCA_STUB=1)
  endif
  CFLAGS += $(PC_CFLAGS)
  LDLIBS += $(PC_LIBS)
  LDLIBS += $(shell pkg-config --libs doca-dpdk-bridge libdpdk 2>/dev/null)
  DOCA_FLOW_H := $(firstword $(wildcard /opt/mellanox/doca/include/doca_flow.h /usr/include/doca_flow.h))
endif

.PHONY: all clean

all: dpu_doca_flowd

DEPS := dpu_doca_flowd.c
ifeq ($(DOCA),1)
DEPS += doca_compat.inc
endif

dpu_doca_flowd: $(DEPS)
	$(CC) $(CFLAGS) -o $@ dpu_doca_flowd.c $(LDFLAGS) $(LDLIBS)

# PDF prototypes omit trailing flags/usr_ctx on 2.7.0085. Probe the installed header.
ifeq ($(DOCA),1)
doca_compat.inc: $(DOCA_FLOW_H) Makefile
	@dir=$$(mktemp -d /tmp/opi_doca.XXXXXX); \
	src=$$dir/p.c; obj=$$dir/p.o; \
	probe_add() { \
	  printf '#include <doca_flow.h>\ndoca_error_t p(void){return doca_flow_pipe_control_add_entry(%s);}\n' "$$1" > "$$src"; \
	$(CC) $(CFLAGS) -Werror=implicit-function-declaration -Werror=int-conversion -Werror=incompatible-pointer-types -c -o "$$obj" "$$src" >/dev/null 2>&1; \
	}; \
	b='0,0,(void*)0,(void*)0,(void*)0,(void*)0,(void*)0,(void*)0,(void*)0,(void*)0,(void*)0'; \
	tail=; \
	if probe_add "$$b, DOCA_FLOW_NO_WAIT, (void*)0, (void*)0"; then tail=', DOCA_FLOW_NO_WAIT, NULL'; \
	elif probe_add "$$b, (void*)0"; then tail=; \
	elif probe_add "$$b, DOCA_FLOW_NO_WAIT, (void*)0"; then tail=', DOCA_FLOW_NO_WAIT'; \
	elif probe_add "$$b, (void*)0, (void*)0"; then tail=', NULL'; \
	else \
	  echo "error: doca_flow_pipe_control_add_entry prototype not recognized:" >&2; \
	  sed -n '/doca_flow_pipe_control_add_entry(/,/);/p' "$(DOCA_FLOW_H)" >&2 || true; \
	  rm -rf "$$dir"; exit 1; \
	fi; \
	have_port_id=0; have_open_dev_first=0; have_first_port=0; \
	if grep -q 'doca_flow_port_cfg_set_port_id' "$(DOCA_FLOW_H)" 2>/dev/null; then \
	  printf '#include <doca_flow.h>\ndoca_error_t p(struct doca_flow_port_cfg *c){return doca_flow_port_cfg_set_port_id(c,0);}\n' > "$$src"; \
	  if $(CC) $(CFLAGS) -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -c -o "$$obj" "$$src" >/dev/null 2>&1; then have_port_id=1; fi; \
	fi; \
	printf '#include <doca_dev.h>\ndoca_error_t p(struct doca_dev **d, struct doca_devinfo *i){return doca_dev_open(d,i);}\n' > "$$src"; \
	if $(CC) $(CFLAGS) -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -c -o "$$obj" "$$src" >/dev/null 2>&1; then have_open_dev_first=1; fi; \
	printf '#include <doca_dev.h>\n#include <doca_dpdk.h>\ndoca_error_t p(struct doca_dev *d, uint16_t *i){return doca_dpdk_get_first_port_id(d,i);}\n' > "$$src"; \
	if $(CC) $(CFLAGS) -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -c -o "$$obj" "$$src" >/dev/null 2>&1; then have_first_port=1; fi; \
	rm -rf "$$dir"; \
	{ \
	  echo "/* generated from $(DOCA_FLOW_H) */"; \
	  echo "#define OPI_CONTROL_ADD_TAIL $$tail"; \
	  [ "$$have_port_id" = 1 ] && echo "#define OPI_HAVE_SET_PORT_ID 1"; \
	  [ "$$have_open_dev_first" = 1 ] && echo "#define OPI_DEV_OPEN_DEV_FIRST 1"; \
	  [ "$$have_first_port" = 1 ] && echo "#define OPI_HAVE_GET_FIRST_PORT_ID 1"; \
	  echo "/* installed control_add_entry prototype: */"; \
	  sed -n '/doca_flow_pipe_control_add_entry(/,/);/p' "$(DOCA_FLOW_H)" | sed 's|^|// |'; \
	} > $@; \
	echo "DOCA control_add_entry tail after fwd: '$$tail' set_port_id=$$have_port_id open_dev_first=$$have_open_dev_first"
endif

clean:
	rm -f dpu_doca_flowd doca_compat.inc
