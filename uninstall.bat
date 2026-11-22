@echo off
:: Clic droit > Executer en tant qu'administrateur
regsvr32 /u "%ProgramFiles%\WaveXLR-ASIO\WaveXlrAsio.dll"
del /q "%ProgramFiles%\WaveXLR-ASIO\WaveXlrAsio.dll"
