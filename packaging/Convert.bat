@echo off
cd /d "%~dp0"
echo ============================================
echo   HEIC to PDF Converter
echo ============================================
echo.
echo Converting every .heic photo in the
echo "photos to convert" folder...
echo.
heic_converter_mt.exe "photos to convert"
echo.
echo Done. Your .pdf files are in the "output" folder.
echo.
pause
