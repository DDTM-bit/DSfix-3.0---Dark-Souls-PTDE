@echo off
setlocal enabledelayedexpansion

:: Define the file extensions you want to read (space-separated)
set "extensions=.def .h .cpp .config"

:: Define the name of the final output file
set "output_file=Combined_Output.txt"

:: Clear the output file if it already exists to avoid duplicate appends
if exist "%output_file%" del "%output_file%"

:: Loop through each extension in your list
for %%e in (%extensions%) do (
    :: Loop through all files matching that extension in the current folder
    for %%f in (*%%e) do (
        :: Skip the output file itself just in case it shares an extension
        if not "%%f"=="%output_file%" (
            echo File: %%f:>> "%output_file%"
            type "%%f">> "%output_file%"
            echo.>> "%output_file%"
            echo.>> "%output_file%"
        )
    )
)

echo Done! All matching text has been saved to %output_file%.
pause