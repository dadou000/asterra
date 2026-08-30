param(
    [string]$VenvPath = ".venv-isaac"
)

$ErrorActionPreference = "Stop"

function Assert-LastCommandSucceeded {
    param([string]$Step)
    if ($LASTEXITCODE -ne 0) {
        throw "$Step failed with exit code $LASTEXITCODE."
    }
}

Write-Host "Asterra Isaac Lab training environment"
Write-Host "Virtual environment: $VenvPath"

$py = Get-Command py -ErrorAction SilentlyContinue
if ($null -eq $py) {
    throw "Python launcher 'py' was not found. Install Python 3.11 first."
}

& py -3.11 -m venv $VenvPath
Assert-LastCommandSucceeded "Python 3.11 virtual environment creation"
$python = Join-Path $VenvPath "Scripts\python.exe"

& $python -m pip install --upgrade pip
Assert-LastCommandSucceeded "pip upgrade"
& $python -m pip install "setuptools==80.9.0" "wheel==0.42.0"
Assert-LastCommandSucceeded "packaging tool installation"
# flatdict 4.0.1 imports pkg_resources while determining build requirements.
# Installing it without isolation keeps that legacy build on the compatible
# setuptools version above instead of the latest isolated build environment.
& $python -m pip install "flatdict==4.0.1" --no-build-isolation
Assert-LastCommandSucceeded "flatdict compatibility installation"
& $python -m pip install "isaaclab[isaacsim,all]==2.3.1" --extra-index-url https://pypi.nvidia.com
Assert-LastCommandSucceeded "Isaac Lab installation"
& $python -m pip install --upgrade torch==2.7.0 torchvision==0.22.0 --index-url https://download.pytorch.org/whl/cu128
Assert-LastCommandSucceeded "CUDA PyTorch installation"

# RSL-RL 3.0.1 declares tensordict>=0.7 without an upper bound. TensorDict
# 0.12.x has a known Windows/Python 3.11 native import crash when loaded after
# Isaac Sim 5.1. Pin the last known-good release and do not let pip touch the
# already validated Torch/Isaac dependency set while applying the workaround.
& $python -m pip install --force-reinstall --no-deps "tensordict==0.11.0"
Assert-LastCommandSucceeded "TensorDict Windows compatibility pin"

Write-Host ""
Write-Host "Environment installed. Verify with:"
Write-Host "$python experiments/locomotion_19body/training/check_training_stack.py"
