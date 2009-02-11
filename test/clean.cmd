@echo off

if exist "ubertest.php" (
	for /D %%i in (test_*) do (
		if exist "%%i\test.xml" (
			del /q "%%i\report.txt" 2>nul
			rmdir /s /q "%%i\Conf" 2>nul
		)
	)

	del /q "data\*.sp*" 2>nul
	del /q "*.log" 2>nul

	del /q "error*.txt" 2>nul
	del /q "config*.conf" 2>nul
)
