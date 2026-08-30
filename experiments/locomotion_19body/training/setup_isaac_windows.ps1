param(
    [string]$VenvPath = ".venv-isaac"
)

$ErrorActionPreference = "Stop"

Write-Host "Asterra Isaac Lab training environment"
Write-Host "Virtual environment: $VenvPath"

$py = Get-Command py -ErrorAction SilentlyContinue
if ($null -eq $py) {
    throw "Python launcher 'py' was not found. Install Python 3.11 first."
}

& py -3.11 -m venv $VenvPath
$python = Join-Path $VenvPath "Scripts\python.exe"

& $python -m pip install --upgrade pip
& $python -m pip install "isaaclab[isaacsim,all]==2.3.1" --extra-index-url https://pypi.nvidia.com
& $python -m pip install --upgrade torch==2.7.0 torchvision==0.22.0 --index-url https://download.pytorch.org/whl/cu128

Write-Host ""
Write-Host "Environment installed. Verify with:"
Write-Host "$python experiments/locomotion_19body/training/check_training_stack.py"
