# SPDX-License-Identifier: Apache-2.0
# Build from repository root:
#   docker build -f tests/opi_server/Dockerfile .
FROM python:3.12-slim-bookworm
WORKDIR /opt/dash-opi-map
COPY tests/opi_server/requirements.txt tests/opi_server/requirements.txt
RUN pip install --no-cache-dir -r tests/opi_server/requirements.txt
COPY proto proto
COPY tests/opi_server tests/opi_server
ENV PYTHONUNBUFFERED=1
EXPOSE 50051
CMD ["python", "-u", "tests/opi_server/server.py", "--port", "50051"]
