@echo off
REM Ubuntu Touch (Halium 16) installer for OnePlus Nord N30 5G (larry)
REM Usage: flash-larry.bat a     (or b - your ACTIVE slot: adb shell getprop ro.boot.slot_suffix)
REM Run from the folder containing boot.img, vendor_boot.img, dtbo.img, vbmeta.img, ubuntu.img.0 ... ubuntu.img.8
REM Phone must already be in bootloader mode (adb reboot bootloader).

setlocal
set SLOT=%1
if "%SLOT%"=="" (
  echo Pass your active slot: flash-larry.bat a   or   flash-larry.bat b
  exit /b 1
)
for %%F in (boot.img vendor_boot.img dtbo.img vbmeta.img ubuntu.img.0 ubuntu.img.8) do (
  if not exist %%F ( echo Missing %%F & exit /b 1 )
)

echo.
echo This ERASES ALL DATA on the phone and removes Android (product/system_ext).
echo Make sure you backed up the stock boot, vendor_boot, dtbo and vbmeta images.
echo Target slot: _%SLOT%
pause

fastboot devices
fastboot --disable-verity --disable-verification flash vbmeta_%SLOT% vbmeta.img || goto :fail
fastboot flash boot_%SLOT% boot.img || goto :fail
fastboot flash vendor_boot_%SLOT% vendor_boot.img || goto :fail
fastboot flash dtbo_%SLOT% dtbo.img || goto :fail
fastboot format:ext4 userdata || goto :fail

echo Rebooting to fastbootd...
fastboot reboot fastboot || goto :fail
timeout /t 15 >nul
fastboot getvar is-userspace

fastboot delete-logical-partition product_%SLOT%
fastboot delete-logical-partition system_ext_%SLOT%
fastboot resize-logical-partition system_%SLOT% 4718592000 || goto :fail
for %%N in (0 1 2 3 4 5 6 7 8) do (
  echo Flashing rootfs chunk %%N of 8
  fastboot flash system_%SLOT% ubuntu.img.%%N || goto :fail
)

echo.
echo Done. Rebooting into Ubuntu Touch (first boot can take a few minutes).
fastboot reboot
exit /b 0

:fail
echo.
echo A fastboot step failed - stop here and check the message above.
echo If fastbootd refuses to resize/delete partitions with a snapshot error, run:
echo   fastboot snapshot-update cancel
echo and run this script again.
exit /b 1
