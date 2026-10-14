# SPDX-License-Identifier: Apache-2.0
# Lab dashorch: APPL_DB DASH_* → ASIC_DB work queue (SAI C-API ops).
# Replace with docker-orchagent / sonic-swss when a DASH-enabled image is on hand.
FROM python:3.12-slim-bookworm
WORKDIR /opt/dashorch
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY orch.py .
ENV PYTHONUNBUFFERED=1
ENV REDIS_HOST=redis-appl-db
CMD ["python", "-u", "orch.py"]
