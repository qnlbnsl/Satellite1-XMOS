# x86 image for the VNR model check: gcc + cmake for the harness, and two
# venvs with the xmos-ai-tools releases the old and the new firmware converted
# the VNR model with (1.3.1: fwk_voice era requirements.txt; 1.4.3.dev40: now).
FROM gcc:13
RUN apt-get update -qq && apt-get install -y -qq cmake python3-venv python-is-python3 python3-numpy >/dev/null && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /venv/old && /venv/old/bin/pip install -q 'xmos-ai-tools==1.3.1' 'numpy<2'
RUN python3 -m venv /venv/new && /venv/new/bin/pip install -q 'xmos-ai-tools==1.4.3.dev40' 'numpy<2'
