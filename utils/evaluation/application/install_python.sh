#!/bin/bash
# install_python.sh [DEST] -- the Python that Fig. 11's A2, A4, A5 and A6 run
# under, in a venv at DEST (default /opt/splinefs-ae-py), with the package
# versions of the paper's runs.  scripts/campaigns/run_applookup.sh finds it
# there.  Needs python3-venv and network; honours http_proxy and https_proxy.
set -euo pipefail
DEST=${1:-/opt/splinefs-ae-py}
python3 -m venv "$DEST"
"$DEST/bin/pip" install -q --upgrade pip
# The CPU build: the default torch wheel pulls in CUDA libraries A2 never uses.
"$DEST/bin/pip" install -q torch==2.9.0 --index-url https://download.pytorch.org/whl/cpu
"$DEST/bin/pip" install -q pyarrow==24.0.0 duckdb==1.5.3 llama-index-core==0.14.22
"$DEST/bin/python" -c 'import torch, pyarrow, duckdb, llama_index.core'
echo "ready: $DEST/bin/python"
