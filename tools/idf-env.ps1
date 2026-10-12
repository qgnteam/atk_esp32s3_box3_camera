param([string]$IdfPath = 'D:\esp\v6.1\esp-idf',
      [string]$ToolsPath = 'C:\Espressif\tools')
# 本机工具链入口；路径可通过参数替换。组件管理器下载 manifest 锁定的依赖。
$env:IDF_PATH = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath
$env:IDF_PYTHON_ENV_PATH = Join-Path $ToolsPath 'python\v6.1\venv'
$env:IDF_COMPONENT_MANAGER = '1'
$env:ESP_IDF_VERSION = '6.1'
$env:PYTHONUTF8 = '1'
$env:IDF_CCACHE_ENABLE = '0'
$env:ESP_ROM_ELF_DIR = Join-Path $ToolsPath 'esp-rom-elfs\20241011'
$bins = @(
    (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts'),
    (Join-Path $ToolsPath 'xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin'),
    (Join-Path $ToolsPath 'cmake\4.0.3\bin'),
    (Join-Path $ToolsPath 'ninja\1.12.1')
)
$env:PATH = ($bins -join ';') + ';' + $env:PATH
function Invoke-CameraIdf {
    & (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe') (Join-Path $env:IDF_PATH 'tools\idf.py') @args
}
