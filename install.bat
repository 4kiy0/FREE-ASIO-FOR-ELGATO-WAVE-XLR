@echo off
:: Clic droit > Executer en tant qu'administrateur
cd /d "%~dp0"
if not exist "%ProgramFiles%\WaveXLR-ASIO" mkdir "%ProgramFiles%\WaveXLR-ASIO"
copy /y WaveXlrAsio.dll "%ProgramFiles%\WaveXLR-ASIO\" >nul
regsvr32 "%ProgramFiles%\WaveXLR-ASIO\WaveXlrAsio.dll"
