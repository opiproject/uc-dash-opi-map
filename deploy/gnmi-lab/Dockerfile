# SPDX-License-Identifier: Apache-2.0
# Lab sonic-gnmi: official gNMI RPCs (openconfig/gnmi) writing DASH APPL_DB.
# Swap the image for docker-sonic-gnmi from sonic-buildimage when available.
FROM python:3.12-slim-bookworm

RUN apt-get update && apt-get install -y --no-install-recommends git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/gnmi-lab
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Official gNMI protobufs (same service gnmic / gnmi_cli speak).
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

COPY server.py .

ENV PYTHONUNBUFFERED=1
ENV REDIS_HOST=redis-appl-db
ENV REDIS_PORT=6379
ENV APPL_DB=0
EXPOSE 8080
CMD ["python", "-u", "server.py"]
