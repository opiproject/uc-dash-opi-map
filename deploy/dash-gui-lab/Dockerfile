# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Summit demo GUI: browser → this service → sonic-gnmi (gNMI Set) → APPL_DB.
FROM python:3.12-slim-bookworm

RUN apt-get update && apt-get install -y --no-install-recommends git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/dash-gui
COPY deploy/dash-gui-lab/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

RUN git clone --depth 1 --branch v0.11.0 https://github.com/openconfig/gnmi.git /tmp/gnmi \
    && mkdir -p proto \
    && cp /tmp/gnmi/proto/gnmi/gnmi.proto proto/gnmi.proto \
    && cp /tmp/gnmi/proto/gnmi_ext/gnmi_ext.proto proto/gnmi_ext.proto \
    && sed -i 's|import "github.com/openconfig/gnmi/proto/gnmi_ext/gnmi_ext.proto"|import "gnmi_ext.proto"|' proto/gnmi.proto \
    && python -c "import grpc_tools, pathlib; print(pathlib.Path(grpc_tools.__file__).parent / '_proto')" > /tmp/wk \
    && python -m grpc_tools.protoc -I proto -I "$(cat /tmp/wk)" \
         --python_out=. --grpc_python_out=. \
         proto/gnmi.proto proto/gnmi_ext.proto \
    && rm -rf /tmp/gnmi

COPY deploy/payloads ./payloads
COPY deploy/dash-gui-lab/app.py .
COPY deploy/dash-gui-lab/static ./static

ENV PYTHONUNBUFFERED=1
ENV GNMI_TARGET=sonic-gnmi:8080
ENV REDIS_HOST=redis-appl-db
ENV LISTEN=0.0.0.0:8090
EXPOSE 8090
CMD ["python", "-u", "app.py"]
