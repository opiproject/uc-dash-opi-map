# SPDX-License-Identifier: Apache-2.0
# Lab syncd: compile liblab_sai.so (dlopen wrapper) and run syncd.py.
#
# Base must match the host that builds libsai_opi_adapter.so (README: Ubuntu
# jammy libgrpc++-dev / libprotobuf23). Bookworm's libprotobuf32 is a different
# SONAME (libprotobuf.so.32) and cannot dlopen a jammy-built adapter.

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      python3 python3-pip \
      gcc libc6-dev \
      libprotobuf23 libgrpc++1 libgrpc10 \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf python3 /usr/bin/python

WORKDIR /opt/syncd
COPY deploy/syncd-lab/requirements.txt .
RUN pip3 install --no-cache-dir -r requirements.txt
COPY deploy/syncd-lab/lab_sai.c deploy/syncd-lab/syncd.py deploy/syncd-lab/entrypoint.sh ./
RUN gcc -shared -fPIC -o /usr/local/lib/liblab_sai.so lab_sai.c -ldl \
    && chmod +x entrypoint.sh \
    && ldconfig

ENV PYTHONUNBUFFERED=1
ENV REDIS_HOST=redis-appl-db
ENV OPI_SERVER_ADDRESS=host.docker.internal:50051
ENV LD_LIBRARY_PATH=/usr/lib:/usr/local/lib
ENV SAI_OPI_LOG=/var/log/sai-opi-adapter.log
CMD ["./entrypoint.sh"]
